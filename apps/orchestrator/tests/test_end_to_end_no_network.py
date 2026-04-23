"""Smoke test that exercises the deterministic (non-network) stages end-to-end.

The LLM-backed stages (BuildSpec/AssetManifest/MechanicPlan/SceneSpec/
CodeGen) are skipped; the test seeds the project directory with hand-authored
artifacts and then runs the deterministic downstream stages.
"""

from __future__ import annotations

import json
from pathlib import Path

from ptv_orchestrator.pipeline.stage_asset_normalize import run as normalize_run
from ptv_orchestrator.pipeline.stage_scene_compile import run as scene_compile_run
from ptv_orchestrator.pipeline.context import PipelineContext
from ptv_orchestrator.events import EventBus
from ptv_orchestrator.settings import Settings
from ptv_orchestrator.storage import ProjectStore
from ptv_schemas import (
    AssetEntry,
    AssetItem,
    AssetManifest,
    BuildSpec,
    MechanicBinding,
    MechanicClassification,
    MechanicEntry,
    MechanicPlan,
    MechanicPlanItem,
    ReferenceImageRequest,
    SceneEntry,
    SceneSpec,
    TestEntry,
    Transform,
)
from ptv_schemas.asset_manifest import AssetCategory
from ptv_schemas.build_spec import GameType
from ptv_schemas.common import Vec3
from ptv_schemas.scene_spec import EntitySpec, MeshRef, PhysicsProps


def _seed(tmp_path: Path) -> tuple[ProjectStore, str, Settings]:
    settings = Settings(
        projects_dir=tmp_path / "projects",
        engine_dir=tmp_path / "engine",
        samples_dir=tmp_path / "samples",
        repo_root=tmp_path,
    )
    store = ProjectStore(settings.projects_dir)
    state = store.create("Demo", "prompt here")

    build = BuildSpec(
        prompt_id="demo",
        title="Demo",
        pitch="A tiny demo.",
        game_type=GameType.arcade,
        scenes=[SceneEntry(id="room", name="Room", summary="one room.", entry_point=True)],
        mechanics=[MechanicEntry(id="grab", name="Grab", summary="grab cubes")],
        assets=[AssetEntry(id="cube", name="Cube", category="prop", summary="wood cube")],
        acceptance_tests=[TestEntry(id="t1", description="grab works")],
    )
    store.save_build_spec(state.project_id, build)

    manifest = AssetManifest(
        prompt_id="demo",
        items=[
            AssetItem(
                id="cube",
                name="Cube",
                category=AssetCategory.prop,
                style_notes="wood",
                reference=ReferenceImageRequest(prompt="wooden cube"),
                target_scale_meters=0.2,
            )
        ],
    )
    store.save_asset_manifest(state.project_id, manifest)

    plan = MechanicPlan(
        prompt_id="demo",
        items=[
            MechanicPlanItem(
                id="grab",
                classification=MechanicClassification.builtin,
                builtin_id="builtin.grab",
            )
        ],
    )
    store.save_mechanic_plan(state.project_id, plan)

    scene = SceneSpec(
        prompt_id="demo",
        scene_id="room",
        name="Room",
        entities=[
            EntitySpec(
                id="cube1",
                name="Cube",
                transform=Transform(position=Vec3(x=0.0, y=1.0, z=-0.6)),
                mesh=MeshRef(asset_id="cube", glb_path="projects/demo/assets/cube/model.glb"),
                physics=PhysicsProps(kind="dynamic"),
                mechanic_bindings=[MechanicBinding(mechanic_id="builtin.grab")],
            )
        ],
    )
    store.save_scene_spec(state.project_id, scene, filename="scene_spec.room.json")
    store.save_scene_spec(state.project_id, scene, filename="scene_spec.json")

    # Write a minimal valid GLB binary (header + empty JSON chunk + empty BIN chunk).
    import struct

    def _minimal_glb() -> bytes:
        j = b'{"asset":{"version":"2.0"},"materials":[{}],"textures":[{"source":0}],"images":[{}],"meshes":[{"primitives":[]}]}'
        # pad JSON to 4-byte boundary with spaces
        pad = (4 - (len(j) % 4)) % 4
        j = j + b" " * pad
        chunk0 = struct.pack("<II", len(j), 0x4E4F534A) + j
        total = 12 + len(chunk0)
        header = struct.pack("<III", 0x46546C67, 2, total)
        return header + chunk0

    assets_dir = store.dir_for(state.project_id) / "assets" / "cube"
    assets_dir.mkdir(parents=True, exist_ok=True)
    (assets_dir / "model.glb").write_bytes(_minimal_glb())

    return store, state.project_id, settings


def test_deterministic_stages_run(tmp_path: Path):
    store, pid, settings = _seed(tmp_path)
    ctx = PipelineContext(
        project_id=pid,
        settings=settings,
        store=store,
        bus=EventBus(),
    )

    r1 = normalize_run(ctx)
    r2 = scene_compile_run(ctx)

    # Normalization report exists and marks our asset ok.
    report_path = store.dir_for(pid) / "asset_normalization.json"
    assert report_path.exists()
    report = json.loads(report_path.read_text())
    assert report["cube"]["status"] in ("ok", "warn_no_material")
    assert report["cube"]["valid"] is True

    # Scene compile produces scene.json with the mesh resolved.
    scene_path = store.dir_for(pid) / "scene.json"
    assert scene_path.exists()
    scene = json.loads(scene_path.read_text())
    assert scene["version"] == 1
    assert scene["entities"][0]["mesh"]["asset_id"] == "cube"
    assert scene["entities"][0]["mechanic_bindings"][0]["builtin_id"] == "builtin.grab"

    # Stage results report artifacts.
    assert r1.artifacts
    assert r2.artifacts
