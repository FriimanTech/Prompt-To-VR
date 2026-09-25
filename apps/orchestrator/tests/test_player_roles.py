"""Player roles, movement reconcile and the Hands Only / Full Avatar choice.

Nothing about the player is injected by the engine any more, so these are
the tests that stand between a prompt and a game with two aim arcs, a
teleporter in Beat Saber, or a body the player never asked for.
"""

from __future__ import annotations

from pathlib import Path
from types import SimpleNamespace

import pytest

from ptv_orchestrator.events import EventBus
from ptv_orchestrator.pipeline import stage_asset_manifest
from ptv_orchestrator.pipeline.context import PipelineContext
from ptv_orchestrator.pipeline.movement import (
    effective_movement,
    reconcile_mechanic_plan,
    required_roles,
)
from ptv_orchestrator.pipeline.player_bindings import PlayerRoleError
from ptv_orchestrator.pipeline.player_experience import (
    strip_player_avatar_assets,
    sync_build_spec_experience,
)
from ptv_orchestrator.pipeline.stage_scene_compile import (
    _mesh_render_fields,
    _repoint_world_meshes_to_lods,
    compile_scene_json,
    reconcile_world_copies,
)
from ptv_orchestrator.pipeline.stage_world_generation import _validate_barriers
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
    MovementMode,
    MovementSpec,
    PlayerExperience,
    ReferenceImageRequest,
    SceneEntry,
    SceneSpec,
    TestEntry,
    Transform,
    TurnStyle,
    VignetteSpec,
)
from ptv_schemas.asset_manifest import AssetCategory, CharacterRole
from ptv_schemas.build_spec import AvatarCoverage, GameType, LocomotionStyle
from ptv_schemas.code_task import CodeTask, CodeTaskKind
from ptv_schemas.common import Vec3
from ptv_schemas.scene_spec import EntitySpec, MeshRef


def _spec(
    *,
    movement: MovementSpec | None = None,
    experience: PlayerExperience = PlayerExperience.full_avatar,
    assets: list[AssetEntry] | None = None,
    **extra,
) -> BuildSpec:
    kwargs = dict(
        prompt_id="demo",
        title="Demo",
        pitch="A demo game.",
        game_type=GameType.arcade,
        scenes=[SceneEntry(id="room", name="Room", summary="one room.",
                           entry_point=True)],
        mechanics=[MechanicEntry(id="grab", name="Grab", summary="grab")],
        assets=assets or [AssetEntry(id="cube", name="Cube", category="prop",
                                     summary="a cube")],
        acceptance_tests=[TestEntry(id="t1", description="works")],
        player_experience=experience,
        **extra,
    )
    if movement is not None:
        kwargs["movement"] = movement
    return BuildSpec(**kwargs)


def _stationary(turning: list[TurnStyle] | None = None,
                modes: list[MovementMode] | None = None,
                menu: bool = False) -> MovementSpec:
    return MovementSpec(modes=modes or [], turning=turning or [],
                        settings_menu=menu)


def _builtin(item_id: str, builtin_id: str, **config) -> MechanicPlanItem:
    return MechanicPlanItem(
        id=item_id,
        classification=(MechanicClassification.builtin_config if config
                        else MechanicClassification.builtin),
        builtin_id=builtin_id,
        config=config,
    )


def _plan(*items: MechanicPlanItem) -> MechanicPlan:
    # A plan needs one item; the game's own grab stands in for "the rest of
    # the game" and is never a player role.
    return MechanicPlan(
        prompt_id="demo",
        items=list(items) or [_builtin("grab", "builtin.grab")],
    )


def _builtins(plan: MechanicPlan) -> set[str]:
    return {it.builtin_id for it in plan.items
            if it.builtin_id and it.builtin_id != "builtin.grab"}


# ---------------------------------------------------------------------------
# Movement reconcile: the examples from the plan
# ---------------------------------------------------------------------------


def test_beat_saber_binds_only_hand_pose_and_no_menu():
    spec = _spec(movement=_stationary(),
                 experience=PlayerExperience.hands_only)
    out = reconcile_mechanic_plan(_plan(), spec)
    assert _builtins(out.plan) == {"builtin.hand_pose"}


