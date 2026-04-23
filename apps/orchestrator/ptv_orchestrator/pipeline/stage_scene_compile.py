"""Stage 9: deterministic scene compile.

Combines SceneSpec(s), AssetManifest (asset normalization), and MechanicPlan
into an engine-readable ``scene.json`` under
``projects/<id>/builds/scene/<scene_id>.json`` plus a top-level
``scene.json`` pointing at the entry-point scene. The engine consumes this
JSON directly via ``engine/runtime/src/scene/SceneJson.cpp``.

This stage is fully deterministic and runs locally. No LLM calls.
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from ptv_schemas import (
    AssetManifest,
    BuildSpec,
    MechanicPlan,
    MechanicClassification,
    ProjectStage,
    ProjectStatus,
    SceneSpec,
)

from .context import PipelineContext, StageResult


SCENE_JSON_VERSION = 1


def compile_scene_json(
    build_spec: BuildSpec,
    manifest: AssetManifest,
    plan: MechanicPlan,
    scene: SceneSpec,
    normalization_report: dict[str, dict[str, Any]] | None = None,
) -> dict[str, Any]:
    """Pure function: produce the engine-loadable scene JSON dict."""

    asset_by_id = {a.id: a for a in manifest.items}
    plan_by_id = {it.id: it for it in plan.items}
    nrep = normalization_report or {}

    entities_out: list[dict[str, Any]] = []
    for e in scene.entities:
        mesh: dict[str, Any] | None = None
        if e.mesh is not None:
            asset = asset_by_id.get(e.mesh.asset_id)
            norm = nrep.get(e.mesh.asset_id, {})
            mesh = {
                "asset_id": e.mesh.asset_id,
                "glb_path": norm.get("glb_path") or e.mesh.glb_path,
                "target_scale_meters": (
                    asset.target_scale_meters if asset is not None else 1.0
                ),
                "collision_shape": (
                    asset.collision_shape if asset is not None else "aabb"
                ),
            }
        bindings: list[dict[str, Any]] = []
        for b in e.mechanic_bindings:
            item = plan_by_id.get(b.mechanic_id.split(".", 1)[-1])
            resolved: dict[str, Any] = {
                "id": b.mechanic_id,
                "params": dict(b.params),
            }
            if item is not None and item.classification in (
                MechanicClassification.builtin,
                MechanicClassification.builtin_config,
            ):
                resolved["builtin_id"] = item.builtin_id or b.mechanic_id
                # Merge item.config with per-entity params; per-entity wins.
                merged = dict(item.config)
                merged.update(resolved["params"])
                resolved["params"] = merged
            bindings.append(resolved)
        entities_out.append(
            {
                "id": e.id,
                "name": e.name,
                "transform": {
                    "position": [
                        e.transform.position.x,
                        e.transform.position.y,
                        e.transform.position.z,
                    ],
                    "rotation": [
                        e.transform.rotation.x,
                        e.transform.rotation.y,
                        e.transform.rotation.z,
                        e.transform.rotation.w,
                    ],
                    "scale": e.transform.scale,
                },
                "mesh": mesh,
                "physics": (
                    e.physics.model_dump(mode="json") if e.physics is not None else None
                ),
                "mechanic_bindings": bindings,
                "tags": list(e.tags),
            }
        )

    return {
        "version": SCENE_JSON_VERSION,
        "prompt_id": scene.prompt_id,
        "scene_id": scene.scene_id,
        "title": build_spec.title,
        "name": scene.name,
        "player": {
            "height_meters": build_spec.player_height_meters,
            "locomotion": build_spec.locomotion.value,
            "comfort": build_spec.comfort.value,
            "spawn": {
                "position": [
                    scene.player_spawn.transform.position.x,
                    scene.player_spawn.transform.position.y,
                    scene.player_spawn.transform.position.z,
                ],
                "rotation": [
                    scene.player_spawn.transform.rotation.x,
                    scene.player_spawn.transform.rotation.y,
                    scene.player_spawn.transform.rotation.z,
                    scene.player_spawn.transform.rotation.w,
                ],
            },
        },
        "sky_color": list(scene.sky_color),
        "ambient_intensity": scene.ambient_intensity,
        "lights": [
            {
                "id": l.id,
                "kind": l.kind,
                "transform": {
                    "position": [
                        l.transform.position.x,
                        l.transform.position.y,
                        l.transform.position.z,
                    ],
                    "rotation": [
                        l.transform.rotation.x,
                        l.transform.rotation.y,
                        l.transform.rotation.z,
                        l.transform.rotation.w,
                    ],
                    "scale": l.transform.scale,
                },
                "color_rgb": list(l.color_rgb),
                "intensity": l.intensity,
            }
            for l in scene.lights
        ],
        "entities": entities_out,
    }


def run(ctx: PipelineContext) -> StageResult:
    ctx.emit(ProjectStage.scene_compile, "running", "Compiling scene JSON")
    state = ctx.store.load_state(ctx.project_id)
    ctx.store.record_stage(state, ProjectStage.scene_compile, ProjectStatus.running)

    build_spec = ctx.build_spec or ctx.store.load_build_spec(ctx.project_id)
    manifest = ctx.asset_manifest or ctx.store.load_asset_manifest(ctx.project_id)
    plan = ctx.mechanic_plan or ctx.store.load_mechanic_plan(ctx.project_id)

    norm_path = ctx.project_dir / "asset_normalization.json"
    normalization = (
        json.loads(norm_path.read_text(encoding="utf-8"))
        if norm_path.exists()
        else {}
    )

    builds_dir = ctx.project_dir / "builds" / "scene"
    builds_dir.mkdir(parents=True, exist_ok=True)
    artifacts: list[str] = []

    entry_scene_json: str | None = None
    for scene_entry in build_spec.scenes:
        spec_path = ctx.project_dir / f"scene_spec.{scene_entry.id}.json"
        if not spec_path.exists():
            continue
        scene_spec = SceneSpec.model_validate(
            json.loads(spec_path.read_text(encoding="utf-8"))
        )
        doc = compile_scene_json(build_spec, manifest, plan, scene_spec, normalization)
        out = builds_dir / f"{scene_entry.id}.scene.json"
        out.write_text(json.dumps(doc, indent=2, sort_keys=True), encoding="utf-8")
        artifacts.append(str(out.relative_to(ctx.settings.repo_root)).replace("\\", "/"))
        if scene_entry.entry_point or entry_scene_json is None:
            entry_scene_json = str(out)

    # Canonical top-level scene.json used by the runtime.
    if entry_scene_json is not None:
        top = ctx.project_dir / "scene.json"
        Path(top).write_text(
            Path(entry_scene_json).read_text(encoding="utf-8"), encoding="utf-8"
        )
        state.latest_scene_json = str(top)
        ctx.store.save_state(state)
        artifacts.append(str(top.relative_to(ctx.settings.repo_root)).replace("\\", "/"))

    ctx.store.record_stage(
        state,
        ProjectStage.scene_compile,
        ProjectStatus.succeeded,
        message=f"{len(artifacts)} scene file(s)",
        artifacts=artifacts,
    )
    ctx.emit(ProjectStage.scene_compile, "succeeded", f"{len(artifacts)} files")
    return StageResult(stage=ProjectStage.scene_compile, artifacts=artifacts)
