"""Smoke tests for schema round-trip and JSON Schema export."""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from ptv_schemas import (
    AcceptanceCheck,
    AcceptanceTestPlan,
    AssetItem,
    AssetManifest,
    BuildSpec,
    CodeTask,
    CodeTaskFile,
    CodeTaskKind,
    EntitySpec,
    MechanicBinding,
    MechanicClassification,
    MechanicEntry,
    MechanicPlan,
    MechanicPlanItem,
    ProjectStage,
    ProjectState,
    ProjectStatus,
    ReferenceImageRequest,
    SceneEntry,
    SceneSpec,
    Transform,
    AssetEntry,
    TestEntry,
)
from ptv_schemas.asset_manifest import AssetCategory
from ptv_schemas.common import Vec3
from ptv_schemas.build_spec import GameType
from ptv_schemas.export import export_all


def _sample_build_spec() -> BuildSpec:
    return BuildSpec(
        prompt_id="demo",
        title="Demo",
        pitch="Pick a block and place it on the pedestal.",
        game_type=GameType.arcade,
        scenes=[SceneEntry(id="room", name="Room", summary="A small room.")],
        mechanics=[MechanicEntry(id="grab", name="Grab", summary="Grab blocks.")],
        assets=[AssetEntry(id="block", name="Block", category="prop", summary="A wooden block.")],
        acceptance_tests=[TestEntry(id="t1", description="Player can grab the block.")],
    )


def test_build_spec_round_trip():
    spec = _sample_build_spec()
    data = spec.model_dump(mode="json")
    back = BuildSpec.model_validate(data)
    assert back == spec


def test_asset_manifest_forces_tripo_defaults():
    item = AssetItem(
        id="block",
        name="Block",
        category=AssetCategory.prop,
        style_notes="Wooden cube, soft shadows.",
        reference=ReferenceImageRequest(prompt="a simple wooden block"),
    )
    assert item.tripo.texture is True
    assert item.tripo.pbr is True
    assert item.tripo.texture_alignment.value == "original_image"


def test_scene_spec_identity_quat_and_unit_scale():
    entity = EntitySpec(id="cube", name="Cube")
    assert entity.transform.rotation.w == 1.0
    assert entity.transform.scale == 1.0


def test_mechanic_plan_allows_builtin_without_code_task():
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
    assert plan.items[0].code_task is None


def test_code_task_file_boundary_is_expressible():
    task = CodeTask(
        id="repulsor",
        kind=CodeTaskKind.mechanic,
        title="Repulsor shooter",
        description="Fire a projectile when the trigger is pulled.",
        interface="IMechanic",
        files=[
            CodeTaskFile(
                path="engine/generated/mechanics/RepulsorShooter.cpp",
                contents="// stub",
            )
        ],
    )
    assert task.files[0].path.startswith("engine/generated/mechanics/")


def test_project_state_stages_default_empty():
    ps = ProjectState(project_id="demo", title="Demo", prompt="hello")
    assert ps.current_stage == ProjectStage.created
    assert ps.status == ProjectStatus.idle
    assert ps.stages == []


def test_export_all_writes_files(tmp_path: Path):
    written = export_all(tmp_path)
    assert set(written) == {
        "build_spec",
        "asset_manifest",
        "mechanic_plan",
        "scene_spec",
        "code_task",
        "project_state",
        "artifact_metadata",
        "acceptance_test_plan",
        "performance_profile",
    }
    schema = json.loads(written["build_spec"].read_text(encoding="utf-8"))
    assert schema["$defs"] or schema.get("properties")


def test_strict_extra_forbidden():
    with pytest.raises(Exception):
        BuildSpec.model_validate({"extra_field": True})
