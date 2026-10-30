"""Socket solvers on synthetic box-built meshes whose answers are known.

The vision pass is replaced by regions taken straight from the boxes each
part was built from, so these tests check the geometry exactly: a pistol's
palm lands on the handle's axis and its thumb arrow points at the slide, a
muzzle sits at the bore facing out of it, a rifle's support grip is under the
handguard, a sword's grip faces its blade, a shield's straps land on its
back, and the mask decoder picks the coordinate convention the model used.
"""

from __future__ import annotations

import numpy as np
import pytest

from ptv_schemas import (
    FireSystem,
    HoldKind,
    HoldProfile,
    ItemInteraction,
    ItemUsage,
    MeleeSpec,
    ShieldSpec,
    SocketKind,
    SocketSpec,
    TwoHandMode,
    WeaponSpec,
)
from ptv_orchestrator.pipeline.sockets.fusion import Fuser, Region
from ptv_orchestrator.pipeline.sockets.solvers import point_inside, solve_item
from ptv_orchestrator.pipeline.sockets.views import MeshData, ViewData, ViewSet
from ptv_orchestrator.pipeline.sockets.vision import decode_mask


def _box(lo, hi):
    lo, hi = np.array(lo, float), np.array(hi, float)
    c = np.array([[x, y, z] for x in (lo[0], hi[0]) for y in (lo[1], hi[1])
                  for z in (lo[2], hi[2])])
    # Subdivide each face 4x4 so regions have plenty of points.
    pts, tris = [], []
    faces = [(0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)]
    for f in faces:
        a, b, cc, d = (c[i] for i in f)
        base = len(pts)
        n = 4
        for i in range(n + 1):
            for j in range(n + 1):
                u, v = i / n, j / n
                pts.append((1 - u) * ((1 - v) * a + v * b) + u * ((1 - v) * d + v * cc))
        for i in range(n):
            for j in range(n):
                p0 = base + i * (n + 1) + j
                p1, p2, p3 = p0 + 1, p0 + n + 1, p0 + n + 2
                tris += [(p0, p2, p1), (p1, p2, p3)]
    return np.array(pts), np.array(tris)


def _mesh(*boxes):
    pts, tris, owner = [], [], []
    for k, (lo, hi) in enumerate(boxes):
        p, t = _box(lo, hi)
        tris.append(t + sum(len(x) for x in pts))
        pts.append(p)
        owner += [k] * len(t)
    return MeshData(np.vstack(pts), np.vstack(tris)), np.array(owner)


def _regions(mesh: MeshData, owner: np.ndarray, names: dict[str, list[int]]):
    vs = ViewSet(out_dir=None, mesh=mesh, views=[], meta={"longest_axis": 1.0})
    f = Fuser(vs)
    return {name: f._make(name, np.flatnonzero(np.isin(owner, ks)),
                          np.ones(mesh.tri_count), 3)
            for name, ks in names.items()}


# A pistol in metres: slide along +X (muzzle at +X), handle below the rear.
SLIDE = ((-0.08, 0.0, -0.015), (0.10, 0.035, 0.015))
HANDLE = ((-0.075, -0.11, -0.016), (-0.035, 0.0, 0.016))


def _pistol_item():
    return ItemInteraction(
        asset_id="pistol", usage=ItemUsage.held_weapon,
        hold=HoldProfile(kind=HoldKind.two_hand_wrap, aim_aligned=True,
                         two_hand=TwoHandMode.optional, primary_socket="grip",
                         support_socket="grip_wrap"),
        sockets=[SocketSpec(name="grip", kind=SocketKind.hand_grip, part_description="handle"),
                 SocketSpec(name="grip_wrap", kind=SocketKind.support_wrap,
                            part_description="handle"),
                 SocketSpec(name="muzzle", kind=SocketKind.muzzle, part_description="barrel end")],
        weapon=WeaponSpec(fire_system=FireSystem.hybrid))


