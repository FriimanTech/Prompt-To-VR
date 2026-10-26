"""Stage 2c: decide the rules of the game loop, once, from the prompt.

Health, lives, what happens on death, objectives, how the game is won or lost,
the end screens and what each of their buttons does, and what a restart puts
back. Before this stage every generated game invented its own run flow, and
the humans-vs-aliens APK showed the result: health reached zero, the director
paused, and the player was left standing in a frozen arena with no screen and
no way to restart.

Runs after ``npc_plan`` (who can hurt the player, which waves exist) and
before ``mechanic_plan``, which maps the loop onto the engine's
``builtin.game_flow`` instead of planning a generated one.

The agent's answer goes through a validate-and-repair loop: the validator's
findings are sent back up to ``game_design_max_repair_rounds`` times, and
whatever is still incomplete is then completed deterministically, so the
engine is never handed a flow with a dead end in it. When the designer cannot
run at all, a conservative design derived from the NPC plan is used.
"""

from __future__ import annotations

import os
from typing import Optional

from ptv_schemas import GameDesign, ProjectStage, ProjectStatus

from .context import PipelineContext, StageResult
from .game_design_agent import (
    GameDesignAgentError,
    GameDesignAgentInput,
    GameDesigner,
    build_designer_message,
)
from .game_design_rules import (
    fallback_design,
    game_design_violations,
    repair_game_design,
)
from .prompts import GAME_DESIGN_SYSTEM

_STAGE = ProjectStage.game_design


def run(ctx: PipelineContext) -> StageResult:
    return run_with_designer(ctx, designer=None)


def run_with_designer(ctx: PipelineContext,
                      designer: Optional[GameDesigner] = None) -> StageResult:
    ctx.emit(_STAGE, "running", "Designing the game loop")
    state = ctx.store.load_state(ctx.project_id)
    ctx.store.record_stage(state, _STAGE, ProjectStatus.running)

    build_spec = ctx.build_spec or ctx.store.load_build_spec(ctx.project_id)
    manifest = ctx.asset_manifest or ctx.store.load_asset_manifest(ctx.project_id)
    npc_plan = ctx.npc_plan or ctx.store.load_npc_plan(ctx.project_id)
    prompt = _read_prompt(ctx)

    if ctx.settings.openai_api_key and not os.environ.get("OPENAI_API_KEY"):
        os.environ["OPENAI_API_KEY"] = ctx.settings.openai_api_key

    if designer is None:
        designer = GameDesigner(
            instructions=GAME_DESIGN_SYSTEM,
            model=ctx.settings.game_design_model,
            reasoning_effort=ctx.settings.game_design_reasoning_effort,
            verbosity=ctx.settings.game_design_verbosity,
            max_turns=ctx.settings.game_design_agent_max_turns,
            session_id=f"ptv-game-design-{ctx.project_id}",
            max_retries=ctx.settings.game_design_max_retries,
            log=lambda text: ctx.store.append_log(ctx.project_id, "game_design", text),
        )

    inp = GameDesignAgentInput(
        prompt_text=prompt,
        build_spec_json=build_spec.model_dump_json(indent=2),
        npc_plan_json=npc_plan.model_dump_json(indent=2),
        asset_manifest_json=manifest.model_dump_json(indent=2),
        extra_instructions=[
            f"Target device: {getattr(build_spec.target_device, 'value', build_spec.target_device)}."
            " Menus are world-space panels the player points at; keep end screens"
            " to at most three buttons.",
        ],
    )
    message = build_designer_message(inp)

    source = "designer"
    try:
        design = designer.run(message)
    except GameDesignAgentError as exc:
        note = f"game designer unavailable ({exc}); using a design derived from the NPC plan"
        ctx.emit(_STAGE, "warning", note[:240])
        ctx.store.append_log(ctx.project_id, "game_design", note)
        design = fallback_design(build_spec, npc_plan, prompt)
        source = "fallback"
    else:
        for round_ in range(ctx.settings.game_design_max_repair_rounds):
            problems = game_design_violations(design, prompt=prompt, npc_plan=npc_plan,
                                              manifest=manifest)
            if not problems:
                break
            ctx.emit(_STAGE, "warning",
                     f"game design needs revising (round {round_ + 1}): "
                     + "; ".join(problems)[:600])
            try:
                design = designer.run(
                    message + "\n\nYour previous GameDesign had these problems; return "
                    "the COMPLETE design with all of them fixed:\n"
                    + "\n".join(f"- {p}" for p in problems))
            except GameDesignAgentError as exc:
                ctx.store.append_log(ctx.project_id, "game_design",
                                     f"revision failed: {exc}")
                break

    design = design.model_copy(update={"prompt_id": build_spec.prompt_id})
    design, notes = repair_game_design(design, npc_plan=npc_plan)
    for note in notes:
        ctx.emit(_STAGE, "info", note)
    leftover = game_design_violations(design, prompt=prompt, npc_plan=npc_plan,
                                      manifest=manifest)
    # Quotes the agent could not substantiate are the only thing repair does
    # not touch: they are downgraded rather than trusted.
    if leftover:
        design = _downgrade_unquoted(design, prompt)
        leftover = game_design_violations(design, prompt=prompt, npc_plan=npc_plan,
                                          manifest=manifest)
    if leftover:
        msg = "game design is still incomplete: " + "; ".join(leftover)
        ctx.store.record_stage(state, _STAGE, ProjectStatus.failed, message=msg[:400])
        ctx.emit(_STAGE, "failed", msg[:400])
        raise GameDesignError(msg)

    art = ctx.store.save_game_design(ctx.project_id, design)
    ctx.game_design = design
    message_out = (
        f"{design.experience.value} ({source}): "
        f"{'health ' + str(int(design.player_vitality.max_health)) if design.player_vitality.has_health else 'no health'}, "
        f"lives {design.lives.count if design.lives.count is not None else 'unlimited' if design.player_vitality.has_health else '-'}, "
        f"{len(design.objectives)} objective(s), "
        f"screens {[s.kind.value for s in design.end_screens] or 'none'}"
    )
    ctx.store.record_stage(state, _STAGE, ProjectStatus.succeeded,
                           message=message_out[:400], artifacts=[art.path])
    ctx.emit(_STAGE, "succeeded", message_out[:400])
    return StageResult(stage=_STAGE, artifacts=[art.path], data=design)


class GameDesignError(RuntimeError):
    """The design could not be made complete."""


def _downgrade_unquoted(design: GameDesign, prompt: str) -> GameDesign:
    from .game_design_rules import _all_provenances, _norm  # noqa: PLC0415
    from ptv_schemas import DecisionSource  # noqa: PLC0415

    d = design.model_copy(deep=True)
    p = _norm(prompt)
    for _where, prov in _all_provenances(d):
        if prov.source == DecisionSource.explicit and _norm(prov.prompt_quote) not in p:
            prov.source = DecisionSource.inferred
            prov.rationale = (prov.rationale or "") + " (quote not found in the prompt)"
            prov.prompt_quote = ""
    return d


def _read_prompt(ctx: PipelineContext) -> str:
    path = ctx.project_dir / "prompt.txt"
    if not path.exists():
        return "(original prompt unavailable)"
    return path.read_text(encoding="utf-8")[:20_000]
