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
    EquipSlotContractError,
    SceneAssetMismatchError,
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
from ptv_schemas.scene_spec import EntitySpec, EquipSlotSpec, MeshRef

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


def test_hold_profile_and_socket_extras_reach_scene_json():
    from ptv_orchestrator.pipeline.stage_scene_compile import (
        _mesh_render_fields,
        hold_violations,
    )

    contract = {
        "sockets": {
            "grip": {"position": [0.0, -0.2, 0.0], "rotation": [0, 0, 0, 1],
                     "kind": "hand_grip", "confidence": "vision_verified",
                     "handle_thickness_unit": 0.1, "mirror": "same"},
            "support": {"position": [0.5, 0.0, 0.0], "rotation": [0, 0, 0, 1],
                        "kind": "support_grip", "mirror": "flip_z180"},
            "muzzle": {"position": [0.0, 0.0, 1.0], "rotation": [0, 0, 0, 1],
                       "kind": "muzzle", "bore_diameter_unit": 0.01},
        },
        "hold": {"kind": "two_hand_line", "aim_aligned": True, "two_hand": "optional",
                 "primary_socket": "grip", "support_socket": "support",
                 "finger_pose": "trigger", "forward": [0, 0, 1], "up": [0, 1, 0]},
    }
    out = _mesh_render_fields({}, 0.8, contract, asset_id="rifle")
    assert out["sockets"]["grip"]["kind"] == "hand_grip"
    assert out["sockets"]["grip"]["handle_thickness_m"] == pytest.approx(0.08)
    assert out["sockets"]["muzzle"]["bore_diameter_m"] == pytest.approx(0.008)
    assert out["sockets"]["support"]["mirror"] == "flip_z180"
    assert out["hold"]["kind"] == "two_hand_line" and out["hold"]["forward"] == [0, 0, 1]
    errors, warnings = hold_violations([{"id": "rifle", "mesh": out}])
    assert errors == [] and warnings == []
    out["hold"]["primary_socket"] = "nope"
    errors, _ = hold_violations([{"id": "rifle", "mesh": out}])
    assert errors and "missing primary socket" in errors[0]


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


# ---------------------------------------------------------------------------
# Mechanics bound to a mesh-less entity.
#
# A shooting hub binds to the player's right hand, which has no mesh by
# design, and reads the muzzle off whatever weapon it resolves at runtime --
# guarded by has_socket. Demanding the socket sit on the hand itself failed a
# correct build. What is still worth guaranteeing is that SOME asset in the
# game can supply the socket, or the mechanic is dead code.
# ---------------------------------------------------------------------------


def _hand_and_weapon_scene() -> SceneSpec:
    """The real shooting-range shape: a mesh-less hand plus a weapon."""
    scene = _scene()
    scene.entities[0].mechanic_bindings = []
    scene.entities.insert(
        0,
        EntitySpec(
            id="player_right_hand",
            name="Right hand",
            transform=Transform(position=Vec3(x=0.0, y=1.2, z=0.0)),
            mesh=None,
            mechanic_bindings=[MechanicBinding(mechanic_id="generated.fire")],
        ),
    )
    return scene


def test_meshless_binding_passes_when_another_asset_supplies_the_socket():
    warnings: list[str] = []
    doc = compile_scene_json(
        _build_spec(),
        _manifest(),
        _plan(["muzzle"]),
        _hand_and_weapon_scene(),
        {},
        _CONTRACT,
        on_warning=warnings.append,
    )

    assert doc["entities"][0]["id"] == "player_right_hand"
    # Not silent: the compile says which asset will satisfy it at runtime,
    # which is the fact you need when the gun turns out not to fire.
    socket_warnings = [w for w in warnings if "socket" in w]
    assert len(socket_warnings) == 1
    assert "player_right_hand" in socket_warnings[0]
    assert "blaster" in socket_warnings[0]


def test_meshless_binding_fails_when_no_asset_supplies_the_socket():
    with pytest.raises(SocketContractError) as excinfo:
        compile_scene_json(
            _build_spec(),
            _manifest(),
            _plan(["muzzle"]),
            _hand_and_weapon_scene(),
            {},
            {"blaster": {"status": "ok", "space": "unit", "sockets": {}}},
        )
    assert "no asset in the scene provides it" in str(excinfo.value)
    assert "player_right_hand" in str(excinfo.value)


def test_mesh_naming_an_unknown_asset_fails_the_compile():
    """A stale scene used to fall through to the SceneSpec's own glb_path and
    emit a mesh pointing at a GLB that was never generated."""
    scene = _scene()
    scene.entities[0].mesh = MeshRef(
        asset_id="weapon_bench", glb_path="projects/demo/assets/bench/model.glb"
    )
    with pytest.raises(SceneAssetMismatchError) as excinfo:
        compile_scene_json(
            _build_spec(), _manifest(), _plan([]), scene, {}, _CONTRACT
        )
    message = str(excinfo.value)
    assert "weapon_bench" in message
    # The message has to name the file to delete and the stage to re-run.
    assert "scene_spec.room.json" in message


