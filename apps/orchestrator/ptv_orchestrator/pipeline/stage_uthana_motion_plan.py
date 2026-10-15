"""Stage 9b: plan every Uthana motion each Uthana-animated character needs.

Nothing is generated here. The stage only decides, so the plan can be read
and costed before a second of motion is billed:

1. Measure each Uthana-rigged GLB (Part 3A): ``height_for_motion_m`` is the
   height Uthana animates the character at, and ``engine_scale`` is how much
   the engine will resize it.
2. Query Uthana's locomotion styles (live list).
3. Ask the motion-plan agent for every locomotion motion's style, strides,
   speed and angle -- walk, run and any ``<gait>.<variant>`` -- from the
   character's role and the routing decision.
4. Check and correct the answer in code: a known style, strides 1-5, angle 0,
   speed = the engine's factor x height (0.78 walk, 2.2 run) within 5%,
   clamped to Uthana's 0.25-8.0 m/s, rounded to one decimal, and the
   canonical name ``style-str{N}-ms{speed}-deg{angle}``.
5. Add the shared library idle (``settings.uthana_idle_motion_id``) to every
   character. The agent never plans an idle; code verifies once that the
   idle motion exists in the organisation.
"""

from __future__ import annotations

import json
import os
from typing import Any, Optional

from ptv_providers import UthanaClient
from ptv_providers.animation_catalog import (
    RUN_SPEED_PER_M,
    UTHANA_MAX_MOVE_SPEED,
    UTHANA_MIN_MOVE_SPEED,
    UTHANA_STYLES_FALLBACK,
    WALK_SPEED_PER_M,
)
from ptv_schemas import (
    CharacterRoute,
    MotionSource,
    PlannedLocomotion,
    ProjectStage,
    ProjectStatus,
    RigProvider,
    UthanaCharacterMotionPlan,
    UthanaMotionAgentOutput,
    UthanaMotionPlan,
    UthanaMotionSpec,
)

from .character_artifacts import (
    UTHANA_RIG_NAME,
    UTHANA_RIGGED_GLB,
    load_json,
    load_routing,
    rel,
    save_motion_plan,
)
from .context import PipelineContext, StageResult
from .prompts import UTHANA_MOTION_PLAN_SYSTEM
from .structured_agent import AgentBrief, StructuredAgent, StructuredAgentError
from .uthana.glb_measure import measure
from .uthana.motion_names import format_locomotion_name, round_speed

_STAGE = ProjectStage.uthana_motion_plan

#: How far the agent's speed may stray from factor x height before code
#: replaces it.
SPEED_TOLERANCE = 0.05
DEFAULT_STYLE = "neutral_non_gendered"


def run(ctx: PipelineContext) -> StageResult:
    return run_with(ctx)


