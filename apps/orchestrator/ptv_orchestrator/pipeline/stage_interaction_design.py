"""Stage 2a: decide how every object in the game is used, held and fired.

Runs directly after ``asset_manifest`` and before ``npc_plan``. Two reasons
for the position:

* Projectile meshes (a rocket, an arrow) are decided here and appended to the
  manifest BEFORE reference_images and Tripo spend money on anything.
* Every later planning stage (npc_plan, game_design, mechanic_plan,
  scene_spec, codegen) reads the result, so "the pistol is a hybrid hitscan
  weapon held two-handed" is decided once and agreed on everywhere.

The agent's answer goes through a validate-and-repair loop like GameDesign's:
validator findings go back to the agent up to
``interaction_design_max_repair_rounds`` times, whatever is left is completed
deterministically, and quotes the agent could not substantiate are downgraded
to inferred. When the agent cannot run at all, a conservative design is
derived from asset names so obvious weapons still get holds and muzzles.
"""

from __future__ import annotations

import os
from typing import Optional

from ptv_schemas import (
    AssetCategory,
    InteractionDesign,
    ProjectStage,
    ProjectStatus,
)

from .context import PipelineContext, StageResult
from .interaction_design_agent import (
    InteractionDesignAgentError,
    InteractionDesignAgentInput,
    InteractionDesigner,
    build_designer_message,
)
from .interaction_rules import (
    downgrade_unquoted,
    fallback_design,
    interaction_violations,
    reconcile_manifest,
    repair_interaction_design,
)
from .prompts import INTERACTION_DESIGN_SYSTEM

_STAGE = ProjectStage.interaction_design


class InteractionDesignError(RuntimeError):
    """The design could not be made runnable."""


def run(ctx: PipelineContext) -> StageResult:
    return run_with_designer(ctx, designer=None)


def run_with_designer(ctx: PipelineContext,
                      designer: Optional[InteractionDesigner] = None) -> StageResult:
    ctx.emit(_STAGE, "running", "Designing how each item is held and used")
    state = ctx.store.load_state(ctx.project_id)
    ctx.store.record_stage(state, _STAGE, ProjectStatus.running)

    build_spec = ctx.build_spec or ctx.store.load_build_spec(ctx.project_id)
    manifest = ctx.asset_manifest or ctx.store.load_asset_manifest(ctx.project_id)
    prompt = _read_prompt(ctx)

    props = [a for a in manifest.items if a.category != AssetCategory.character]
    source = "designer"
    if not props:
        design = InteractionDesign(prompt_id=build_spec.prompt_id,
                                   omitted=["no held or interactive objects"])
        source = "empty"
    else:
        design, source = _design(ctx, designer, build_spec, manifest, prompt)

    design = design.model_copy(update={"prompt_id": build_spec.prompt_id})
    design, notes = repair_interaction_design(design, prompt=prompt, manifest=manifest)
    for note in notes:
        ctx.emit(_STAGE, "info", note[:240])
    leftover = interaction_violations(design, prompt=prompt, manifest=manifest)
    if leftover:
        design = downgrade_unquoted(design, prompt)
        leftover = interaction_violations(design, prompt=prompt, manifest=manifest)
    if leftover:
        msg = "interaction design is still incomplete: " + "; ".join(leftover)
        ctx.store.record_stage(state, _STAGE, ProjectStatus.failed, message=msg[:400])
        ctx.emit(_STAGE, "failed", msg[:400])
        raise InteractionDesignError(msg)

    new_manifest, design, rec_notes = reconcile_manifest(manifest, design)
    for note in rec_notes:
        ctx.emit(_STAGE, "info", note[:240])
        ctx.store.append_log(ctx.project_id, "interaction_design", note)
    artifacts = []
    if new_manifest.model_dump() != manifest.model_dump():
        artifacts.append(ctx.store.save_asset_manifest(ctx.project_id, new_manifest).path)
    ctx.asset_manifest = new_manifest

    art = ctx.store.save_interaction_design(ctx.project_id, design)
    artifacts.insert(0, art.path)
    ctx.interaction_design = design

    held = [i for i in design.items if i.hold.kind.value != "none"]
    message = (f"{source}: {len(design.items)} item(s), {len(held)} held, "
               f"{len(design.weapons())} weapon(s), "
               f"{len(design.projectiles)} projectile(s), "
               f"{sum(1 for p in design.projectiles if p.needs_mesh)} with a mesh")
    ctx.store.record_stage(state, _STAGE, ProjectStatus.succeeded,
                           message=message[:400], artifacts=artifacts)
    ctx.emit(_STAGE, "succeeded", message[:400])
    return StageResult(stage=_STAGE, artifacts=artifacts, data=design)


