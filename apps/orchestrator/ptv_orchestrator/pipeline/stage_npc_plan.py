"""Stage 2b: decide what every NPC is supposed to DO, before anything is built.

Placing a goblin model in a scene does not produce an enemy. The prompt says
what these characters are FOR, and until now nothing read that intent as a
whole -- every downstream stage re-derived it from the same prose and they
disagreed with each other. The manifest would rig a duck that the codegen
agent then wrote as a static slider, and the rig was billed for nothing.

Why it runs HERE, between ``asset_manifest`` and ``mechanic_plan``:

* Before ``asset_rigging`` / ``asset_animation``, because "does this thing
  need animation at all" is the most expensive question in the pipeline
  (~30 credits per rig, ~20 per clip) and only the prompt can answer it.
* Before ``mechanic_plan`` and ``scene_spec``, because both need to know
  which characters are combatants, what factions exist and what behaviors
  the generated C++ has to cover.

The stage also RECONCILES the asset manifest with its own decision, so there
is one source of truth rather than two that drift. If the director says the
ducks slide, the manifest's ``rigged``/``animations`` are cleared and the
rigging stage never sees them.
"""

from __future__ import annotations

import os
from typing import Optional

from ptv_schemas import (
    AssetCategory,
    MovementStyle,
    NpcPlan,
    ProjectStage,
    ProjectStatus,
)
from ptv_schemas.asset_manifest import AssetItem, CharacterRole

from .context import PipelineContext, StageResult
from .npc_population import npc_plan_violations, repair_npc_plan
from .npc_plan_agent import (
    NpcDirector,
    NpcPlanAgentError,
    NpcPlanAgentInput,
    build_director_message,
)
from .prompts import NPC_PLAN_SYSTEM

_STAGE = ProjectStage.npc_plan

# Categories that can plausibly be an autonomous actor. Anything else is
# scenery as far as the director is concerned, and an entry naming one is a
# mistake worth dropping rather than acting on.
_ACTOR_CATEGORIES = {AssetCategory.character}


def _is_actor_candidate(item: AssetItem) -> bool:
    """Could this asset be a NON-player character?

    Category alone is not the answer, and using it alone was a bug: the
    player's own body is ``category: character`` too, so a first-person game
    with no NPCs at all still looked like it had one. That spent an agent turn
    to be told what the manifest already said, and it defeated the empty-plan
    short circuit below that exists for exactly this case.

    ``character_role`` is the field that separates the two, and it is why that
    field exists -- see its docstring in ``ptv_schemas.asset_manifest``.

    Only ``player_avatar`` is excluded, deliberately: a character whose role is
    ``none`` is still a candidate, because the role is set by an upstream agent
    and an unset one must not silently remove a real enemy from the plan.
    """
    if item.category not in _ACTOR_CATEGORIES:
        return False
    return item.character_role != CharacterRole.player_avatar


def run(ctx: PipelineContext) -> StageResult:
    return run_with_director(ctx, director=None)