def test_stationary_bow_with_snap_and_smooth_gets_turns_and_menu():
    spec = _spec(movement=_stationary([TurnStyle.snap, TurnStyle.smooth],
                                      menu=True))
    got = _builtins(reconcile_mechanic_plan(_plan(), spec).plan)
    assert got == {
        "builtin.hand_pose",
        "builtin.avatar_body",
        "builtin.locomotion.snap_turn",
        "builtin.locomotion.smooth_turn",
        "builtin.movement_settings_menu",
    }


def test_teleport_only_bow_adds_teleport_and_keeps_the_turning_menu():
    spec = _spec(movement=_stationary([TurnStyle.snap, TurnStyle.smooth],
                                      modes=[MovementMode.teleport],
                                      menu=True))
    got = _builtins(reconcile_mechanic_plan(_plan(), spec).plan)
    assert "builtin.locomotion.teleport" in got
    assert "builtin.movement_settings_menu" in got
    assert not {"builtin.locomotion.slide.head_relative",
                "builtin.locomotion.slide.controller_relative"} & got


def test_silent_travelling_game_gets_three_modes_snap_and_menu():
    # The MovementSpec default is the travelling default the prompt uses
    # when the game clearly travels and says nothing about how.
    spec = _spec(movement=MovementSpec())
    got = _builtins(reconcile_mechanic_plan(_plan(), spec).plan)
    assert {
        "builtin.locomotion.teleport",
        "builtin.locomotion.slide.head_relative",
        "builtin.locomotion.slide.controller_relative",
        "builtin.locomotion.snap_turn",
        "builtin.movement_settings_menu",
    } <= got
    assert "builtin.locomotion.smooth_turn" not in got


def test_llm_planned_teleport_is_removed_from_a_stationary_game():
    spec = _spec(movement=_stationary())
    plan = _plan(_builtin("teleport", "builtin.locomotion.teleport"),
                 _builtin("grab", "builtin.grab"))
    out = reconcile_mechanic_plan(plan, spec)
    assert "teleport" in out.removed
    assert "builtin.locomotion.teleport" not in _builtins(out.plan)
    assert "grab" in [it.id for it in out.plan.items]
    assert any("teleport" in n for n in out.notes)


def test_a_settings_menu_with_nothing_to_choose_is_not_added():
    # settings_menu: true but one mode and one turn style -> nothing on it.
    spec = _spec(movement=_stationary([TurnStyle.snap],
                                      modes=[MovementMode.teleport],
                                      menu=True))
    assert "role:movement_settings_menu" not in required_roles(spec)


def test_duplicate_role_items_collapse_and_a_replacement_wins():
    spec = _spec(movement=_stationary([TurnStyle.snap]))
    replacement = MechanicPlanItem(
        id="custom_snap",
        classification=MechanicClassification.builtin_config_code_edit,
        base_builtin_id="builtin.locomotion.snap_turn",
        code_task=CodeTask(id="custom_snap", kind=CodeTaskKind.mechanic,
                           title="Custom snap", description="x" * 40,
                           interface="IMechanic"),
    )
    plan = _plan(_builtin("snap_turn", "builtin.locomotion.snap_turn"),
                 _builtin("snap_turn_2", "builtin.locomotion.snap_turn"),
                 replacement)
    out = reconcile_mechanic_plan(plan, spec)
    ids = [it.id for it in out.plan.items]
    assert "custom_snap" in ids
    assert "snap_turn" not in ids and "snap_turn_2" not in ids


def test_vignette_config_follows_the_buildspec():
    mv = MovementSpec(modes=[MovementMode.slide_head], turning=[TurnStyle.smooth],
                      vignette=VignetteSpec(slide=True, smooth_turn=False,
                                            user_toggle=True))
    spec = _spec(movement=mv)
    plan = _plan(_builtin("slide", "builtin.locomotion.slide.head_relative",
                          vignette={"enabled": False, "strength": 0.5}))
    out = reconcile_mechanic_plan(plan, spec)
    slide = next(it for it in out.plan.items if it.id == "slide")
    assert slide.config["vignette"] == {"enabled": True, "user_toggle": True,
                                        "strength": 0.5}
    smooth = next(it for it in out.plan.items
                  if it.builtin_id == "builtin.locomotion.smooth_turn")
    assert smooth.config["vignette"]["enabled"] is False
    # A toggle exists (slide vignette + user_toggle), so the menu is added.
    assert "builtin.movement_settings_menu" in _builtins(out.plan)


