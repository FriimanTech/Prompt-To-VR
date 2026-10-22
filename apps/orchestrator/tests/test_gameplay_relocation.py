"""The gameplay layer moves with the player when the world moves the spawn."""

from __future__ import annotations

import math

from ptv_orchestrator.pipeline.gameplay_relocation import (
    relocate_gameplay_layer,
    resolve_overlaps,
    yaw_from_direction,
    yaw_from_quat,
)
from ptv_orchestrator.pipeline.stage_scene_compile import _merge_world_into_scene


def _ent(eid, pos, tags=(), **extra):
    e = {"id": eid, "tags": list(tags),
         "transform": {"position": list(pos), "rotation": [0, 0, 0, 1],
                       "scale": 1.0}}
    e.update(extra)
    return e


def _dist(a, b):
    return math.hypot(a[0] - b[0], a[2] - b[2])


def test_yaw_conventions_agree() -> None:
    for deg in (0, 45, 90, 163, -120):
        th = math.radians(deg)
        q = [0.0, math.sin(th / 2), 0.0, math.cos(th / 2)]
        fwd = [-math.sin(th), 0.0, -math.cos(th)]
        assert abs(yaw_from_quat(q) - math.atan2(math.sin(th), math.cos(th))) < 1e-6
        assert abs(yaw_from_direction(fwd) - math.atan2(math.sin(th), math.cos(th))) < 1e-6


def test_layer_keeps_its_layout_around_the_new_spawn() -> None:
    """The goblin game: composed around the origin facing +Z, the world put
    the player at (2.2, 0.05, -30) facing elsewhere."""
    ally = _ent("ally", [0.6, 0.0, -2.2], ["npc", "ally"])
    blaster = _ent("blaster", [0.65, 1.08, 0.65], ["grabbable"])
    template = _ent("goblin", [0, -50, 20], ["enemy", "template"])
    avatar = _ent("avatar", [0, 0, 0], ["player_avatar"])
    club = _ent("club", [0, 0, 0], attached_to={"entity_id": "goblin"})
    ents = [ally, blaster, template, avatar, club]
    world_q = [0.0, -0.9894, 0.0, 0.1452]
    moved = relocate_gameplay_layer(
        ents, [0.0, 1.7, 0.0], yaw_from_direction([0, 0, 1]),
        [2.2, 0.05, -30.0], yaw_from_quat(world_q))
    assert moved == 2
    spawn = [2.2, 0.05, -30.0]
    # Distances to the player are preserved...
    assert abs(_dist(ally["transform"]["position"], spawn) - math.hypot(0.6, 2.2)) < 1e-6
    assert abs(_dist(blaster["transform"]["position"], spawn) - math.hypot(0.65, 0.65)) < 1e-6
    # ...heights stay relative to the floor...
    assert abs(blaster["transform"]["position"][1] - (1.08 + 0.05)) < 1e-6
    # ...and templates, the avatar and attached children are left alone.
    assert template["transform"]["position"] == [0, -50, 20]
    assert avatar["transform"]["position"] == [0, 0, 0]
    assert club["transform"]["position"] == [0, 0, 0]


def test_layer_is_rotated_with_the_spawn() -> None:
    """What was in front of the composed spawn is in front of the world's."""
    ahead = _ent("panel", [0.0, 1.5, 2.0])  # 2 m ahead when facing +Z
    world_yaw = yaw_from_direction([1, 0, 0])  # world spawn faces +X
    relocate_gameplay_layer([ahead], [0, 0, 0], yaw_from_direction([0, 0, 1]),
                            [10.0, 0.0, 10.0], world_yaw)
    p = ahead["transform"]["position"]
    assert abs(p[0] - 12.0) < 1e-6 and abs(p[2] - 10.0) < 1e-6


def test_same_spawn_moves_nothing() -> None:
    e = _ent("x", [1, 0, 1])
    assert relocate_gameplay_layer([e], [0, 1.7, 0], 0.0, [0.1, 0.0, 0.0], 0.0) == 0
    assert e["transform"]["position"] == [1, 0, 1]


def test_characters_are_moved_out_of_walls_onto_walkable_floor() -> None:
    wall = {"id": "wall", "physics": {"kind": "static"},
            "primitive": {"kind": "box", "size": [1.0, 3.0, 10.0]},
            "transform": {"position": [0.0, 1.5, 0.0], "rotation": [0, 0, 0, 1]}}
    floor = {"id": "floor", "physics": {"kind": "static"},
             "primitive": {"kind": "box", "size": [50.0, 0.2, 50.0]},
             "transform": {"position": [0.0, -0.1, 0.0], "rotation": [0, 0, 0, 1]}}
    regions = [{"id": "east", "min": [1.0, 0, -5], "max": [10, 0, 5], "floor_y": 0.0}]
    npc = _ent("ally", [0.2, 0.0, 0.0], ["npc", "ally"])
    crate = _ent("crate", [5.0, 0.0, 0.0])
    moved = resolve_overlaps([npc, crate], [wall, floor], regions)
    assert moved == 1
    p = npc["transform"]["position"]
    assert p[0] >= 1.0  # out of the wall, onto the east region
    assert crate["transform"]["position"] == [5.0, 0.0, 0.0]


def test_merge_moves_the_layer_and_takes_the_world_spawn() -> None:
    gameplay = {
        "entities": [_ent("ally", [0.0, 0.0, -2.0], ["npc"])],
        "player": {"spawn": {"position": [0, 1.7, 0], "rotation": [0, 0, 0, 1]}},
    }
    world = {"entities": [],
             "player": {"spawn": {"position": [20.0, 0.0, 0.0],
                                  "rotation": [0, 0, 0, 1]}}}
    merged = _merge_world_into_scene(gameplay, world)
    ally = next(e for e in merged["entities"] if e["id"] == "ally")
    assert ally["transform"]["position"] == [20.0, 0.0, -2.0]
    assert merged["player"]["spawn"]["position"] == [20.0, 0.0, 0.0]