def run_with_director(
    ctx: PipelineContext, director: Optional[NpcDirector] = None
) -> StageResult:
    ctx.emit(_STAGE, "running", "Planning NPC roles and behaviour")
    state = ctx.store.load_state(ctx.project_id)
    ctx.store.record_stage(state, _STAGE, ProjectStatus.running)

    build_spec = ctx.build_spec or ctx.store.load_build_spec(ctx.project_id)
    manifest = ctx.asset_manifest or ctx.store.load_asset_manifest(
        ctx.project_id
    )

    candidates = [i for i in manifest.items if _is_actor_candidate(i)]
    if not candidates:
        # A book-reading experience, a puzzle game, or a first-person shooting
        # range whose only character is the player's own avatar has no NPCs,
        # and that is a complete answer -- not a reason to spend an agent turn.
        plan = NpcPlan(prompt_id=build_spec.prompt_id)
        art = ctx.store.save_npc_plan(ctx.project_id, plan)
        ctx.npc_plan = plan
        # Says WHICH of the two reasons applies, because "no NPC assets" on a
        # game that clearly has a character on screen reads as a bug.
        avatars = sum(
            1
            for i in manifest.items
            if i.character_role == CharacterRole.player_avatar
        )
        message = (
            "only the player's own avatar; empty NPC plan"
            if avatars
            else "no character assets; empty NPC plan"
        )
        ctx.store.record_stage(
            state, _STAGE, ProjectStatus.succeeded, message=message,
            artifacts=[art.path],
        )
        ctx.emit(_STAGE, "succeeded", message)
        return StageResult(stage=_STAGE, artifacts=[art.path])

    # Bridge OPENAI_API_KEY into os.environ for the Agents SDK (BaseSettings
    # does not export it automatically), mirroring the mechanic_plan /
    # scene_spec / world / codegen stages.
    #
    # This stage NEEDS it more than they do, not less: it is the first
    # Agents-SDK stage in a run, so on a cold orchestrator nothing has
    # populated os.environ yet. Missing here, the NPC director failed on every
    # fresh run and only appeared to work once a later stage had set the key
    # for the rest of the process -- which made a deterministic bug look
    # intermittent.
    if ctx.settings.openai_api_key and not os.environ.get("OPENAI_API_KEY"):
        os.environ["OPENAI_API_KEY"] = ctx.settings.openai_api_key

    if director is None:
        director = NpcDirector(
            instructions=NPC_PLAN_SYSTEM,
            model=ctx.settings.npc_plan_model,
            reasoning_effort=ctx.settings.npc_plan_reasoning_effort,
            verbosity=ctx.settings.npc_plan_verbosity,
            max_turns=ctx.settings.npc_plan_agent_max_turns,
            session_id=f"ptv-npc-plan-{ctx.project_id}",
            max_retries=ctx.settings.npc_plan_max_retries,
            log=lambda text: ctx.store.append_log(
                ctx.project_id, "npc_plan", text
            ),
        )

    inp = NpcPlanAgentInput(
        prompt_text=_read_prompt(ctx),
        build_spec_json=build_spec.model_dump_json(indent=2),
        asset_manifest_json=manifest.model_dump_json(indent=2),
        extra_instructions=[
            "Character assets in this manifest: "
            + ", ".join(i.id for i in candidates),
            "Emit at most one NpcSpec per asset id, and only for assets that "
            "are autonomous actors.",
        ],
    )

    try:
        plan = director.run(build_director_message(inp))
    except NpcPlanAgentError as exc:
        # A failure here must not sink the run: without a plan the pipeline
        # behaves exactly as it did before this stage existed.
        message = f"NPC director unavailable ({exc}); continuing without a plan"
        plan = NpcPlan(prompt_id=build_spec.prompt_id)
        ctx.emit(_STAGE, "warning", message[:240])
        ctx.store.append_log(ctx.project_id, "npc_plan", message)
    else:
        plan = _drop_unknown_assets(ctx, plan, {i.id for i in candidates})
        # The population has rules the engine depends on (every roster
        # variant planned, companions placed once, waves naming real types).
        # Ask once for a fix, then repair what is left deterministically.
        problems = npc_plan_violations(plan, build_spec)
        if problems:
            ctx.emit(_STAGE, "warning",
                     "NPC plan needs revising: " + "; ".join(problems)[:600])
            try:
                revised = director.run(
                    build_director_message(inp)
                    + "\n\nYour previous NpcPlan had these problems; return the"
                    " COMPLETE plan with all of them fixed:\n"
                    + "\n".join(f"- {p}" for p in problems)
                )
            except NpcPlanAgentError as exc:
                ctx.store.append_log(ctx.project_id, "npc_plan",
                                     f"revision failed: {exc}")
            else:
                plan = _drop_unknown_assets(ctx, revised, {i.id for i in candidates})
        plan, repairs = repair_npc_plan(plan, build_spec)
        for note in repairs:
            ctx.emit(_STAGE, "info", note)

    art = ctx.store.save_npc_plan(ctx.project_id, plan)
    ctx.npc_plan = plan

    changed = _reconcile_manifest(ctx, plan)
    if changed:
        ctx.store.save_asset_manifest(ctx.project_id, manifest)
        ctx.asset_manifest = manifest

    animated = sum(
        1
        for n in plan.npcs
        if n.movement_style != MovementStyle.slide_no_animation
        and n.animation_intents
    )
    clips = sum(len(n.animation_intents) for n in plan.npcs)
    message = (
        f"{len(plan.npcs)} NPC type(s), {animated} animated, {clips} clip(s) "
        "to retarget"
    )
    if plan.encounters is not None:
        message += (
            f"; {len(plan.encounters.waves)} wave(s) of "
            f"{len(plan.spawned_asset_ids())} type(s)"
        )
    ctx.store.record_stage(
        state, _STAGE, ProjectStatus.succeeded, message=message,
        artifacts=[art.path],
    )
    ctx.emit(_STAGE, "succeeded", message)
    return StageResult(stage=_STAGE, artifacts=[art.path])