def test_pistol_grip_is_on_the_handle_axis_and_points_at_the_slide() -> None:
    mesh, owner = _mesh(SLIDE, HANDLE)
    muzzle_tris = owner == 0
    regions = _regions(mesh, owner, {"grip": [1], "barrel": [0]})
    # The muzzle region: the slide's front face.
    vs = ViewSet(out_dir=None, mesh=mesh, views=[], meta={"longest_axis": 1.0})
    front = np.flatnonzero(muzzle_tris & (mesh.centroids[:, 0] > 0.099))
    regions["muzzle"] = Fuser(vs)._make("muzzle", front, np.ones(mesh.tri_count), 2)
    res = solve_item(_pistol_item(), regions, mesh, metres_per_unit=1.0)
    g = res.sockets["grip"]
    # On the handle's axis (x = -0.055, z = 0), below the slide.
    assert abs(g.position[0] + 0.055) < 0.004 and abs(g.position[2]) < 0.003
    assert -0.08 < g.position[1] < -0.02
    X, Y, Z = g.rotation[:, 0], g.rotation[:, 1], g.rotation[:, 2]
    # -Z (little finger -> thumb) points up toward the slide.
    assert np.dot(-Z, [0, 1, 0]) > 0.98
    # -Y (fingers' side) points forward toward the muzzle (trigger side).
    assert np.dot(-Y, [1, 0, 0]) > 0.98
    # +X is the gun's right; right-handed frame.
    assert np.allclose(np.cross(X, Y), Z, atol=1e-6)
    assert point_inside(mesh, g.position)
    m = res.sockets["muzzle"]
    assert abs(m.position[0] - 0.10) < 0.002
    assert np.dot(m.rotation[:, 2], [1, 0, 0]) > 0.98
    assert np.dot(m.rotation[:, 1], [0, 1, 0]) > 0.9
    # A filled front face (not a bore ring): between the face's half and full size.
    assert 0.01 < m.extras["bore_diameter"] < 0.05
    assert res.forward is not None and np.dot(res.forward, [1, 0, 0]) > 0.98
    w = res.sockets["grip_wrap"]
    assert w.extras["wrap_of"] == "grip" and w.extras["lateral_offset"] > 0.02
    assert not res.warnings


def test_rifle_support_grip_is_under_the_handguard_palm_up() -> None:
    body = ((-0.3, 0.0, -0.02), (0.5, 0.06, 0.02))
    handle = ((-0.12, -0.1, -0.015), (-0.08, 0.0, 0.015))
    guard = ((0.12, -0.03, -0.02), (0.38, 0.0, 0.02))
    mesh, owner = _mesh(body, handle, guard)
    regions = _regions(mesh, owner, {"grip": [1], "barrel": [0], "support": [2]})
    item = ItemInteraction(
        asset_id="rifle", usage=ItemUsage.held_weapon,
        hold=HoldProfile(kind=HoldKind.two_hand_line, aim_aligned=True,
                         primary_socket="grip", support_socket="support"),
        sockets=[SocketSpec(name="grip", kind=SocketKind.hand_grip, part_description="part g"),
                 SocketSpec(name="support", kind=SocketKind.support_grip,
                            part_description="handguard"),
                 SocketSpec(name="muzzle", kind=SocketKind.muzzle, part_description="part m")],
        weapon=WeaponSpec(fire_system=FireSystem.hybrid))
    vs = ViewSet(out_dir=None, mesh=mesh, views=[], meta={"longest_axis": 1.0})
    front = np.flatnonzero((owner == 0) & (mesh.centroids[:, 0] > 0.499))
    regions["muzzle"] = Fuser(vs)._make("muzzle", front, np.ones(mesh.tri_count), 2)
    res = solve_item(item, regions, mesh, metres_per_unit=1.0)
    s = res.sockets["support"]
    assert 0.2 < s.position[0] < 0.3
    assert s.extras["mirror"] == "flip_z180" and s.extras["mode"] == "under"
    # Right-hand palm faces up: +X (into the palm) points down.
    assert np.dot(s.rotation[:, 0], [0, -1, 0]) > 0.95
    assert np.dot(-s.rotation[:, 2], [1, 0, 0]) > 0.95


def test_sword_grip_thumb_points_at_the_blade() -> None:
    hilt = ((-0.012, -0.2, -0.012), (0.012, 0.0, 0.012))
    blade = ((-0.03, 0.02, -0.004), (0.03, 0.9, 0.004))
    mesh, owner = _mesh(hilt, blade)
    regions = _regions(mesh, owner, {"grip": [0], "blade": [1]})
    item = ItemInteraction(
        asset_id="sword", usage=ItemUsage.held_weapon,
        hold=HoldProfile(kind=HoldKind.one_hand, primary_socket="grip"),
        sockets=[SocketSpec(name="grip", kind=SocketKind.hand_grip, part_description="hilt"),
                 SocketSpec(name="strike_base", kind=SocketKind.strike_base, part_description="part b"),
                 SocketSpec(name="strike_tip", kind=SocketKind.strike_tip, part_description="part t")],
        melee=MeleeSpec())
    res = solve_item(item, regions, mesh, metres_per_unit=1.0)
    g = res.sockets["grip"]
    assert np.dot(-g.rotation[:, 2], [0, 1, 0]) > 0.98
    assert abs(g.position[1] + 0.1) < 0.02
    assert res.sockets["strike_tip"].position[1] > 0.88
    assert res.sockets["strike_base"].position[1] < 0.06
    assert np.dot(res.sockets["strike_tip"].rotation[:, 2], [0, 1, 0]) > 0.98