def _copy_of(item_id: str, base: str, depends_on=None, **config) -> MechanicPlanItem:
    return MechanicPlanItem(
        id=item_id,
        classification=MechanicClassification.builtin_config_code_edit,
        base_builtin_id=base,
        config=config,
        code_task=CodeTask(id=item_id, kind=CodeTaskKind.mechanic,
                           title=item_id, description="x" * 40,
                           interface="IMechanic",
                           depends_on=list(depends_on or [])),
    )


def test_a_copy_that_only_adds_a_blink_becomes_the_builtin():
    spec = _spec(movement=_stationary([TurnStyle.snap],
                                      modes=[MovementMode.teleport]))
    plan = _plan(
        _copy_of("teleport_copy", "builtin.locomotion.teleport",
                 max_range_m=10.0, blink_fade_s=0.1),
        _copy_of("snap_copy", "builtin.locomotion.snap_turn",
                 degrees=30.0, allowed_degrees=[15, 30, 45], blink_fade_s=0.08),
        _copy_of("hud", "builtin.ui_panel", depends_on=["snap_copy"]),
    )
    out = reconcile_mechanic_plan(plan, spec)
    by_id = {it.id: it for it in out.plan.items}
    tp = by_id["teleport_copy"]
    assert tp.classification == MechanicClassification.builtin_config
    assert tp.builtin_id == "builtin.locomotion.teleport"
    assert tp.code_task is None and "blink_fade_s" not in tp.config
    snap = by_id["snap_copy"]
    assert snap.builtin_id == "builtin.locomotion.snap_turn"
    assert snap.config["allowed_degrees"] == [15, 30, 45]
    # The HUD waited on the snap copy's code task, which no longer exists.
    assert by_id["hud"].code_task.depends_on == []


def test_a_copy_with_real_custom_behaviour_keeps_its_code_but_not_the_blink():
    spec = _spec(movement=_stationary(modes=[MovementMode.teleport]))
    plan = _plan(_copy_of("pad_teleport", "builtin.locomotion.teleport",
                          only_onto_pads=True, fade_s=0.2))
    out = reconcile_mechanic_plan(plan, spec)
    item = next(it for it in out.plan.items if it.id == "pad_teleport")
    assert item.classification == MechanicClassification.builtin_config_code_edit
    assert "fade_s" not in item.config and item.config["only_onto_pads"] is True
    assert "NEVER blink" in item.code_task.description


@pytest.mark.parametrize(
    "legacy, modes",
    [
        (LocomotionStyle.stationary, []),
        (LocomotionStyle.teleport, [MovementMode.teleport]),
        (LocomotionStyle.smooth, [MovementMode.slide_head,
                                  MovementMode.slide_controller]),
        (LocomotionStyle.mixed, [MovementMode.teleport, MovementMode.slide_head,
                                 MovementMode.slide_controller]),
    ],
)
def test_legacy_buildspec_maps_locomotion_to_movement(legacy, modes):
    spec = _spec(locomotion=legacy)
    assert "movement" not in spec.model_fields_set
    assert effective_movement(spec).modes == modes


# ---------------------------------------------------------------------------
# scene_compile: explicit bindings on the right hosts
# ---------------------------------------------------------------------------


def _manifest(with_avatar: bool = False) -> AssetManifest:
    items = [
        AssetItem(id="cube", name="Cube", category=AssetCategory.prop,
                  style_notes="wood cube",
                  reference=ReferenceImageRequest(prompt="a cube"),
                  target_scale_meters=0.2),
    ]
    if with_avatar:
        items.append(
            AssetItem(id="hero", name="Hero", category=AssetCategory.character,
                      style_notes="the player's body",
                      character_role=CharacterRole.player_avatar, rigged=True,
                      reference=ReferenceImageRequest(prompt="a hero"),
                      target_scale_meters=1.8))
    return AssetManifest(prompt_id="demo", items=items)


def _scene(*entities: EntitySpec) -> SceneSpec:
    return SceneSpec(prompt_id="demo", scene_id="room", name="Room",
                     entities=list(entities))