def test_mesh_bearing_entity_still_fails_hard_on_its_own_asset():
    """The original protection is untouched: a mechanic bound to an entity
    that HAS a mesh is reading `self`, so the socket must be on that asset."""
    with pytest.raises(SocketContractError) as excinfo:
        compile_scene_json(
            _build_spec(),
            _manifest(),
            _plan(["muzzle"]),
            _scene(),
            {},
            {"blaster": {"status": "ok", "space": "unit", "sockets": {}}},
        )
    assert "provides []" in str(excinfo.value)


# ---------------------------------------------------------------------------
# Equip slots.
#
# A mesh SOCKET is a point on an item ("the muzzle of this rifle"); an equip
# SLOT is a place on a WEARER ("the holster on this player's right hip").
# CEquipSlots was only ever created in smoke.cpp, so builtin.equip bound to a
# holster could never fire and nothing said so.
# ---------------------------------------------------------------------------


def _equip_plan() -> MechanicPlan:
    """A builtin+config equip whose slot filter comes from plan config."""
    return MechanicPlan(
        prompt_id="demo",
        items=[
            MechanicPlanItem(
                id="equip",
                classification=MechanicClassification.builtin_config,
                builtin_id="builtin.equip",
                config={"slots": ["hip_holster_r"], "snap_radius_m": 0.2},
            )
        ],
    )


def _wearer_scene(slot_name: str | None) -> SceneSpec:
    scene = _scene()
    scene.entities[0].mechanic_bindings = [
        MechanicBinding(mechanic_id="builtin.equip")
    ]
    scene.entities[0].tags = ["pistol"]
    player = EntitySpec(
        id="player_rig",
        name="Player",
        transform=Transform(position=Vec3(x=0.0, y=0.0, z=0.0)),
        mesh=None,
    )
    if slot_name is not None:
        player.equip_slots = [
            EquipSlotSpec(
                name=slot_name,
                body_slot="waist",
                local_position=Vec3(x=0.16, y=-0.05, z=0.02),
                snap_radius_m=0.2,
                accepts_tags=["pistol"],
            )
        ]
    scene.entities.append(player)
    return scene


def test_declared_equip_slot_reaches_scene_json():
    doc = compile_scene_json(
        _build_spec(),
        _manifest(),
        _equip_plan(),
        _wearer_scene("hip_holster_r"),
        {},
        _CONTRACT,
    )
    player = next(e for e in doc["entities"] if e["id"] == "player_rig")
    slot = player["equip_slots"][0]
    assert slot["name"] == "hip_holster_r"
    assert slot["body_slot"] == "waist"
    assert slot["local_position"] == pytest.approx([0.16, -0.05, 0.02])
    assert slot["local_rotation"] == [0.0, 0.0, 0.0, 1.0]
    assert slot["snap_radius_m"] == pytest.approx(0.2)
    assert slot["exclusive"] is True
    assert slot["accepts_tags"] == ["pistol"]


def test_equip_binding_with_no_slot_anywhere_fails_the_compile():
    with pytest.raises(EquipSlotContractError) as excinfo:
        compile_scene_json(
            _build_spec(),
            _manifest(),
            _equip_plan(),
            _wearer_scene(None),
            {},
            _CONTRACT,
        )
    message = str(excinfo.value)
    assert "blaster1" in message
    # The fix has to be obvious from the message alone.
    assert "WEARER" in message
    assert "not on the holster prop" in message


def test_equip_binding_naming_a_slot_nobody_declares_fails():
    """params.slots arrives from the MechanicPlan config, so this can only be
    caught after the merge -- which is why the check runs on compiled
    entities rather than on the SceneSpec."""
    with pytest.raises(EquipSlotContractError) as excinfo:
        compile_scene_json(
            _build_spec(),
            _manifest(),
            _equip_plan(),
            _wearer_scene("hip_holster_LEFT"),
            {},
            _CONTRACT,
        )
    assert "hip_holster_r" in str(excinfo.value)
    assert "hip_holster_LEFT" in str(excinfo.value)


def test_duplicate_slot_names_across_entities_fail():
    scene = _wearer_scene("hip_holster_r")
    twin = EntitySpec(
        id="npc_guard",
        name="Guard",
        transform=Transform(position=Vec3(x=2.0, y=0.0, z=0.0)),
        mesh=None,
        equip_slots=[
            EquipSlotSpec(name="hip_holster_r", body_slot="waist")
        ],
    )
    scene.entities.append(twin)
    with pytest.raises(EquipSlotContractError, match="unique across the scene"):
        compile_scene_json(
            _build_spec(), _manifest(), _equip_plan(), scene, {}, _CONTRACT
        )


def test_scenes_without_equip_are_untouched():
    """Every project generated before equip slots existed must compile."""
    doc = compile_scene_json(
        _build_spec(), _manifest(), _plan(["muzzle"]), _scene(), {}, _CONTRACT
    )
    assert "equip_slots" not in doc["entities"][0]
