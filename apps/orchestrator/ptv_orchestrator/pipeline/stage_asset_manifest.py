"""Stage 2: BuildSpec -> AssetManifest (Claude Sonnet)."""

from __future__ import annotations

import json

from ptv_schemas import AssetCategory, AssetManifest, ProjectStage, ProjectStatus

from .context import PipelineContext, StageResult
from .llm import make_llm
from .prompts import ASSET_MANIFEST_SYSTEM


def run(ctx: PipelineContext) -> StageResult:
    ctx.emit(ProjectStage.asset_manifest, "running", "Planning AssetManifest")
    state = ctx.store.load_state(ctx.project_id)
    ctx.store.record_stage(state, ProjectStage.asset_manifest, ProjectStatus.running)

    build_spec = ctx.build_spec or ctx.store.load_build_spec(ctx.project_id)
    ctx.build_spec = build_spec

    client = make_llm(ctx.settings)
    resp = client.generate_json(
        prompt=(
            "BuildSpec (JSON):\n"
            + json.dumps(build_spec.model_dump(mode="json"), indent=2)
            + "\n\nProduce an AssetManifest JSON object for the assets above."
        ),
        schema=AssetManifest.model_json_schema(by_alias=True),
        system=ASSET_MANIFEST_SYSTEM,
        model=ctx.settings.claude_light_model,
    )
    manifest = AssetManifest.model_validate(resp.data)

    # The VR environment is now authored in C++ by the world-generation stage,
    # not generated as a Tripo GLB. Drop any environment-category items the
    # planner still emitted (belt-and-suspenders behind the prompt), as long as
    # at least one non-environment object remains (AssetManifest requires >=1).
    env_items = [i for i in manifest.items if i.category == AssetCategory.environment]
    non_env = [i for i in manifest.items if i.category != AssetCategory.environment]
    dropped = 0
    if env_items and non_env:
        manifest = manifest.model_copy(update={"items": non_env})
        dropped = len(env_items)

    ctx.asset_manifest = manifest
    art = ctx.store.save_asset_manifest(ctx.project_id, manifest)

    msg = f"Planned {len(manifest.items)} assets"
    if dropped:
        msg += f" (dropped {dropped} environment asset(s); world is C++-generated)"
    ctx.store.record_stage(
        state,
        ProjectStage.asset_manifest,
        ProjectStatus.succeeded,
        message=msg,
        artifacts=[art.path],
    )
    ctx.emit(ProjectStage.asset_manifest, "succeeded", f"{len(manifest.items)} assets")
    return StageResult(
        stage=ProjectStage.asset_manifest, artifacts=[art.path], data=manifest
    )
