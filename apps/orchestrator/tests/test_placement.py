"""World-aware placement: items rest exactly on the anchors the world
declared, sunk props are lifted onto their supports, and anything still
wrong is reported."""

from __future__ import annotations

from types import SimpleNamespace

from ptv_orchestrator.pipeline import placement


def _table(eid="table", pos=(0.0, 0.0, 0.0), size=(1.4, 0.9, 0.8)):
    return {"id": eid, "transform": {"position": list(pos), "rotation": [0, 0, 0, 1],
                                     "scale": 1.0},
            "primitive": {"kind": "box", "size": list(size)},
            "physics": {"kind": "static", "collision_shape": "box"}, "tags": []}


def _ground():
    return {"id": "ground", "transform": {"position": [0, -0.1, 0], "rotation": [0, 0, 0, 1]},
            "primitive": {"kind": "box", "size": [100, 0.2, 100]},
            "physics": {"kind": "static", "collision_shape": "box"}, "tags": ["ground"]}


def _prop(eid, asset, pos, lo, hi, kind="dynamic", placement_=None):
    e = {"id": eid, "transform": {"position": list(pos), "rotation": [0, 0, 0, 1], "scale": 1.0},
         "mesh": {"asset_id": asset, "collision": {"aabb_min": list(lo), "aabb_max": list(hi)}},
         "physics": {"kind": kind, "collision_shape": "mesh"}, "tags": []}
    if placement_:
        e["placement"] = placement_
    return e


def test_on_surface_rests_the_real_bottom_on_the_anchor():
    table = _table(pos=(5.0, 0.45, 0.0))
    glock = _prop("glock", "glock", (0, 0, 0), (-0.08, -0.1, -0.11), (0.08, 0.1, 0.11),
                  placement_={"relation": "on_surface", "anchor": "table_glock"})
    doc = {"entities": [_ground(), table, glock],
           "anchors": [{"id": "table_glock", "position": [5.2, 0.9, 0.1],
                        "facing": [0, 0, 1], "relation": "on_surface"}]}
    report = placement.resolve(doc)
    assert report["resolved"][0]["anchor"] == "table_glock"
    pos = glock["transform"]["position"]
    assert abs(pos[1] - (0.9 + 0.1 + placement.REST_CLEARANCE_M)) < 1e-4
    assert abs(pos[0] - 5.2) < 1e-4 and abs(pos[2] - 0.1) < 1e-4
    assert "settled" in glock["tags"]
    # Facing +Z: yaw pi.
    assert abs(abs(glock["transform"]["rotation"][1]) - 1.0) < 1e-4
    errors, _ = placement.violations(doc)
    assert errors == []


def test_hang_and_wall_mount():
    rail = _table("rail", pos=(0, 5.0, 0), size=(4, 0.2, 0.2))
    target = _prop("target", "t", (0, 0, 0), (-0.4, -0.9, -0.1), (0.4, 0.9, 0.1),
                   kind="static", placement_={"relation": "hang_from", "anchor": "hook"})
    wall = _table("wall", pos=(0, 2.0, -5.0), size=(6, 4, 0.2))
    board = _prop("board", "b", (0, 0, 0), (-0.5, -0.3, -0.05), (0.5, 0.3, 0.05),
                  kind="static", placement_={"relation": "mount_on_wall", "anchor": "wall_spot"})
    doc = {"entities": [_ground(), rail, target, wall, board],
           "anchors": [{"id": "hook", "position": [0, 4.9, 0], "relation": "hang_from"},
                       {"id": "wall_spot", "position": [1.0, 2.5, -4.9],
                        "normal": [0, 0, 1], "relation": "mount_on_wall"}]}
    placement.resolve(doc)
    assert abs(target["transform"]["position"][1] - (4.9 - 0.9)) < 1e-4
    bp = board["transform"]["position"]
    # Back face on the wall plane z = -4.9, centred on the anchor.
    assert abs(bp[2] - (-4.9 + 0.05)) < 1e-4 and abs(bp[1] - 2.5) < 1e-4
    errors, _ = placement.violations(doc)
    assert errors == []