def _avatar_entity(*bindings: str) -> EntitySpec:
    return EntitySpec(
        id="hero_body", name="Hero",
        transform=Transform(position=Vec3(x=0.0, y=0.0, z=0.0)),
        mesh=MeshRef(asset_id="hero", glb_path="projects/demo/hero.glb"),
        mechanic_bindings=[MechanicBinding(mechanic_id=b) for b in bindings],
        tags=["player_avatar"],
    )


def _by_id(doc: dict) -> dict[str, dict]:
    return {e["id"]: e for e in doc["entities"]}


def _binding_ids(entity: dict) -> list[str]:
    return [b["id"] for b in entity.get("mechanic_bindings") or []]


def test_full_avatar_binds_body_on_the_avatar_not_the_rig():
    spec = _spec(movement=_stationary([TurnStyle.snap]))
    plan = reconcile_mechanic_plan(_plan(), spec).plan
    doc = compile_scene_json(spec, _manifest(True), plan, _scene(_avatar_entity()),
                             {"hero": {"glb_path": "projects/demo/hero.glb"}})
    ents = _by_id(doc)
    assert "builtin.avatar_body" in _binding_ids(ents["hero_body"])
    rig = ents["player_rig"]
    assert "builtin.avatar_body" not in _binding_ids(rig)
    assert set(_binding_ids(rig)) == {"builtin.hand_pose",
                                      "builtin.locomotion.snap_turn"}
    assert "player_system" in rig["tags"]
    assert doc["player"]["hands"] == {"source": "avatar"}
    assert doc["player"]["locomotion_menu"] is False
    # BuildSpec avatar settings travel into the body's params.
    params = ents["hero_body"]["mechanic_bindings"][0]["params"]
    assert params["coverage"] == "full_body"


def test_hands_only_drops_the_avatar_and_uses_default_hands():
    spec = _spec(movement=_stationary(),
                 experience=PlayerExperience.hands_only)
    plan = reconcile_mechanic_plan(_plan(), spec).plan
    warnings: list[str] = []
    doc = compile_scene_json(spec, _manifest(True), plan, _scene(_avatar_entity()),
                             {"hero": {"glb_path": "projects/demo/hero.glb"}},
                             on_warning=warnings.append)
    ents = _by_id(doc)
    assert "hero_body" not in ents
    assert not any("player_avatar" in (e.get("tags") or [])
                   for e in doc["entities"])
    assert _binding_ids(ents["player_rig"]) == ["builtin.hand_pose"]
    assert doc["player"]["hands"] == {"source": "default"}
    assert "default_locomotion_mode" not in doc["player"]
    assert any("Hands Only" in w for w in warnings)


def test_a_second_body_mechanic_on_the_avatar_fails_the_compile():
    spec = _spec(movement=_stationary())
    body_copy = MechanicPlanItem(
        id="my_body",
        classification=MechanicClassification.builtin_config_code_edit,
        base_builtin_id="builtin.avatar_body",
        code_task=CodeTask(id="my_body", kind=CodeTaskKind.mechanic,
                           title="Body", description="x" * 40,
                           interface="IMechanic"),
    )
    plan = _plan(body_copy, _builtin("hand_pose", "builtin.hand_pose"))
    scene = _scene(_avatar_entity("builtin.avatar_body", "generated.my_body"))
    with pytest.raises(PlayerRoleError) as exc:
        compile_scene_json(spec, _manifest(True), plan, scene,
                           {"hero": {"glb_path": "projects/demo/hero.glb"}})
    assert "role:body" in str(exc.value)


def test_a_teleport_bound_in_a_stationary_game_fails_the_compile():
    spec = _spec(movement=_stationary())
    plan = _plan(_builtin("teleport", "builtin.locomotion.teleport"))
    scene = _scene(EntitySpec(
        id="player_rig", name="Rig",
        transform=Transform(position=Vec3(x=0.0, y=0.0, z=0.0)),
        mechanic_bindings=[MechanicBinding(mechanic_id="builtin.teleport")],
        tags=["player_rig"],
    ))
    with pytest.raises(PlayerRoleError) as exc:
        compile_scene_json(spec, _manifest(), plan, scene, {})
    assert "role:teleport" in str(exc.value)