def test_shield_straps_are_synthesized_on_the_back_when_not_modelled() -> None:
    # A domed shield: a big thin disc-ish box plus a boss on the FRONT (+Z).
    plate = ((-0.35, -0.35, -0.01), (0.35, 0.35, 0.01))
    boss = ((-0.1, -0.1, 0.01), (0.1, 0.1, 0.06))
    mesh, owner = _mesh(plate, boss)
    item = ItemInteraction(
        asset_id="shield", usage=ItemUsage.held_shield,
        hold=HoldProfile(kind=HoldKind.forearm_strap, primary_socket="hand_strap",
                         forearm_socket="forearm_strap"),
        sockets=[SocketSpec(name="hand_strap", kind=SocketKind.hand_strap, part_description="part h"),
                 SocketSpec(name="forearm_strap", kind=SocketKind.forearm_strap,
                            part_description="part f")],
        shield=ShieldSpec())
    regions = _regions(mesh, owner, {"front_face": [1]})
    res = solve_item(item, regions, mesh, metres_per_unit=1.0)
    h, f = res.sockets["hand_strap"], res.sockets["forearm_strap"]
    assert h.position[2] < -0.01 and f.position[2] < -0.01       # on the back
    assert h.confidence == "synthesized"
    d = np.linalg.norm(h.position - f.position)
    assert 0.12 < d < 0.26
    assert np.dot(res.up, [0, 0, 1]) > 0.95                         # front normal


def test_mask_decoder_picks_the_convention_whose_bbox_is_the_box() -> None:
    size = 100
    ids = np.zeros((size, size), np.uint32)
    ids[20:60, 30:90] = 7                       # the object
    view = ViewData(index=0, name="side", ortho=True, image_path=None, size=size,
                    view=np.eye(4), proj=np.eye(4), eye=np.zeros(3),
                    forward=np.array([0, 0, -1.0]), up=np.array([0, 1.0, 0]),
                    ids=ids, depth=np.zeros((size, size), np.float32))
    box = [200, 300, 600, 900]                  # ymin, xmin, ymax, xmax
    corners = [[0, 0], [1000, 0], [1000, 1000], [0, 1000]]
    box_rel = {"box_2d": box, "mask": corners}
    m, conv = decode_mask(box_rel, view)
    assert conv in ("box_xy", "box_yx") and m.sum() > 2000
    absolute = {"box_2d": box, "mask": [[300, 200], [900, 200], [900, 600], [300, 600]]}
    m2, conv2 = decode_mask(absolute, view)
    assert conv2 == "abs_xy" and m2.sum() > 2000
    swapped = {"box_2d": box, "mask": [[200, 300], [200, 900], [600, 900], [600, 300]]}
    _, conv3 = decode_mask(swapped, view)
    assert conv3 == "abs_yx"


@pytest.mark.parametrize("flip", [False, True])
def test_region_votes_ignore_a_single_wrong_view(flip: bool) -> None:
    from ptv_orchestrator.pipeline.sockets.vision import ViewMasks

    mesh, owner = _mesh(SLIDE, HANDLE)
    n = mesh.tri_count
    size = 40
    # Two fake views that see every triangle once (id = triangle + 1).
    views = []
    for k in range(3):
        ids = np.zeros((size, size), np.uint32)
        flat = ids.reshape(-1)
        flat[:n] = np.arange(1, n + 1)
        views.append(ViewData(index=k, name="v", ortho=True, image_path=None, size=size,
                              view=np.eye(4), proj=np.eye(4), eye=np.zeros(3),
                              forward=np.zeros(3), up=np.zeros(3), ids=ids,
                              depth=np.zeros((size, size), np.float32)))
    vs = ViewSet(out_dir=None, mesh=mesh, views=views, meta={"longest_axis": 1.0})
    handle = np.zeros(size * size, bool)
    handle[:n] = owner == 1
    wrong = np.zeros(size * size, bool)
    wrong[:n] = owner == 0
    masks = [ViewMasks(view=0, masks={"grip": handle.reshape(size, size)}),
             ViewMasks(view=1, masks={"grip": handle.reshape(size, size)}),
             ViewMasks(view=2, masks={"grip": (wrong if flip else handle).reshape(size, size)})]
    reg = Fuser(vs).region("grip", masks)
    got = set(owner[reg.triangles].tolist())
    assert got == {1}
