"""Stage 3: BuildSpec + AssetManifest -> MechanicPlan (Claude Opus).

The planner is reuse-first: at runtime we append the engine catalog
(built-in mechanics + mechanics promoted from past games) to the user
prompt as a stable reference. The system prompt instructs the model to
classify each mechanic against that catalog before flagging anything as
"generate". See ``apps/orchestrator/ptv_orchestrator/engine_catalog.py``
for the catalog source of truth.
"""

from __future__ import annotations

import json

from ptv_schemas import MechanicPlan, ProjectStage, ProjectStatus

from ..engine_catalog import as_planner_text
from .context import PipelineContext, StageResult
from .llm import make_llm
from .prompts import MECHANIC_PLAN_SYSTEM


def run(ctx: PipelineContext) -> StageResult:
    ctx.emit(ProjectStage.mechanic_plan, "running", "Planning mechanics")
    state = ctx.store.load_state(ctx.project_id)
    ctx.store.record_stage(state, ProjectStage.mechanic_plan, ProjectStatus.running)

    build_spec = ctx.build_spec or ctx.store.load_build_spec(ctx.project_id)
    manifest = ctx.asset_manifest or ctx.store.load_asset_manifest(ctx.project_id)

    catalog_text = as_planner_text(ctx.settings.repo_root)

    client = make_llm(ctx.settings)
    prompt = (
        "BuildSpec (JSON):\n"
        + json.dumps(build_spec.model_dump(mode="json"), indent=2)
        + "\n\nAssetManifest (JSON):\n"
        + json.dumps(manifest.model_dump(mode="json"), indent=2)
        + "\n\n"
        + catalog_text
        + "\n\nProduce a MechanicPlan. Reuse catalog entries (built-in OR"
        " promoted) whenever the request is covered. Only emit 'generate'"
        " items when no catalog id reasonably fits."
    )
    resp = client.generate_json(
        prompt=prompt,
        schema=MechanicPlan.model_json_schema(by_alias=True),
        system=MECHANIC_PLAN_SYSTEM,
    )
    plan = MechanicPlan.model_validate(resp.data)
    ctx.mechanic_plan = plan
    art = ctx.store.save_mechanic_plan(ctx.project_id, plan)

    # Emit a reuse-vs-generate summary so the UI can highlight engine growth.
    counts: dict[str, int] = {}
    for it in plan.items:
        key = it.classification.value
        counts[key] = counts.get(key, 0) + 1
    gen_count = counts.get("generate", 0)
    reuse_count = counts.get("builtin", 0) + counts.get("builtin+config", 0)
    summary = (
        f"reuse={reuse_count} (builtin/builtin+config), generate={gen_count}"
    )
    ctx.emit(ProjectStage.mechanic_plan, "info", summary)

    ctx.store.record_stage(
        state,
        ProjectStage.mechanic_plan,
        ProjectStatus.succeeded,
        message=f"{gen_count} mechanics require generated C++ ({summary})",
        artifacts=[art.path],
    )
    ctx.emit(
        ProjectStage.mechanic_plan,
        "succeeded",
        f"{gen_count} to generate, {reuse_count} reused",
    )
    return StageResult(stage=ProjectStage.mechanic_plan, artifacts=[art.path], data=plan)