def test_the_same_mechanic_bound_twice_collapses_to_one():
    spec = _spec(movement=_stationary([TurnStyle.snap]))
    plan = reconcile_mechanic_plan(_plan(), spec).plan
    snap = MechanicBinding(mechanic_id="builtin.snap_turn")
    scene = _scene(
        EntitySpec(id="player_rig", name="Rig",
                   transform=Transform(position=Vec3(x=0.0, y=0.0, z=0.0)),
                   mechanic_bindings=[snap], tags=["player_rig"]),
        EntitySpec(id="marker", name="Marker",
                   transform=Transform(position=Vec3(x=0.0, y=0.0, z=0.0)),
                   mechanic_bindings=[snap]),
    )
    doc = compile_scene_json(spec, _manifest(), plan, scene, {})
    count = sum(_binding_ids(e).count("builtin.snap_turn")
                for e in doc["entities"])
    assert count == 1


def test_default_modes_are_written_to_the_player_block():
    mv = MovementSpec(modes=[MovementMode.teleport, MovementMode.slide_head],
                      default_mode=MovementMode.slide_head,
                      turning=[TurnStyle.snap, TurnStyle.smooth],
                      default_turn=TurnStyle.smooth)
    spec = _spec(movement=mv, experience=PlayerExperience.hands_only)
    plan = reconcile_mechanic_plan(_plan(), spec).plan
    doc = compile_scene_json(spec, _manifest(), plan, _scene(), {})
    assert doc["player"]["default_locomotion_mode"] == "slide_head_relative"
    assert doc["player"]["default_turn_mode"] == "smooth"


def test_grab_gets_the_manifest_hold_offset():
    spec = _spec(movement=_stationary(), experience=PlayerExperience.hands_only)
    manifest = _manifest()
    manifest.items[0].hold_rotation_offset_deg = [0.0, 180.0, 0.0]
    plan = _plan(_builtin("grab", "builtin.grab"))
    scene = _scene(EntitySpec(
        id="cube1", name="Cube",
        transform=Transform(position=Vec3(x=0.0, y=1.0, z=0.0)),
        mesh=MeshRef(asset_id="cube", glb_path="projects/demo/cube.glb"),
        mechanic_bindings=[MechanicBinding(mechanic_id="builtin.grab")],
    ))
    doc = compile_scene_json(spec, manifest, plan, scene, {})
    b = _by_id(doc)["cube1"]["mechanic_bindings"][0]
    assert b["params"]["hold_rotation_offset_deg"] == [0.0, 180.0, 0.0]


# ---------------------------------------------------------------------------
# Player experience sync + the Hands Only avatar drop
# ---------------------------------------------------------------------------


def test_sync_pins_hands_only_coverage():
    spec = _spec(experience=PlayerExperience.full_avatar)
    out, changed = sync_build_spec_experience(spec, PlayerExperience.hands_only)
    assert changed
    assert out.player_experience == PlayerExperience.hands_only
    assert out.player_avatar.coverage == AvatarCoverage.hands_only
    again, changed = sync_build_spec_experience(out, PlayerExperience.hands_only)
    assert not changed and again is out


def test_hands_only_buildspec_loses_the_body_but_keeps_the_gauntlet():
    spec = _spec(
        experience=PlayerExperience.hands_only,
        assets=[
            AssetEntry(id="hero_avatar", name="Player Avatar",
                       category="character", summary="the player avatar body"),
            AssetEntry(id="gauntlet", name="Gauntlet", category="prop",
                       summary="a repulsor gauntlet"),
        ],
    )
    out, dropped = strip_player_avatar_assets(spec)
    assert dropped == ["hero_avatar"]
    assert [a.id for a in out.assets] == ["gauntlet"]


class _ListBus(EventBus):
    def __init__(self) -> None:
        super().__init__()
        self.events: list[tuple[str, str]] = []

    def publish(self, event) -> None:  # type: ignore[override]
        self.events.append((getattr(event, "status", ""),
                            getattr(event, "message", "")))