def run_with(
    ctx: PipelineContext,
    *,
    client: Any = None,
    agent: Optional[StructuredAgent] = None,
) -> StageResult:
    ctx.emit(_STAGE, "running", "Planning Uthana motions")
    state = ctx.store.load_state(ctx.project_id)
    ctx.store.record_stage(state, _STAGE, ProjectStatus.running)

    routing = load_routing(ctx.project_dir)
    rig_ledger = load_json(ctx.project_dir / UTHANA_RIG_NAME)
    manifest = ctx.asset_manifest or ctx.store.load_asset_manifest(ctx.project_id)
    by_id = {i.id: i for i in manifest.items}
    build_spec = ctx.build_spec or ctx.store.load_build_spec(ctx.project_id)

    targets: list[CharacterRoute] = [
        r for r in (routing.routes if routing else [])
        if r.effective_animation_provider() == RigProvider.uthana
        and isinstance(rig_ledger.get(r.asset_id), dict)
        and rig_ledger[r.asset_id].get("status") == "rigged"
    ]
    if not targets:
        plan = UthanaMotionPlan(prompt_id=build_spec.prompt_id)
        art = save_motion_plan(ctx.project_dir, plan)
        message = "No Uthana-animated characters"
        ctx.store.record_stage(state, _STAGE, ProjectStatus.succeeded, message=message,
                               artifacts=[rel(ctx.settings.repo_root, art)])
        ctx.emit(_STAGE, "succeeded", message)
        return StageResult(stage=_STAGE, data=plan)

    if client is None:
        client = UthanaClient(ctx.settings.uthana_api_key)

    styles = _styles(ctx, client)
    idle_id = ctx.settings.uthana_idle_motion_id
    idle = client.get_motion(idle_id)
    if idle is None or not idle.name:
        raise RuntimeError(
            f"The library idle motion {idle_id!r} was not found in your Uthana "
            "organisation. Set UTHANA_IDLE_MOTION_ID to an existing idle motion."
        )

    characters: list[UthanaCharacterMotionPlan] = []
    for route in targets:
        item = by_id.get(route.asset_id)
        glb = ctx.project_dir / "assets" / route.asset_id / UTHANA_RIGGED_GLB
        target = float(item.target_scale_meters) if item else 1.8
        report = measure(glb, target)
        characters.append(UthanaCharacterMotionPlan(
            asset_id=route.asset_id,
            character_id=str(rig_ledger[route.asset_id]["character_id"]),
            character_role=route.character_role,
            height_for_motion_m=round(report.height_for_motion_m, 4),
            target_scale_meters=target,
            engine_scale=report.engine_scale or (target / report.bind_raw_longest_axis),
            notes=list(report.notes),
        ))

    answers = _ask_agent(ctx, agent, characters, targets, by_id, styles)
    route_by_id = {r.asset_id: r for r in targets}
    for c in characters:
        c.motions = plan_character_motions(
            route_by_id[c.asset_id], c.height_for_motion_m, answers.get(c.asset_id, []),
            set(styles), ctx.settings.uthana_locomotion_strides, c.notes,
        )
        c.motions.insert(0, UthanaMotionSpec(
            key="idle", kind=MotionSource.uthana_library, motion_name=idle.name,
            motion_id=idle_id, purpose="shared library idle",
        ))

    plan = UthanaMotionPlan(prompt_id=build_spec.prompt_id, locomotion_styles=styles,
                            characters=characters)
    art = save_motion_plan(ctx.project_dir, plan)
    for c in characters:
        ctx.emit(_STAGE, "info",
                 f"{c.asset_id}: h={c.height_for_motion_m:.2f} m -> "
                 + ", ".join(f"{m.key}={m.motion_name}" for m in c.motions))
        for note in c.notes:
            ctx.emit(_STAGE, "warning", f"{c.asset_id}: {note}"[:300])
    total = sum(len(c.motions) for c in characters)
    message = f"{total} motion(s) planned for {len(characters)} character(s)"
    artifacts = [rel(ctx.settings.repo_root, art)]
    ctx.store.record_stage(state, _STAGE, ProjectStatus.succeeded, message=message,
                           artifacts=artifacts)
    ctx.emit(_STAGE, "succeeded", message)
    return StageResult(stage=_STAGE, artifacts=artifacts, data=plan)


# ---------------------------------------------------------------------------


def _styles(ctx: PipelineContext, client: Any) -> list[str]:
    try:
        styles = [s.lower() for s in client.list_locomotion_styles()]
    except Exception as exc:  # noqa: BLE001
        ctx.emit(_STAGE, "warning", f"style query failed ({exc}); using the cached list")
        styles = []
    return styles or list(UTHANA_STYLES_FALLBACK)


def _ask_agent(
    ctx: PipelineContext,
    agent: Optional[StructuredAgent],
    characters: list[UthanaCharacterMotionPlan],
    routes: list[CharacterRoute],
    by_id: dict[str, Any],
    styles: list[str],
) -> dict[str, list[PlannedLocomotion]]:
    if ctx.settings.openai_api_key and not os.environ.get("OPENAI_API_KEY"):
        os.environ["OPENAI_API_KEY"] = ctx.settings.openai_api_key
    if agent is None:
        agent = StructuredAgent(
            name=f"ptv-uthana-motion-plan-{ctx.project_id}",
            instructions=UTHANA_MOTION_PLAN_SYSTEM,
            output_model=UthanaMotionAgentOutput,
            model=ctx.settings.uthana_motion_plan_model,
            reasoning_effort=ctx.settings.uthana_motion_plan_reasoning_effort,
            verbosity=ctx.settings.uthana_motion_plan_verbosity,
            max_turns=ctx.settings.uthana_motion_plan_agent_max_turns,
            max_retries=ctx.settings.uthana_motion_plan_max_retries,
            log=lambda t: ctx.store.append_log(ctx.project_id, "uthana_motion_plan", t),
        )
    npc_plan = ctx.npc_plan
    if npc_plan is None:
        try:
            npc_plan = ctx.store.load_npc_plan(ctx.project_id)
        except (FileNotFoundError, ValueError):
            npc_plan = None
    npcs = {n.asset_id: n for n in (npc_plan.npcs if npc_plan else [])}
    route_by_id = {r.asset_id: r for r in routes}
    sections = []
    for c in characters:
        route = route_by_id[c.asset_id]
        item = by_id.get(c.asset_id)
        keys = [m.key for m in route.motions_from(MotionSource.uthana_locomotion)]
        sections.append(json.dumps({
            "asset_id": c.asset_id,
            "role": c.character_role,
            "name": item.name if item else c.asset_id,
            "style_notes": (item.style_notes[:600] if item else ""),
            "height_for_motion_m": c.height_for_motion_m,
            "walk_move_speed": round_speed(WALK_SPEED_PER_M * c.height_for_motion_m),
            "run_move_speed": round_speed(RUN_SPEED_PER_M * c.height_for_motion_m),
            "motions_to_plan": keys,
            "routing_style_hints": {m.key: m.style_hint for m in route.required_motions
                                    if m.style_hint},
            "routing_rationale": route.rationale[:600],
            "npc_plan": json.loads(npcs[c.asset_id].model_dump_json()) if c.asset_id in npcs else None,
        }, indent=2))
    brief = AgentBrief(text=(
        "Plan the Uthana locomotion motions for these characters.\n\n"
        "================ Uthana locomotion styles (live) ================\n"
        f"{', '.join(styles)}\n\n"
        "================ Characters ================\n" + "\n\n".join(sections)
    ))
    try:
        out = agent.run(brief)
    except StructuredAgentError as exc:
        ctx.emit(_STAGE, "warning", f"motion-plan agent unavailable ({exc}); using defaults")
        return {}
    return {c.asset_id: list(c.motions) for c in out.characters}