def test_validate_anchors_names_what_is_missing_or_unsupported():
    table = _table(pos=(0, 0.45, 0))
    doc = {"entities": [_ground(), table],
           "anchors": [{"id": "floating", "position": [0, 1.4, 0], "relation": "on_surface"},
                       {"id": "edge", "position": [0.68, 0.9, 0], "relation": "on_surface"}],
           "anchor_sets": [{"id": "pads", "relation": "on_floor", "points": [[3, 0, 3]]}]}
    reqs = [
        {"entity": "a", "asset_id": "x", "relation": "on_surface", "anchor": "missing"},
        {"entity": "b", "asset_id": "x", "relation": "on_surface", "anchor": "floating"},
        {"entity": "c", "asset_id": "x", "relation": "on_surface", "anchor": "edge",
         "size_m": [0.3, 0.1, 0.2]},
        {"entity": "d", "asset_id": "x", "relation": "on_floor", "anchor_set": "pads",
         "count": 4},
    ]
    problems = placement.validate_anchors(doc, reqs)
    text = "\n".join(problems)
    assert "'missing'" in text and "was not declared" in text
    assert "'floating'" in text and "nothing to rest on" in text
    assert "'edge'" in text and "footprint does not fit" in text
    assert "'pads' has 1 point(s); 4 requested" in text


def test_settle_lifts_a_centred_table_and_the_gun_on_it():
    table = _prop("table", "table", (0, 0, 0), (-0.7, -0.47, -0.7), (0.7, 0.47, 0.7),
                  kind="static")
    gun = _prop("gun", "gun", (0.1, 0.95, 0.0), (-0.1, -0.1, -0.05), (0.1, 0.1, 0.05))
    doc = {"entities": [_ground(), table, gun]}
    assert placement.settle(doc) == 2
    assert abs(table["transform"]["position"][1] - (0.47 + placement.REST_CLEARANCE_M)) < 1e-4
    table_top = table["transform"]["position"][1] + 0.47
    assert abs(gun["transform"]["position"][1] - 0.1 - table_top) < 0.005
    assert "settled" in gun["tags"]


def test_a_rifle_standing_on_its_edge_is_laid_on_its_side():
    """The Navy SEAL rifles: 9 cm wide, 29 cm tall as placed -- physics
    toppled them 22 cm the moment the game started."""
    table = _table(pos=(0, 0.45, 0))
    rifle = _prop("rifle", "m16", (0.0, 1.05, 0.0), (-0.045, -0.147, -0.5), (0.045, 0.147, 0.5))
    rifle["tags"] = ["grabbable"]
    rifle["transform"]["rotation"] = [0.0, 1.0, 0.0, 0.0]   # yaw 180
    doc = {"entities": [_ground(), table, rifle]}
    placement.settle(doc)
    lo, hi = placement.world_bounds(rifle)
    assert hi[1] - lo[1] < 0.1                       # lying on its side
    assert hi[2] - lo[2] > 0.9                       # still long along z
    assert abs(lo[1] - 0.9) < 0.005                  # resting on the table top
    assert abs(abs(placement.yaw_of(rifle["transform"]["rotation"])) - 3.14159) < 1e-3
    # A flat item and a static one are left alone.
    pad = _prop("pad", "pad", (2, 0.05, 0), (-0.2, -0.02, -0.2), (0.2, 0.02, 0.2))
    pad["tags"] = ["grabbable"]
    assert not placement.lay_flat(pad)
    post = _prop("post", "post", (3, 1, 0), (-0.05, -1, -0.05), (0.05, 1, 0.05), kind="static")
    assert not placement.lay_flat(post)


def test_a_rifle_upright_on_its_magazine_is_laid_flat_by_its_footprint():
    """The SCAR: 20 cm wide, 30 cm tall -- under the 1.5 ratio rule -- but
    balanced on a 4 x 9 cm magazine patch, so it toppled 20 cm."""
    rifle = _prop("scar", "scar", (0.0, 1.05, 0.0), (-0.1, -0.15, -0.45), (0.1, 0.15, 0.45))
    rifle["tags"] = ["grabbable"]
    hull = [[x, 0.15, z] for x in (-0.1, 0.1) for z in (-0.45, 0.45)]       # top rail
    hull += [[x, 0.0, z] for x in (-0.1, 0.1) for z in (-0.45, 0.45)]       # body
    hull += [[x, -0.15, z] for x in (-0.02, 0.02) for z in (0.0, 0.09)]     # magazine foot
    rifle["mesh"]["collision"]["hull"] = hull
    assert placement.tips_over(rifle)
    assert placement.lay_flat(rifle)
    lo, hi = placement.world_bounds(rifle)
    assert hi[1] - lo[1] < 0.21
    # Lying flat it is left alone.
    assert not placement.lay_flat(rifle)


