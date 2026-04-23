"""Stage 4: BuildSpec + AssetManifest + MechanicPlan -> SceneSpec per scene."""

from __future__ import annotations

import json

from ptv_schemas import ProjectStage, ProjectStatus, SceneSpec

from .context import PipelineContext, StageResult
from .llm import make_llm
from .prompts import SCENE_SPEC_SYSTEM


def run(ctx: PipelineContext) -> StageResult:
    ctx.emit(ProjectStage.scene_spec, "running", "Composing SceneSpec(s)")
    state = ctx.store.load_state(ctx.project_id)
    ctx.store.record_stage(state, ProjectStage.scene_spec, ProjectStatus.running)

    build_spec = ctx.build_spec or ctx.store.load_build_spec(ctx.project_id)
    manifest = ctx.asset_manifest or ctx.store.load_asset_manifest(ctx.project_id)
    plan = ctx.mechanic_plan or ctx.store.load_mechanic_plan(ctx.project_id)

    client = make_llm(ctx.settings)
    schema = SceneSpec.model_json_schema(by_alias=True)
    artifacts: list[str] = []
    scenes: list[SceneSpec] = []
    for scene in build_spec.scenes:
        prompt = (
            f"Compose the SceneSpec for scene_id={scene.id!r}, name={scene.name!r}.\n"
            f"Summary: {scene.summary}\n\nBuildSpec (JSON):\n"
            + json.dumps(build_spec.model_dump(mode="json"), indent=2)
            + "\n\nAssetManifest (JSON):\n"
            + json.dumps(manifest.model_dump(mode="json"), indent=2)
            + "\n\nMechanicPlan (JSON):\n"
            + json.dumps(plan.model_dump(mode="json"), indent=2)
        )
        resp = client.generate_json(
            prompt=prompt,
            schema=schema,
            system=SCENE_SPEC_SYSTEM,
        )
        scene_spec = SceneSpec.model_validate(resp.data)
        filename = f"scene_spec.{scene.id}.json"
        # For single-scene builds, also write the canonical scene_spec.json.
        art = ctx.store.save_scene_spec(ctx.project_id, scene_spec, filename=filename)
        artifacts.append(art.path)
        if scene.entry_point or len(build_spec.scenes) == 1:
            art2 = ctx.store.save_scene_spec(
                ctx.project_id, scene_spec, filename="scene_spec.json"
            )
            artifacts.append(art2.path)
        scenes.append(scene_spec)

    ctx.scene_specs = scenes
    ctx.store.record_stage(
        state,
        ProjectStage.scene_spec,
        ProjectStatus.succeeded,
        message=f"Composed {len(scenes)} scenes",
        artifacts=artifacts,
    )
    ctx.emit(ProjectStage.scene_spec, "succeeded", f"{len(scenes)} scenes")
    return StageResult(stage=ProjectStage.scene_spec, artifacts=artifacts, data=scenes)