def _read_prompt(ctx: PipelineContext) -> str:
    """The user's own words.

    Passed verbatim rather than relying on the BuildSpec because the
    decisions here hinge on phrasing a summary discards -- "the ducks should
    just slide, no animations" is exactly the instruction that does not
    survive being condensed into a mechanic description.
    """
    path = ctx.project_dir / "prompt.txt"
    if not path.exists():
        return "(original prompt unavailable)"
    return path.read_text(encoding="utf-8")[:20_000]


def _drop_unknown_assets(
    ctx: PipelineContext, plan: NpcPlan, known: set[str]
) -> NpcPlan:
    """Remove entries naming assets that do not exist.

    A hallucinated asset id would otherwise reach the scene compiler as an
    NPC with no mesh, which fails much later and much less clearly.
    """
    kept = [n for n in plan.npcs if n.asset_id in known]
    dropped = [n.asset_id for n in plan.npcs if n.asset_id not in known]
    for asset_id in dropped:
        ctx.emit(
            _STAGE,
            "warning",
            f"dropped NPC entry for unknown asset '{asset_id}'",
        )
    if not dropped:
        return plan
    return plan.model_copy(update={"npcs": kept})


def _reconcile_manifest(ctx: PipelineContext, plan: NpcPlan) -> bool:
    """Make the asset manifest agree with the NPC plan.

    Two artifacts describing the same decision will eventually disagree, and
    when they do the pipeline pays real money for the difference: a manifest
    that still says ``rigged: true`` for a duck the plan wants to slide costs
    a 30-credit rig plus a clip per animation, for output nothing will ever
    play. The plan is the authority because it is the stage that actually
    read the prompt's intent.

    Returns True when the manifest was modified.
    """
    if not plan.npcs:
        return False

    by_asset = {n.asset_id: n for n in plan.npcs}
    manifest = ctx.asset_manifest or ctx.store.load_asset_manifest(
        ctx.project_id
    )
    changed = False

    for item in manifest.items:
        npc = by_asset.get(item.id)
        if npc is None:
            continue

        wants_animation = (
            npc.movement_style != MovementStyle.slide_no_animation
            and bool(npc.animation_intents)
        )

        if not wants_animation:
            if item.rigged or item.animations:
                ctx.emit(
                    _STAGE,
                    "info",
                    f"{item.id}: {npc.movement_style.value}; skipping rig and "
                    "animation",
                )
                item.rigged = False
                item.animations = []
                changed = True
            continue

        if not item.rigged:
            item.rigged = True
            changed = True
        if list(item.animations) != list(npc.animation_intents):
            item.animations = list(npc.animation_intents)
            changed = True

    return changed