def test_a_loose_item_in_mid_air_is_rested_on_what_is_under_it():
    """The Navy SEAL rifles: authored at chest height by the spawn, dropped
    by physics on frame one and left rolling on the sand."""
    rifle = _prop("m16", "m16", (0.0, 1.4, 0.0), (-0.045, -0.05, -0.5), (0.045, 0.05, 0.5))
    rifle["tags"] = ["grabbable"]
    held = _prop("held", "m16", (2.0, 1.4, 0.0), (-0.045, -0.05, -0.5), (0.045, 0.05, 0.5))
    held["tags"] = ["grabbable"]
    held["attached_to"] = {"entity": "player_rig", "socket": "hand_r"}
    lamp = _prop("lamp", "lamp", (4.0, 2.0, 0.0), (-0.1, -0.1, -0.1), (0.1, 0.1, 0.1))
    doc = {"entities": [_ground(), rifle, held, lamp]}
    placement.settle(doc)
    lo, _ = placement.world_bounds(rifle)
    assert abs(lo[1] - 0.0) < 0.005 and "settled" in rifle["tags"]
    assert held["transform"]["position"][1] == 1.4      # attached: not ours to move
    assert lamp["transform"]["position"][1] == 2.0      # not a loose item


def test_a_loose_item_sunk_deep_into_a_table_goes_on_top_not_under_it():
    """The range glock: its bottom 10 cm below the table top, under the 60 %
    search line -- the floor under the table read as its support and the
    float rule put it inside the table."""
    table = _table(pos=(0, 0.45, 0))
    glock = _prop("glock", "glock", (0.0, 0.85, 0.0), (-0.09, -0.07, -0.1), (0.09, 0.07, 0.1))
    glock["tags"] = ["grabbable"]
    pad = _prop("pad", "pad", (0.0, 0.035, 0.0), (-2, -0.035, -2), (2, 0.035, 2), kind="static")
    doc = {"entities": [_ground(), pad, table, glock]}
    placement.settle(doc)
    lo, _ = placement.world_bounds(glock)
    assert abs(lo[1] - 0.9) < 0.005


def test_anchored_items_rest_on_the_real_table_top_once_the_table_settles():
    """The rerun: the world's anchors were at 0.90 m, the table prop's
    measured top settled at 0.94 m, and every gun sat 4 cm into it."""
    table = _prop("table", "equipment_table", (5.0, 0.0, 0.0), (-0.7, -0.47, -0.4),
                  (0.7, 0.47, 0.4), kind="static")
    glock = _prop("glock", "glock", (0, 0, 0), (-0.08, -0.05, -0.11), (0.08, 0.05, 0.11),
                  placement_={"relation": "on_surface", "anchor": "table_glock"})
    doc = {"entities": [_ground(), table, glock],
           "anchors": [{"id": "table_glock", "position": [5.2, 0.90, 0.1],
                        "facing": [0, 0, 1], "relation": "on_surface"}]}
    placement.resolve(doc)
    placement.settle(doc)
    t_lo, t_hi = placement.world_bounds(table)
    g_lo, _ = placement.world_bounds(glock)
    assert abs(t_hi[1] - 0.94) < 0.01
    assert abs(g_lo[1] - t_hi[1]) < 0.005
    errors, _ = placement.violations(doc)
    assert errors == []


def test_violations_report_floating_sunk_and_overlaps():
    floating = _prop("f", "x", (0, 1.0, 0), (-0.1, -0.1, -0.1), (0.1, 0.1, 0.1),
                     placement_={"relation": "on_floor", "anchor": "a1"})
    a = _prop("a", "x", (3, 0.1, 0), (-0.1, -0.1, -0.1), (0.1, 0.1, 0.1),
              placement_={"relation": "on_floor", "anchor": "a2"})
    b = _prop("b", "x", (3.05, 0.1, 0), (-0.1, -0.1, -0.1), (0.1, 0.1, 0.1))
    doc = {"entities": [_ground(), floating, a, b],
           "anchors": [{"id": "a1", "position": [0, 0, 0]}, {"id": "a2", "position": [3, 0, 0]}]}
    errors, _ = placement.violations(doc)
    text = "\n".join(errors)
    assert "'f': floats" in text
    assert "'a' and 'b' interpenetrate" in text


def test_world_owns_static_furniture_only():
    manifest = SimpleNamespace(items=[
        SimpleNamespace(id="table", category=SimpleNamespace(value="prop")),
        SimpleNamespace(id="button", category=SimpleNamespace(value="prop")),
        SimpleNamespace(id="target", category=SimpleNamespace(value="prop")),
        SimpleNamespace(id="goblin", category=SimpleNamespace(value="character")),
    ])
    design = SimpleNamespace(items=[
        SimpleNamespace(asset_id="table", usage=SimpleNamespace(value="static_prop"),
                        destructible=None),
        SimpleNamespace(asset_id="button", usage=SimpleNamespace(value="pressable"),
                        destructible=None),
        SimpleNamespace(asset_id="target", usage=SimpleNamespace(value="static_prop"),
                        destructible=SimpleNamespace(max_health=50)),
    ])
    assert placement.world_owned_assets(manifest, design) == {"table"}