def _gait_of(key: str) -> str:
    base = key.split(".", 1)[0]
    return "run" if base == "run" else "walk"


def plan_character_motions(
    route: CharacterRoute,
    height_m: float,
    answers: list[PlannedLocomotion],
    styles: set[str],
    default_strides: int,
    notes: list[str],
) -> list[UthanaMotionSpec]:
    """Validate and correct the agent's plan into canonical motion specs."""
    wanted = [m for m in route.motions_from(MotionSource.uthana_locomotion)]
    by_key = {a.key: a for a in answers}
    keys = [m.key for m in wanted]
    # Gait variants the agent added on its own are honoured too.
    for a in answers:
        base = a.key.split(".", 1)[0]
        if a.key not in keys and base in ("walk", "run") and a.key != "idle":
            keys.append(a.key)
    hints = {m.key: m.style_hint for m in wanted if m.style_hint}
    purposes = {m.key: m.purpose for m in wanted}
    # A gait the agent did not answer takes the character's own style from
    # its other plain gaits, so a character never walks in one body attitude
    # and runs in another by accident.
    character_style = next(
        (a.style_id.lower() for a in answers
         if "." not in a.key and a.key != "idle" and a.style_id.lower() in styles),
        None,
    ) or next((h.lower() for k, h in hints.items() if "." not in k and h.lower() in styles), None)

    out: list[UthanaMotionSpec] = []
    for key in keys:
        gait = _gait_of(key)
        a = by_key.get(key)
        style = (a.style_id.lower() if a else "") or (hints.get(key) or "")
        if style not in styles:
            fallback = (hints.get(key) or "").lower()
            if style:
                notes.append(f"{key}: style {style!r} is not a Uthana style")
            if fallback in styles:
                style = fallback
            elif "." not in key and character_style:
                style = character_style
            else:
                style = DEFAULT_STYLE
        strides = a.strides if (a and 1 <= a.strides <= 5) else default_strides
        expected = (RUN_SPEED_PER_M if gait == "run" else WALK_SPEED_PER_M) * height_m
        speed = a.move_speed if a else expected
        if expected > 0 and abs(speed - expected) / expected > SPEED_TOLERANCE:
            if a:
                notes.append(f"{key}: speed {speed:.2f} replaced by {expected:.2f} "
                             f"({gait} factor x {height_m:.2f} m)")
            speed = expected
        if speed > UTHANA_MAX_MOVE_SPEED or speed < UTHANA_MIN_MOVE_SPEED:
            clamped = min(UTHANA_MAX_MOVE_SPEED, max(UTHANA_MIN_MOVE_SPEED, speed))
            notes.append(f"{key}: {speed:.2f} m/s is outside Uthana's "
                         f"{UTHANA_MIN_MOVE_SPEED}-{UTHANA_MAX_MOVE_SPEED} m/s; using {clamped:.2f}")
            speed = clamped
        speed = round_speed(speed)
        out.append(UthanaMotionSpec(
            key=key,
            kind=MotionSource.uthana_locomotion,
            gait=gait,
            style_id=style,
            strides=strides,
            move_speed=speed,
            travel_angle=0.0,
            motion_name=format_locomotion_name(style, strides, speed, 0.0),
            purpose=(a.purpose if a and a.purpose else purposes.get(key, ""))[:400],
        ))
    return out