def _design(ctx: PipelineContext, designer: Optional[InteractionDesigner],
            build_spec, manifest, prompt: str) -> tuple[InteractionDesign, str]:
    if ctx.settings.openai_api_key and not os.environ.get("OPENAI_API_KEY"):
        os.environ["OPENAI_API_KEY"] = ctx.settings.openai_api_key
    if designer is None:
        if not (ctx.settings.openai_api_key or os.environ.get("OPENAI_API_KEY")):
            note = "no OpenAI key; interaction design derived from asset names"
            ctx.emit(_STAGE, "warning", note)
            return fallback_design(manifest, prompt), "fallback"
        designer = InteractionDesigner(
            instructions=INTERACTION_DESIGN_SYSTEM,
            model=ctx.settings.interaction_design_model,
            reasoning_effort=ctx.settings.interaction_design_reasoning_effort,
            verbosity=ctx.settings.interaction_design_verbosity,
            max_turns=ctx.settings.interaction_design_agent_max_turns,
            session_id=f"ptv-interaction-design-{ctx.project_id}",
            web_search=ctx.settings.interaction_design_web_search,
            max_retries=ctx.settings.interaction_design_max_retries,
            log=lambda text: ctx.store.append_log(ctx.project_id,
                                                  "interaction_design", text),
        )
    device = getattr(build_spec.target_device, "value", build_spec.target_device)
    inp = InteractionDesignAgentInput(
        prompt_text=prompt,
        build_spec_json=build_spec.model_dump_json(indent=2),
        asset_manifest_json=manifest.model_dump_json(indent=2),
        extra_instructions=[
            f"Target device: {device}. "
            + ("Quest 3 is a mobile GPU: prefer hybrid over simulated bullets and "
               "give a projectile a mesh only when it is seen."
               if device == "quest" else
               "PC VR still runs at 72-120 Hz: invisible bullets are hybrid."),
            "Only non-character assets belong in `items`.",
        ],
    )
    message = build_designer_message(inp)
    try:
        design = designer.run(message)
    except InteractionDesignAgentError as exc:
        note = (f"interaction designer unavailable ({exc}); using a design derived "
                "from asset names")
        ctx.emit(_STAGE, "warning", note[:240])
        ctx.store.append_log(ctx.project_id, "interaction_design", note)
        return fallback_design(manifest, prompt), "fallback"

    for round_ in range(ctx.settings.interaction_design_max_repair_rounds):
        problems = interaction_violations(design, prompt=prompt, manifest=manifest)
        if not problems:
            break
        ctx.emit(_STAGE, "warning",
                 f"interaction design needs revising (round {round_ + 1}): "
                 + "; ".join(problems)[:600])
        try:
            design = designer.run(
                message + "\n\nYour previous InteractionDesign had these problems; "
                "return the COMPLETE design with all of them fixed:\n"
                + "\n".join(f"- {p}" for p in problems))
        except InteractionDesignAgentError as exc:
            ctx.store.append_log(ctx.project_id, "interaction_design",
                                 f"revision failed: {exc}")
            break
    return design, "designer"


def _read_prompt(ctx: PipelineContext) -> str:
    path = ctx.project_dir / "prompt.txt"
    if not path.exists():
        return "(original prompt unavailable)"
    return path.read_text(encoding="utf-8")[:20_000]