def test_manifest_stage_drops_the_player_avatar_in_hands_only(tmp_path: Path,
                                                               monkeypatch):
    settings = Settings(repo_root=tmp_path, projects_dir=tmp_path / "projects")
    store = ProjectStore(settings.projects_dir)
    state = store.create("Hands", "a prompt long enough to pass validation",
                         player_experience=PlayerExperience.hands_only)
    ctx = PipelineContext(project_id=state.project_id, settings=settings,
                          store=store, bus=_ListBus())
    ctx.build_spec = _spec(experience=PlayerExperience.hands_only)
    returns = _manifest(with_avatar=True)

    class _Fake:
        def generate_json(self, **kwargs):
            return SimpleNamespace(data=returns.model_dump(mode="json"))

    monkeypatch.setattr(stage_asset_manifest, "make_llm", lambda _s: _Fake())
    stage_asset_manifest.run(ctx)
    saved = store.load_asset_manifest(state.project_id)
    assert [i.id for i in saved.items] == ["cube"]
    assert store.load_state(state.project_id).player_experience == (
        PlayerExperience.hands_only
    )


# ---------------------------------------------------------------------------
# Object size and world copies
# ---------------------------------------------------------------------------


def test_unit_scale_comes_from_lod0():
    out = _mesh_render_fields({"lod0_longest_axis": 2.0}, 0.3)
    assert out["unit_scale"] == pytest.approx(0.15)
    assert "unit_scale" not in _mesh_render_fields({}, 0.3)


def test_world_meshes_take_the_manifest_scale():
    ents = [{"id": "rack_w", "mesh": {"asset_id": "rack",
                                      "target_scale_meters": 1.0}}]
    nrep = {"rack": {"glb_path": "lod0.glb", "lod_paths": ["lod0.glb"],
                     "lod0_longest_axis": 2.0}}
    out = _repoint_world_meshes_to_lods(ents, nrep, None, {"rack": 1.15})
    assert out[0]["mesh"]["target_scale_meters"] == 1.15
    assert out[0]["mesh"]["unit_scale"] == pytest.approx(1.15 / 2.0)


def test_world_copies_inherit_bindings_and_duplicates_are_dropped():
    gameplay = [{
        "id": "pistol_1",
        "transform": {"position": [0.0, 1.0, 0.0]},
        "mesh": {"asset_id": "pistol"},
        "physics": {"kind": "dynamic", "mass_kg": 1.0},
        "mechanic_bindings": [{"id": "builtin.grab", "builtin_id": "builtin.grab",
                               "params": {}}],
        "tags": ["grabbable"],
    }]
    world = [
        {"id": "pistol_on_table", "transform": {"position": [5.0, 1.0, 0.0]},
         "mesh": {"asset_id": "pistol"},
         "physics": {"kind": "static", "mass_kg": 0.0}, "tags": []},
        {"id": "pistol_dupe", "transform": {"position": [0.1, 1.0, 0.0]},
         "mesh": {"asset_id": "pistol"}, "tags": []},
        {"id": "wall", "transform": {"position": [0.0, 0.0, 0.0]}},
    ]
    out = {e["id"]: e for e in reconcile_world_copies(world, gameplay)}
    assert "pistol_dupe" not in out
    copy = out["pistol_on_table"]
    assert _binding_ids(copy) == ["builtin.grab"]
    assert copy["physics"]["kind"] == "dynamic"
    assert "grabbable" in copy["tags"]
    assert "wall" in out


# ---------------------------------------------------------------------------
# Walls
# ---------------------------------------------------------------------------


def _wall(**over) -> dict:
    e = {
        "id": "wall_north",
        "primitive": {"kind": "box", "size": [8.0, 3.0, 0.4]},
        "physics": {"kind": "static", "collision_shape": "box"},
        "transform": {"position": [0.0, 1.5, -4.0], "scale": 1.0},
        "tags": [],
    }
    e.update(over)
    return e


def test_a_solid_wall_passes():
    assert _validate_barriers({"entities": [_wall()]}) == []


def test_a_wall_without_collision_fails_unless_walkthrough():
    assert _validate_barriers({"entities": [_wall(physics=None)]})
    assert _validate_barriers(
        {"entities": [_wall(physics=None, tags=["walkthrough"])]}
    ) == []


def test_a_paper_thin_wall_fails():
    thin = _wall(primitive={"kind": "box", "size": [8.0, 3.0, 0.05]})
    problems = _validate_barriers({"entities": [thin]})
    assert problems and "thick" in problems[0]


def test_wall_dressing_is_not_a_barrier():
    trim = _wall(id="wall_trim_north", physics=None,
                 primitive={"kind": "box", "size": [8.0, 3.0, 0.02]})
    assert _validate_barriers({"entities": [trim]}) == []
