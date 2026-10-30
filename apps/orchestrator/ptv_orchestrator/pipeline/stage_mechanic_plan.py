"""Stage 3: BuildSpec + AssetManifest -> MechanicPlan (OpenAI Agents SDK).

The planner is an agent (gpt-5.6-sol, no tools), not a single LLM call.
It is reuse-first: at runtime we append the engine catalog (built-in
mechanics + mechanics promoted from past games) so the agent classifies
each mechanic against that catalog before flagging anything as "generate".

Classification is four-way:

* ``builtin``                    -- a catalog entry satisfies as-is.
* ``builtin+config``             -- satisfied by parameter tuning only.
* ``builtin+config_code_edit``   -- a catalog entry is the right foundation
  but params cannot satisfy the prompt; the code stage copies the built-in's
  source and edits it into a NEW generated mechanic.
* ``generate``                   -- a brand-new mechanic.

The agent also assigns a controller binding to every input-driven mechanic;
a deterministic validator (``binding_validator``) then normalizes
face-button hands (A/B -> right, X/Y -> left) and emits advisory warnings
before the plan is saved. Sharing a control across mechanics is allowed and
is not rejected.

See ``apps/orchestrator/ptv_orchestrator/engine_catalog.py`` for the catalog
source of truth and ``builtin_source.py`` for the code-edit source resolver.
"""

from __future__ import annotations

import os
from typing import Optional

from ptv_schemas import MechanicClassification, MechanicPlan, ProjectStage, ProjectStatus

from ..engine_catalog import as_planner_text
from .binding_validator import validate_bindings
from .builtin_source import editable_builtins_note
from .context import PipelineContext, StageResult
from .game_flow_plan import reconcile_game_flow_items
from .interaction_rules import design_excerpt
from .weapon_plan import reconcile_weapon_items
from .mechanic_plan_agent import (
    MechanicPlanAgentInput,
    MechanicPlanner,
    build_planner_message,
)
from .movement import reconcile_mechanic_plan, required_roles
from .npc_population import reconcile_npc_ai_items, reconcile_npc_mechanics
from .prompts import mechanic_plan_system


def run(ctx: PipelineContext) -> StageResult:
    """Standard stage entry point."""
    return run_with_planner(ctx, planner=None)


