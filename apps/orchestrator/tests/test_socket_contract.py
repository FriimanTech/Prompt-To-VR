"""Scene-compile socket merge + required_sockets validator.

Scene compile is the only stage that can see BOTH halves of the socket
contract: the CodeTask's ``required_sockets`` (decided by the mechanic-plan
agent, long before any GLB existed) and the asset's derived sockets (decided
by the asset_contract stage, which never knew which mechanic would bind).
These cover the merge and the mismatch.
"""

from __future__ import annotations

import pytest

from ptv_orchestrator.pipeline.stage_scene_compile import (
    SocketContractError,
    compile_scene_json,
)
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
from ptv_schemas.code_task import CodeTask, CodeTaskKind
from ptv_schemas.common import Vec3
from ptv_schemas.scene_spec import EntitySpec, MeshRef

# 0.5 in unit space at a 0.4 m longest axis == 0.2 m.
_CONTRACT = {
    "blaster": {
        "status": "ok",
        "space": "unit",
        "sockets": {
            "muzzle": {"position": [0.0, 0.05, 0.5], "rotation": [0, 0, 0, 1]},
        },
    }
}


def _build_spec() -> BuildSpec:
    return BuildSpec(
        prompt_id="demo",
        title="Demo",
        pitch="Shoot targets.",
        game_type=GameType.arcade,
        scenes=[
            SceneEntry(id="room", name="Room", summary="one room.", entry_point=True)
        ],
        mechanics=[MechanicEntry(id="fire", name="Fire", summary="fire the blaster")],
        assets=[
            AssetEntry(id="blaster", name="Blaster", category="prop", summary="gun")
        ],
        acceptance_tests=[TestEntry(id="t1", description="firing works")],
    )


def _manifest() -> AssetManifest:
    return AssetManifest(
        prompt_id="demo",
        items=[
            AssetItem(
                id="blaster",
                name="Blaster",
                category=AssetCategory.prop,
                style_notes="alien blaster",
                reference=ReferenceImageRequest(prompt="an alien blaster"),
                target_scale_meters=0.4,
                socket_hints=["muzzle"],
            )
        ],
    )


def _plan(required_sockets: list[str]) -> MechanicPlan:
    return MechanicPlan(
        prompt_id="demo",
        items=[
            MechanicPlanItem(
                id="fire",
                classification=MechanicClassification.generate,
                code_task=CodeTask(
                    id="fire",
                    kind=CodeTaskKind.mechanic,
                    title="Fire",
                    description="Fire a bolt from the muzzle.",
                    interface="IMechanic",
                    required_sockets=required_sockets,
                ),
            )
        ],
    )


def _scene() -> SceneSpec:
    return SceneSpec(
        prompt_id="demo",
        scene_id="room",
        name="Room",
        entities=[
            EntitySpec(
                id="blaster1",
                name="Blaster1",
                transform=Transform(position=Vec3(x=0.0, y=1.0, z=-0.8)),
                mesh=MeshRef(
                    asset_id="blaster",
                    glb_path="projects/demo/assets/blaster/model.glb",
                ),
                mechanic_bindings=[MechanicBinding(mechanic_id="generated.fire")],
            )
        ],
    )


def test_sockets_are_converted_from_unit_space_to_metres():
    doc = compile_scene_json(
        _build_spec(), _manifest(), _plan(["muzzle"]), _scene(), {}, _CONTRACT
    )
    muzzle = doc["entities"][0]["mesh"]["sockets"]["muzzle"]
    # 0.5 unit * 0.4 m longest axis == 0.2 m, matching how bounds_radius_m is
    # scaled. Rotation is scale-invariant and passes straight through.
    assert muzzle["position"] == pytest.approx([0.0, 0.02, 0.2])
    assert muzzle["rotation"] == [0.0, 0.0, 0.0, 1.0]


def test_missing_required_socket_fails_loudly_with_available_names():
    empty_contract = {"blaster": {"status": "ok", "space": "unit", "sockets": {}}}
    with pytest.raises(SocketContractError) as excinfo:
        compile_scene_json(
            _build_spec(),
            _manifest(),
            _plan(["muzzle"]),
            _scene(),
            {},
            empty_contract,
        )
    message = str(excinfo.value)
    # The error has to name the entity, the mechanic, the missing socket and
    # what WAS available -- otherwise the fix is a guessing game.
    assert "blaster1" in message
    assert "fire" in message
    assert "muzzle" in message
    assert "provides []" in message


def test_all_violations_are_reported_in_one_raise():
    with pytest.raises(SocketContractError) as excinfo:
        compile_scene_json(
            _build_spec(),
            _manifest(),
            _plan(["muzzle", "scope_mount", "bipod"]),
            _scene(),
            {},
            {"blaster": {"status": "ok", "sockets": {}}},
        )
    message = str(excinfo.value)
    assert message.count("requires socket") == 3


def test_empty_required_sockets_skips_validation_entirely():
    """Projects predating sockets must compile untouched."""
    doc = compile_scene_json(
        _build_spec(), _manifest(), _plan([]), _scene(), {}, None
    )
    assert "sockets" not in doc["entities"][0]["mesh"]


def test_satisfied_contract_compiles_clean():
    doc = compile_scene_json(
        _build_spec(), _manifest(), _plan(["muzzle"]), _scene(), {}, _CONTRACT
    )
    assert doc["entities"][0]["mechanic_bindings"][0]["id"] == "generated.fire"


def test_required_socket_on_a_meshless_entity_is_reported():
    scene = _scene()
    scene.entities[0].mesh = None
    with pytest.raises(SocketContractError, match="no mesh to carry them"):
        compile_scene_json(
            _build_spec(), _manifest(), _plan(["muzzle"]), scene, {}, _CONTRACT
        )
