"""Stage 3: BuildSpec + AssetManifest -> MechanicPlan (Claude Opus)."""

from __future__ import annotations

import json

from ptv_schemas import MechanicPlan, ProjectStage, ProjectStatus

from .context import PipelineContext, StageResult
from .llm import make_llm
from .prompts import MECHANIC_PLAN_SYSTEM


def run(ctx: PipelineContext) -> StageResult:
    ctx.emit(ProjectStage.mechanic_plan, "running", "Planning mechanics")
    state = ctx.store.load_state(ctx.project_id)
    ctx.store.record_stage(state, ProjectStage.mechanic_plan, ProjectStatus.running)

    build_spec = ctx.build_spec or ctx.store.load_build_spec(ctx.project_id)
    manifest = ctx.asset_manifest or ctx.store.load_asset_manifest(ctx.project_id)

    client = make_llm(ctx.settings)
    prompt = (
        "BuildSpec (JSON):\n"
        + json.dumps(build_spec.model_dump(mode="json"), indent=2)
        + "\n\nAssetManifest (JSON):\n"
        + json.dumps(manifest.model_dump(mode="json"), indent=2)
        + "\n\nProduce a MechanicPlan. Reuse built-in mechanics whenever possible."
    )
    resp = client.generate_json(
        prompt=prompt,
        schema=MechanicPlan.model_json_schema(by_alias=True),
        system=MECHANIC_PLAN_SYSTEM,
    )
    plan = MechanicPlan.model_validate(resp.data)
    ctx.mechanic_plan = plan
    art = ctx.store.save_mechanic_plan(ctx.project_id, plan)

    gen_count = sum(1 for it in plan.items if it.classification.value == "generate")
    ctx.store.record_stage(
        state,
        ProjectStage.mechanic_plan,
        ProjectStatus.succeeded,
        message=f"{gen_count} mechanics require generated C++",
        artifacts=[art.path],
    )
    ctx.emit(ProjectStage.mechanic_plan, "succeeded", f"{gen_count} to generate")
    return StageResult(stage=ProjectStage.mechanic_plan, artifacts=[art.path], data=plan)