def run_with_planner(
    ctx: PipelineContext, planner: Optional[MechanicPlanner] = None
) -> StageResult:
    """Run the mechanic-plan stage with an optional injected planner (tests
    pass a stub so the OpenAI Agents SDK is never touched)."""

    ctx.emit(ProjectStage.mechanic_plan, "running", "Planning mechanics")
    state = ctx.store.load_state(ctx.project_id)
    ctx.store.record_stage(state, ProjectStage.mechanic_plan, ProjectStatus.running)

    build_spec = ctx.build_spec or ctx.store.load_build_spec(ctx.project_id)
    manifest = ctx.asset_manifest or ctx.store.load_asset_manifest(ctx.project_id)

    catalog_text = as_planner_text(ctx.settings.repo_root)

    # Bridge OPENAI_API_KEY into os.environ for the Agents SDK (BaseSettings
    # does not export it automatically).
    if ctx.settings.openai_api_key and not os.environ.get("OPENAI_API_KEY"):
        os.environ["OPENAI_API_KEY"] = ctx.settings.openai_api_key

    if planner is None:
        planner = MechanicPlanner(
            instructions=mechanic_plan_system(state.target_device),
            model=ctx.settings.mechanic_plan_model,
            reasoning_effort=ctx.settings.mechanic_plan_reasoning_effort,
            verbosity=ctx.settings.mechanic_plan_verbosity,
            max_turns=ctx.settings.mechanic_plan_agent_max_turns,
            session_id=f"ptv-mechanic-plan-{ctx.project_id}",
            max_retries=ctx.settings.mechanic_plan_max_retries,
            log=lambda text: ctx.store.append_log(
                ctx.project_id, "mechanic_plan", text
            ),
        )

    npc_plan = ctx.npc_plan or ctx.store.load_npc_plan(ctx.project_id)
    design = ctx.game_design or ctx.store.load_game_design(ctx.project_id)
    loop_rule = (
        "GAME LOOP: this game's rules are decided (game_design.json below). "
        "Lives, respawn, health reaching zero, objectives, win/lose, the "
        "Game Over / victory / end screens, Try Again / Play Again, the pause "
        "menu and Leave Game are builtin.game_flow, and the health / lives / "
        "timer / objective HUD is builtin.game_hud -- the scene compiler binds "
        "both. Plan any BuildSpec mechanic that IS one of those as builtin+config "
        "of that built-in; NEVER generate a run flow, lives, respawn, results "
        "screen, restart or quit. A game-specific objective is generated code "
        "that calls ctx.game->add_progress / complete / fail. game_design.json:\n"
        + design.model_dump_json(indent=2)
        if design is not None else
        "GAME LOOP: no game design for this project."
    )
    interaction = (ctx.interaction_design
                   or ctx.store.load_interaction_design(ctx.project_id))
    excerpt = design_excerpt(interaction)
    weapon_rule = (
        "WEAPONS: how every item is held and fires is decided (interaction "
        "design below). Firing, ammo, reload, recoil, tracers, projectiles, "
        "beams, muzzle flash, hit detection and weapon damage are "
        "builtin.weapon; bows are builtin.bow (+ builtin.item_source for the "
        "quiver); swords / hammers / axes are builtin.melee_weapon; shields "
        "are builtin.shield; NPCs that shoot are builtin.npc.ranged_attack. "
        "The scene compiler binds all of them from the design. Plan NO "
        "shooting hub, fire profile, ammo / reload, recoil, projectile or "
        "melee-hit mechanic, and no builtin.recoil. Game-specific reactions "
        "(a target that scores, a HUD) are generated code that READS "
        "ctx.weapons->hits_since / state. Items:\n" + excerpt
        if excerpt else
        "WEAPONS: no interaction design; no weapons are designed."
    )
    inp = MechanicPlanAgentInput(
        build_spec_json=build_spec.model_dump_json(indent=2),
        asset_manifest_json=manifest.model_dump_json(indent=2),
        npc_plan_json=(
            npc_plan.model_dump_json(indent=2) if npc_plan.npcs else ""
        ),
        catalog_text=catalog_text,
        editable_builtins_note=editable_builtins_note(),
        extra_instructions=[
            "Emit one MechanicPlanItem per BuildSpec mechanic (1:1 by id).",
            "Reuse catalog entries (built-in OR promoted) whenever the request"
            " is covered; escalate params -> code_edit -> generate only as"
            " needed.",
            "For hub-type BuildSpec mechanics, generate the hub and set"
            " depends_on so branch mechanics are built after it.",
            "PLAYER ROLES this game needs (plan exactly one item for each,"
            " as builtin / builtin+config of the listed built-in, and plan"
            " NO other locomotion, turning, finger-posing, avatar-body or"
            " movement-menu mechanics): "
            + ", ".join(required_roles(build_spec)),
            "NPC POPULATION: spawning, waves, pools and spawn loops belong to"
            " builtin.encounter_director (added automatically when the NPC"
            " plan has encounters) -- plan NO mechanic that spawns or pools"
            " NPCs. Map 'follow the player' to builtin.npc.follow and"
            " chase + melee attack to builtin.npc.melee_chase; generate code"
            " only for NPC behaviour those cannot express (ranged attacks,"
            " flight, special abilities), and for wave HUDs that READ"
            " ctx.encounters.",
            loop_rule,
            weapon_rule,
        ],
    )

    message = build_planner_message(inp)
    plan: MechanicPlan = planner.run(message)

    # Deterministic controller-binding normalization (normalizes face-button
    # hands in place; emits advisory warnings only -- sharing a control is
    # allowed, so this does not reject the plan).
    binding_result = validate_bindings(plan)
    for warn in binding_result.warnings:
        ctx.emit(ProjectStage.mechanic_plan, "warn", warn[:240])
    if not binding_result.ok:
        detail = "; ".join(binding_result.errors)
        ctx.store.record_stage(
            state,
            ProjectStage.mechanic_plan,
            ProjectStatus.failed,
            message=f"controller binding conflicts: {detail}",
        )
        ctx.emit(ProjectStage.mechanic_plan, "failed", detail[:240])
        raise ValueError(f"MechanicPlan binding conflicts: {detail}")

    # Player roles are decided by the BuildSpec, not by the planner: exactly
    # one mechanic per role the game needs (fingers always, the body for
    # Full Player Avatar, each allowed locomotion mode and turn style, the
    # settings menu when there is a choice), none for roles it does not.
    reconciled = reconcile_mechanic_plan(plan, build_spec)
    plan = reconciled.plan
    for note in reconciled.notes:
        ctx.emit(ProjectStage.mechanic_plan, "info", f"player roles: {note}"[:240])
        ctx.store.append_log(ctx.project_id, "mechanic_plan",
                             f"player roles: {note}")

    # The NPC population runs on engine built-ins (spawning, following,
    # chasing). Make sure the plan carries the ones the NPC plan needs.
    plan, npc_notes = reconcile_npc_mechanics(plan, npc_plan)
    # Generated AI for an NPC a built-in already fully covers is config.
    plan, covered_notes = reconcile_npc_ai_items(plan, npc_plan)
    npc_notes = list(npc_notes) + covered_notes
    for note in npc_notes:
        ctx.emit(ProjectStage.mechanic_plan, "info", f"NPCs: {note}"[:240])
        ctx.store.append_log(ctx.project_id, "mechanic_plan", f"NPCs: {note}")

    # The game loop runs on builtin.game_flow / builtin.game_hud.
    plan, flow_notes = reconcile_game_flow_items(plan, design)
    for note in flow_notes:
        ctx.emit(ProjectStage.mechanic_plan, "info", f"game loop: {note}"[:240])
        ctx.store.append_log(ctx.project_id, "mechanic_plan", f"game loop: {note}")

    # Weapons run on builtin.weapon / bow / melee_weapon / shield.
    plan, weapon_notes = reconcile_weapon_items(plan, interaction, npc_plan)
    for note in weapon_notes:
        ctx.emit(ProjectStage.mechanic_plan, "info", f"weapons: {note}"[:240])
        ctx.store.append_log(ctx.project_id, "mechanic_plan", f"weapons: {note}")

    ctx.mechanic_plan = plan
    art = ctx.store.save_mechanic_plan(ctx.project_id, plan)

    # Emit a reuse-vs-generate summary so the UI can highlight engine growth.
    counts: dict[str, int] = {}
    for it in plan.items:
        key = it.classification.value
        counts[key] = counts.get(key, 0) + 1
    gen_count = counts.get("generate", 0)
    code_edit_count = counts.get(
        MechanicClassification.builtin_config_code_edit.value, 0
    )
    reuse_count = counts.get("builtin", 0) + counts.get("builtin+config", 0)
    # code_edit and generate both produce a CodeTask.
    codegen_count = gen_count + code_edit_count
    summary = (
        f"reuse={reuse_count} (builtin/builtin+config), "
        f"code_edit={code_edit_count}, generate={gen_count}"
    )
    ctx.emit(ProjectStage.mechanic_plan, "info", summary)

    ctx.store.record_stage(
        state,
        ProjectStage.mechanic_plan,
        ProjectStatus.succeeded,
        message=f"{codegen_count} mechanics require generated C++ ({summary})",
        artifacts=[art.path],
    )
    ctx.emit(
        ProjectStage.mechanic_plan,
        "succeeded",
        f"{codegen_count} to generate, {reuse_count} reused",
    )
    return StageResult(stage=ProjectStage.mechanic_plan, artifacts=[art.path], data=plan)
