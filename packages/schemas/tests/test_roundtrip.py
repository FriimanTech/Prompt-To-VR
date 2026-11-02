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


def test_build_spec_defaults_to_a_full_headed_first_person_avatar():
    # These defaults are load-bearing rather than cosmetic. A head that was
    # never generated can never be shown, so the asset always has one and
    # visibility is a runtime decision; and a modelled head must not imply
    # that the camera lives inside the skull.
    spec = _sample_build_spec()
    assert spec.player_perspective.value == "first_person_avatar"
    assert spec.player_avatar.coverage.value == "full_body"
    assert spec.player_avatar.head_visibility.value == "hidden_in_self_view"
    assert spec.player_avatar.leg_animation is True
    assert spec.player_avatar.needs_mirror is False


def test_scene_entry_defaults_to_an_exclusive_standalone_environment():
    scene = SceneEntry(id="room", name="Room", summary="A small room.")
    assert scene.kind.value == "standalone"
    assert scene.environment_mode.value == "exclusive"
    assert scene.host is None
    assert scene.links == []
    # Uniform interaction by default: divergence has to be asked for.
    assert _sample_build_spec().interaction_rules == []


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
            "npc_plan",
            "game_design",
            "interaction_design",
            "asset_contract",
            "character_routing",
        "uthana_motion_plan",
        "mechanic_plan",
        "scene_spec",
        "code_task",
        "project_state",
        "artifact_metadata",
        "acceptance_test_plan",
        "performance_profile",
        "optimization_preferences",
        "runtime_verification",
    }
    schema = json.loads(written["build_spec"].read_text(encoding="utf-8"))
    assert schema["$defs"] or schema.get("properties")


def test_strict_extra_forbidden():
    with pytest.raises(Exception):
        BuildSpec.model_validate({"extra_field": True})
