"""Part regions -> exact socket transforms, one solver per socket kind.

All maths here is in MESH space (the GLB at source scale, exactly the space
``load_glb_merged`` produces); the stage divides positions by the longest
extent afterwards. Two frame conventions, matching ``docs/sockets.md``:

* HAND sockets (hand_grip, support_grip, support_wrap, hand_strap) are the
  OpenXR grip frame of the hand holding that part: origin on the gripped
  part's axis at palm height, -Z along the part from little finger to thumb,
  +X the palm normal (into the palm for the right hand), +Y completing a
  right-handed frame. For a handle the palm switches sides with the hand and
  this frame is the SAME for both hands (``mirror: same``); for a grip both
  palms approach from the same side (palm-up under a handguard) the left hand
  is this frame turned half a turn about Z (``mirror: flip_z180``).
* FACING sockets (muzzle, strike_tip, projectile_nose, ...): +Z is the
  direction the point faces, +Y is the item's up.

Metric plausibility (a handle is 2-6 cm thick, a rifle's hands are 25-70 cm
apart) is checked against ``metres_per_unit``: the asset's
``target_scale_meters`` divided by the mesh's longest extent.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Optional

import numpy as np

from ptv_schemas import FireSystem, HoldKind, ItemInteraction, SocketKind

from .fusion import Region, pca
from .views import MeshData


@dataclass
class SolvedSocket:
    name: str
    kind: str
    position: np.ndarray                 # mesh space
    rotation: np.ndarray                 # 3x3, columns = X, Y, Z
    confidence: str = "vision"
    extras: dict = field(default_factory=dict)

    def quat_xyzw(self) -> list[float]:
        return mat_to_quat(self.rotation)


@dataclass
class SolveResult:
    sockets: dict[str, SolvedSocket] = field(default_factory=dict)
    forward: Optional[np.ndarray] = None
    up: Optional[np.ndarray] = None
    warnings: list[str] = field(default_factory=list)
    synthesized: list[str] = field(default_factory=list)


def _unit(v: np.ndarray) -> np.ndarray:
    n = float(np.linalg.norm(v))
    return v / n if n > 1e-12 else v


def _perp(v: np.ndarray, axis: np.ndarray) -> np.ndarray:
    return _unit(v - axis * float(np.dot(v, axis)))


def frame_from_zy(z: np.ndarray, y_hint: np.ndarray) -> np.ndarray:
    """Right-handed rotation with +Z = ``z`` and +Y as close to ``y_hint``."""
    z = _unit(z)
    y = y_hint - z * float(np.dot(y_hint, z))
    if np.linalg.norm(y) < 1e-6:
        alt = np.array([0.0, 1.0, 0.0]) if abs(z[1]) < 0.9 else np.array([1.0, 0.0, 0.0])
        y = alt - z * float(np.dot(alt, z))
    y = _unit(y)
    x = np.cross(y, z)
    return np.column_stack([x, y, z])


def mat_to_quat(m: np.ndarray) -> list[float]:
    """3x3 rotation (columns = axes) -> quaternion [x, y, z, w]."""
    t = float(np.trace(m))
    if t > 0:
        s = np.sqrt(t + 1.0) * 2
        w = 0.25 * s
        x = (m[2, 1] - m[1, 2]) / s
        y = (m[0, 2] - m[2, 0]) / s
        z = (m[1, 0] - m[0, 1]) / s
    elif m[0, 0] > m[1, 1] and m[0, 0] > m[2, 2]:
        s = np.sqrt(1.0 + m[0, 0] - m[1, 1] - m[2, 2]) * 2
        w = (m[2, 1] - m[1, 2]) / s
        x = 0.25 * s
        y = (m[0, 1] + m[1, 0]) / s
        z = (m[0, 2] + m[2, 0]) / s
    elif m[1, 1] > m[2, 2]:
        s = np.sqrt(1.0 + m[1, 1] - m[0, 0] - m[2, 2]) * 2
        w = (m[0, 2] - m[2, 0]) / s
        x = (m[0, 1] + m[1, 0]) / s
        y = 0.25 * s
        z = (m[1, 2] + m[2, 1]) / s
    else:
        s = np.sqrt(1.0 + m[2, 2] - m[0, 0] - m[1, 1]) * 2
        w = (m[1, 0] - m[0, 1]) / s
        x = (m[0, 2] + m[2, 0]) / s
        y = (m[1, 2] + m[2, 1]) / s
        z = 0.25 * s
    q = np.array([x, y, z, w], dtype=float)
    q /= np.linalg.norm(q)
    if q[3] < 0:
        q = -q
    return [float(v) for v in q]


# ---------------------------------------------------------------------------
# Geometry helpers
# ---------------------------------------------------------------------------


def point_inside(mesh: MeshData, p: np.ndarray) -> bool:
    """Ray-parity inside test, majority of three directions."""
    dirs = np.array([[0.577, 0.577, 0.577], [-0.267, 0.802, -0.535],
                     [0.836, -0.183, -0.517]])
    a = mesh.positions[mesh.indices[:, 0]]
    b = mesh.positions[mesh.indices[:, 1]]
    c = mesh.positions[mesh.indices[:, 2]]
    e1, e2 = b - a, c - a
    votes = 0
    for d in dirs:
        h = np.cross(np.broadcast_to(d, e2.shape), e2)
        det = np.einsum("ij,ij->i", e1, h)
        ok = np.abs(det) > 1e-14
        inv = np.where(ok, 1.0 / np.where(ok, det, 1.0), 0.0)
        s = p - a
        u = np.einsum("ij,ij->i", s, h) * inv
        q = np.cross(s, e1)
        v = (q @ d) * inv
        t = np.einsum("ij,ij->i", e2, q) * inv
        hit = ok & (u >= 0) & (v >= 0) & (u + v <= 1) & (t > 1e-9)
        votes += int(hit.sum()) % 2
    return votes >= 2


def slab_center(points: np.ndarray, axis: np.ndarray, origin: np.ndarray,
                t: float, half_width: float) -> tuple[np.ndarray, float, float]:
    """Centre of the cross-section of ``points`` at axial coordinate ``t``,
    and its two extents (thickness across, width across)."""
    rel = points - origin
    ta = rel @ axis
    sel = np.abs(ta - t) <= half_width
    if sel.sum() < 6:
        sel = np.argsort(np.abs(ta - t))[: max(6, min(40, len(points)))]
        sl = points[sel]
    else:
        sl = points[sel]
    # Project onto the plane through ``origin`` perpendicular to ``axis``;
    # the two in-plane principal directions span the cross-section.
    flat = sl - np.outer((sl - origin) @ axis, axis)
    _, _, vecs = pca(flat)
    u = _perp(vecs[0], axis)
    v = _unit(np.cross(axis, u))
    pu, pv = flat @ u, flat @ v
    cu = 0.5 * (pu.min() + pu.max())
    cv = 0.5 * (pv.min() + pv.max())
    centre = axis * (float(origin @ axis) + t) + u * cu + v * cv
    return centre, float(pu.max() - pu.min()), float(pv.max() - pv.min())


def extreme(points: np.ndarray, axis: np.ndarray, frac: float = 0.02) -> np.ndarray:
    """Mean of the outermost ``frac`` of points along ``axis``."""
    t = points @ axis
    n = max(3, int(len(points) * frac))
    idx = np.argsort(t)[-n:]
    return points[idx].mean(0)


# ---------------------------------------------------------------------------
# Solvers
# ---------------------------------------------------------------------------


def solve_handle(region_pts: np.ndarray, weights: np.ndarray, *,
                 toward: np.ndarray, front: np.ndarray,
                 palm_from_top: float = 0.45
                 ) -> tuple[np.ndarray, np.ndarray, float, float, np.ndarray]:
    """A gripped bar (handle, hilt, strap, foregrip).

    Returns origin, rotation (OpenXR grip frame of a hand holding it),
    thickness, length and the part axis (little finger -> thumb).
    ``toward`` is a point the thumb end faces (the slide, the blade);
    ``front`` is the direction the fingers' side faces (trigger side)."""
    mean, vals, vecs = pca(region_pts, weights)
    a = vecs[0]
    if np.dot(toward - mean, a) < 0:
        a = -a
    t = (region_pts - mean) @ a
    t_min, t_max = float(t.min()), float(t.max())
    length = t_max - t_min
    t_palm = t_max - palm_from_top * length
    centre, th0, th1 = slab_center(region_pts, a, mean, t_palm, max(0.08 * length, 1e-6))
    z = -a
    back = -_perp(front, a)
    rot = frame_from_zy(z, back)
    return centre, rot, min(th0, th1), length, a


def _region_or_none(regions: dict[str, Region], pid: str, min_area: float = 0.0
                    ) -> Optional[Region]:
    r = regions.get(pid)
    if r is None or r.empty or r.area <= min_area or len(r.points) < 8:
        return None
    return r


def solve_item(item: ItemInteraction, regions: dict[str, Region], mesh: MeshData,
               *, metres_per_unit: float) -> SolveResult:
    res = SolveResult()
    allpts = mesh.positions
    m_mean, _, m_vecs = pca(mesh.centroids, mesh.areas)
    kinds = {s.kind: s for s in item.sockets}
    by_name = {s.name: s for s in item.sockets}
    ranged = item.weapon is not None and item.weapon.fire_system in (
        FireSystem.hitscan, FireSystem.projectile, FireSystem.hybrid, FireSystem.beam)

    primary = item.hold.primary_socket or next(
        (s.name for s in item.sockets if s.kind == SocketKind.hand_grip), "")
    grip_r = _region_or_none(regions, primary) if primary else None

    # --- Item forward / up ------------------------------------------------
    forward: Optional[np.ndarray] = None
    up: Optional[np.ndarray] = None
    barrel = _region_or_none(regions, "barrel")
    muzzle_spec = kinds.get(SocketKind.muzzle)
    muzzle_r = _region_or_none(regions, muzzle_spec.name if muzzle_spec else "muzzle") \
        or _region_or_none(regions, "muzzle")
    if ranged or muzzle_spec is not None:
        if barrel is not None:
            bm, _, bv = pca(barrel.points, barrel.weights)
            forward = bv[0]
            ref = muzzle_r.centroid if muzzle_r is not None else (
                grip_r.centroid if grip_r is not None else m_mean)
            sign = 1.0 if muzzle_r is not None else -1.0
            if np.dot(ref - bm, forward) * sign < 0:
                forward = -forward
        elif muzzle_r is not None and grip_r is not None:
            forward = _unit(muzzle_r.centroid - grip_r.centroid)
        else:
            forward = m_vecs[0]
            res.warnings.append("no barrel region: forward taken from the mesh's long axis")
        if grip_r is not None:
            up = _perp(m_mean - grip_r.centroid, forward)
            if barrel is not None:
                up = _perp(barrel.centroid - grip_r.centroid, forward)
        else:
            up = _perp(m_vecs[1], forward)
        res.forward, res.up = forward, up

    blade = _region_or_none(regions, "blade")
    head = _region_or_none(regions, "head")
    working = blade or head or _region_or_none(regions, "working_end")

    # --- Hand grip (primary) ----------------------------------------------
    grip_axis = None
    if primary and by_name.get(primary) is not None and \
            by_name[primary].kind == SocketKind.hand_grip:
        if grip_r is None:
            res.warnings.append(f"'{primary}': the vision pass found no handle")
        else:
            if ranged and forward is not None:
                toward = (barrel.centroid if barrel is not None
                          else grip_r.centroid + up * 1.0)
                front = forward
                frac = 0.45
            elif working is not None:
                toward = working.centroid
                wm, _, wv = pca(working.points, working.weights)
                front = wv[1]
                frac = 0.5
            else:
                toward = m_mean
                front = m_vecs[1]
                frac = 0.5
            o, rot, thick, length, a = solve_handle(
                grip_r.points, grip_r.weights, toward=toward, front=front,
                palm_from_top=frac)
            grip_axis = a
            res.sockets[primary] = SolvedSocket(
                primary, SocketKind.hand_grip.value, o, rot,
                extras={"handle_thickness": thick, "handle_length": length,
                        "mirror": "same"})
            thick_m = thick * metres_per_unit
            if not (0.012 <= thick_m <= 0.09):
                res.warnings.append(f"'{primary}': handle is {thick_m * 100:.1f} cm "
                                    "thick, outside 1.2-9 cm")
            if not point_inside(mesh, o) and thick_m > 0.01:
                res.warnings.append(f"'{primary}': palm point is not inside the handle")
            if ranged and forward is None:
                res.forward = _unit(-rot[:, 1])

    # --- Facing / other sockets --------------------------------------------
    for spec in item.sockets:
        if spec.name in res.sockets:
            continue
        k = spec.kind
        reg = _region_or_none(regions, spec.name)
        if k == SocketKind.muzzle:
            _solve_muzzle(res, spec.name, muzzle_r or reg, barrel, forward, up, allpts,
                          m_vecs)
        elif k == SocketKind.support_grip:
            _solve_support(res, spec.name, reg, forward, up, barrel, grip_r,
                           metres_per_unit, working)
        elif k == SocketKind.support_wrap:
            prim = res.sockets.get(primary)
            if prim is None:
                res.warnings.append(f"'{spec.name}': no primary grip to wrap")
                continue
            thick = prim.extras.get("handle_thickness", 0.03 / metres_per_unit)
            X, Y, Z = prim.rotation[:, 0], prim.rotation[:, 1], prim.rotation[:, 2]
            down = 0.012 / metres_per_unit
            fwd = 0.008 / metres_per_unit
            pos = prim.position + Z * down - Y * fwd
            res.sockets[spec.name] = SolvedSocket(
                spec.name, k.value, pos, prim.rotation.copy(),
                confidence=prim.confidence,
                extras={"mirror": "same", "wrap_of": primary,
                        "lateral_offset": thick * 0.5 + 0.022 / metres_per_unit})
        elif k in (SocketKind.hand_strap, SocketKind.forearm_strap):
            continue  # solved together below
        elif k in (SocketKind.strike_base, SocketKind.strike_tip):
            _solve_blade(res, spec, blade or reg, grip_r, allpts, m_vecs)
        elif k == SocketKind.strike_face:
            _solve_face(res, spec, head or reg, grip_r, grip_axis, allpts, m_vecs)
        elif k in (SocketKind.projectile_nose, SocketKind.projectile_tail,
                   SocketKind.arrow_nock):
            _solve_projectile_end(res, spec, regions, allpts, m_vecs)
        elif k in (SocketKind.limb_tip_top, SocketKind.limb_tip_bottom,
                   SocketKind.string_rest, SocketKind.arrow_rest):
            _solve_bow_point(res, spec, regions, allpts, m_vecs, grip_r)
        else:
            if reg is None:
                res.warnings.append(f"'{spec.name}': part not found")
                continue
            n = _unit((mesh.normals[reg.triangles] * mesh.areas[reg.triangles, None]).sum(0))
            res.sockets[spec.name] = SolvedSocket(spec.name, k.value, reg.centroid,
                                                  frame_from_zy(n, m_vecs[0]))

    if SocketKind.hand_strap in kinds or SocketKind.forearm_strap in kinds:
        _solve_straps(res, item, regions, mesh, metres_per_unit, m_mean, m_vecs)

    # A hand grip that is not the primary (rare: a second handle on a tool).
    for spec in item.sockets:
        if spec.name not in res.sockets and spec.kind == SocketKind.hand_grip:
            reg = _region_or_none(regions, spec.name)
            if reg is not None:
                o, rot, thick, length, _ = solve_handle(
                    reg.points, reg.weights, toward=m_mean, front=m_vecs[1], palm_from_top=0.5)
                res.sockets[spec.name] = SolvedSocket(
                    spec.name, spec.kind.value, o, rot,
                    extras={"handle_thickness": thick, "mirror": "same"})
    return res


def _solve_muzzle(res: SolveResult, name: str, reg: Optional[Region],
                  barrel: Optional[Region], forward: Optional[np.ndarray],
                  up: Optional[np.ndarray], allpts: np.ndarray, m_vecs: np.ndarray) -> None:
    f = forward if forward is not None else m_vecs[0]
    u = up if up is not None else _perp(m_vecs[1], f)
    src = reg.points if reg is not None else (barrel.points if barrel is not None else allpts)
    t = src @ f
    t_max = float(t.max())
    span = float(t.max() - t.min())
    # The rim: the front-most slice of the muzzle (or barrel) region.
    sel = t >= t_max - max(0.25 * span, 1e-9) if reg is not None else \
        t >= t_max - 0.04 * span
    rim = src[sel]
    if len(rim) < 4:
        rim = src[np.argsort(t)[-8:]]
    centre_flat = rim.mean(0)
    # Push to the front plane of the whole mesh along f (the opening).
    pos = centre_flat + f * (t_max - float(centre_flat @ f))
    radial = rim - pos
    radial = radial - np.outer(radial @ f, f)
    # A rim ring's radius is its upper radial percentile (a filled face's
    # median would under-read it by ~30 %).
    bore = 2.0 * float(np.percentile(np.linalg.norm(radial, axis=1), 85)) if len(rim) else 0.0
    res.sockets[name] = SolvedSocket(name, SocketKind.muzzle.value, pos, frame_from_zy(f, u),
                                     confidence="vision" if reg is not None else "heuristic",
                                     extras={"bore_diameter": bore})
    if reg is None:
        res.warnings.append(f"'{name}': no muzzle region; placed at the barrel's end")


def _solve_support(res: SolveResult, name: str, reg: Optional[Region],
                   forward: Optional[np.ndarray], up: Optional[np.ndarray],
                   barrel: Optional[Region], grip_r: Optional[Region],
                   metres_per_unit: float, working: Optional[Region]) -> None:
    if reg is None:
        res.warnings.append(f"'{name}': the vision pass found no support grip")
        return
    rm, _, rv = pca(reg.points, reg.weights)
    if forward is not None and abs(float(np.dot(rv[0], forward))) > np.cos(np.radians(35)):
        # A handguard along the barrel: palm under it, facing up.
        f = forward
        u = up if up is not None else _perp(rv[1], f)
        t = (reg.points - rm) @ f
        centre, th, _ = slab_center(reg.points, f, rm, 0.5 * (t.min() + t.max()),
                                    max(0.1 * (t.max() - t.min()), 1e-6))
        z = -f
        x = -u                       # right hand: +X into the palm (palm faces up)
        y = np.cross(z, x)
        rot = np.column_stack([x, y, z])
        res.sockets[name] = SolvedSocket(name, SocketKind.support_grip.value, centre, rot,
                                         extras={"mirror": "flip_z180", "mode": "under",
                                                 "handle_thickness": th})
    else:
        toward = barrel.centroid if barrel is not None else (
            working.centroid if working is not None else rm + rv[0])
        front = forward if forward is not None else rv[1]
        o, rot, th, length, _ = solve_handle(reg.points, reg.weights, toward=toward,
                                             front=front, palm_from_top=0.5)
        res.sockets[name] = SolvedSocket(name, SocketKind.support_grip.value, o, rot,
                                         extras={"mirror": "same", "mode": "handle",
                                                 "handle_thickness": th})
    if grip_r is not None:
        d = float(np.linalg.norm(res.sockets[name].position - grip_r.centroid)) * metres_per_unit
        if d < 0.08:
            res.warnings.append(f"'{name}': support grip only {d * 100:.0f} cm from the "
                                "primary grip")


def _solve_blade(res: SolveResult, spec, blade: Optional[Region], grip_r: Optional[Region],
                 allpts: np.ndarray, m_vecs: np.ndarray) -> None:
    if blade is None:
        res.warnings.append(f"'{spec.name}': no blade region; using the long axis")
        pts = allpts
        axis = m_vecs[0]
        edge = m_vecs[1]
        if grip_r is not None and np.dot(pts.mean(0) - grip_r.centroid, axis) < 0:
            axis = -axis
        conf = "heuristic"
    else:
        bm, _, bv = pca(blade.points, blade.weights)
        pts, axis, edge = blade.points, bv[0], bv[1]
        ref = grip_r.centroid if grip_r is not None else allpts.mean(0)
        if np.dot(bm - ref, axis) < 0:
            axis = -axis
        conf = "vision"
    if spec.kind == SocketKind.strike_tip:
        pos = extreme(pts, axis, 0.01)
    else:
        t = pts @ axis
        sel = t <= t.min() + 0.04 * (t.max() - t.min())
        pos = pts[sel].mean(0) if sel.sum() else pts[np.argmin(t)]
    res.sockets[spec.name] = SolvedSocket(spec.name, spec.kind.value, pos,
                                          frame_from_zy(axis, edge), confidence=conf)


def _solve_face(res: SolveResult, spec, head: Optional[Region], grip_r: Optional[Region],
                grip_axis: Optional[np.ndarray], allpts: np.ndarray,
                m_vecs: np.ndarray) -> None:
    pts = head.points if head is not None else allpts
    hm, _, hv = pca(pts, head.weights if head is not None else None)
    axis = hv[0]
    if grip_axis is not None and abs(float(np.dot(axis, grip_axis))) > 0.8:
        axis = hv[1]
    # A war hammer has a flat face on one side and a spike / claw on the
    # other: the face is the end whose cross-section is wider.
    t = (pts - hm) @ axis
    span = float(t.max() - t.min()) or 1.0

    def spread(sel: np.ndarray) -> float:
        if sel.sum() < 3:
            return 0.0
        rel = pts[sel] - hm
        flat = rel - np.outer(rel @ axis, axis)
        return float(np.linalg.norm(flat, axis=1).mean())

    if spread(t <= t.min() + 0.15 * span) > spread(t >= t.max() - 0.15 * span):
        axis = -axis
    pos = extreme(pts, axis, 0.03)
    up = grip_axis if grip_axis is not None else m_vecs[0]
    res.sockets[spec.name] = SolvedSocket(spec.name, spec.kind.value, pos,
                                          frame_from_zy(axis, up),
                                          confidence="vision" if head is not None else "heuristic")


def _solve_projectile_end(res: SolveResult, spec, regions: dict[str, Region],
                          allpts: np.ndarray, m_vecs: np.ndarray) -> None:
    axis = m_vecs[0]
    tip = _region_or_none(regions, "front_tip")
    back = _region_or_none(regions, "back_end")
    centre = allpts.mean(0)
    if tip is not None and np.dot(tip.centroid - centre, axis) < 0:
        axis = -axis
    elif tip is None and back is not None and np.dot(back.centroid - centre, axis) > 0:
        axis = -axis
    conf = "vision" if (tip is not None or back is not None) else "heuristic"
    if spec.kind == SocketKind.projectile_nose:
        pos = extreme(allpts, axis, 0.01)
    else:
        pos = extreme(allpts, -axis, 0.01)
    res.sockets[spec.name] = SolvedSocket(spec.name, spec.kind.value, pos,
                                          frame_from_zy(axis, m_vecs[1]), confidence=conf)
    if res.forward is None:
        res.forward, res.up = axis, _perp(m_vecs[1], axis)


def _solve_bow_point(res: SolveResult, spec, regions: dict[str, Region],
                     allpts: np.ndarray, m_vecs: np.ndarray,
                     grip_r: Optional[Region]) -> None:
    long_axis = m_vecs[0]
    string = _region_or_none(regions, "bowstring")
    riser = _region_or_none(regions, "riser") or grip_r
    rest = _region_or_none(regions, next((n for n in regions if "arrow_rest" in n), ""))
    centre = allpts.mean(0)
    # Up: the arrow rest sits above the handle.
    if rest is not None and grip_r is not None and \
            np.dot(rest.centroid - grip_r.centroid, long_axis) < 0:
        long_axis = -long_axis
    tip_top = extreme(allpts, long_axis, 0.01)
    tip_bottom = extreme(allpts, -long_axis, 0.01)
    # The string line runs between the limb tips. Generated bows often have
    # no string modelled at all -- the engine draws one -- so the line is the
    # string whether or not the mesh has it.
    line_dir = _unit(tip_top - tip_bottom)
    grip_c = grip_r.centroid if grip_r is not None else (
        riser.centroid if riser is not None else centre)
    on_line = tip_bottom + line_dir * float((grip_c - tip_bottom) @ line_dir)
    # Forward (shooting direction): from the string toward the riser.
    if string is not None and riser is not None:
        fwd = _perp(riser.centroid - string.centroid, long_axis)
    elif riser is not None and np.linalg.norm(riser.centroid - on_line) > 1e-6:
        fwd = _perp(riser.centroid - on_line, long_axis)
    else:
        fwd = _perp(m_vecs[1], long_axis)
    k = spec.kind
    conf = "vision" if string is not None else "synthesized"
    if k == SocketKind.limb_tip_top:
        pos, z, conf = tip_top, long_axis, "vision"
    elif k == SocketKind.limb_tip_bottom:
        pos, z, conf = tip_bottom, -long_axis, "vision"
    elif k == SocketKind.string_rest:
        if string is not None:
            t = (string.points - centre) @ long_axis
            t0 = float((grip_c - centre) @ long_axis)
            sel = np.abs(t - t0) <= 0.06 * float(t.max() - t.min() + 1e-9)
            pos = string.points[sel].mean(0) if sel.sum() else string.centroid
        else:
            pos = on_line
        z = fwd
    else:  # arrow_rest
        reg = _region_or_none(regions, spec.name)
        if reg is not None:
            pos, conf = reg.centroid, "vision"
        else:
            # Just above the handle, on the riser's string-facing side.
            g_len = 0.0
            if grip_r is not None:
                tg = (grip_r.points - grip_c) @ long_axis
                g_len = float(tg.max() - tg.min())
            pos = grip_c + long_axis * (0.6 * g_len + 0.01 * float(np.ptp(allpts @ long_axis)))
            conf = "synthesized"
        z = fwd
    res.sockets[spec.name] = SolvedSocket(spec.name, k.value, pos,
                                          frame_from_zy(z, long_axis if k in (
                                              SocketKind.string_rest, SocketKind.arrow_rest)
                                              else fwd), confidence=conf)
    if conf == "synthesized" and "bow points" not in res.synthesized:
        res.synthesized.append("bow points")
    if res.forward is None:
        res.forward, res.up = fwd, long_axis


def _solve_straps(res: SolveResult, item: ItemInteraction, regions: dict[str, Region],
                  mesh: MeshData, metres_per_unit: float, m_mean: np.ndarray,
                  m_vecs: np.ndarray) -> None:
    hand_name = item.hold.primary_socket or next(
        (s.name for s in item.sockets if s.kind == SocketKind.hand_strap), "hand_strap")
    fore_name = item.hold.forearm_socket or next(
        (s.name for s in item.sockets if s.kind == SocketKind.forearm_strap), "forearm_strap")
    hand_r = _region_or_none(regions, hand_name)
    fore_r = _region_or_none(regions, fore_name)
    front = _region_or_none(regions, "front_face")
    allpts = mesh.positions
    thin = m_vecs[2]                       # shield normal axis
    if front is not None:
        # Which side of the shield's mid-plane the front face lies on. (Its
        # normals cannot be summed: a closed boss's normals cancel out.)
        side = float(np.dot(front.centroid - m_mean, thin))
        if abs(side) > 1e-9:
            n_f = thin if side > 0 else -thin
        else:
            n_f = _unit((mesh.normals[front.triangles]
                         * mesh.areas[front.triangles, None]).sum(0))
            n_f = thin if np.dot(n_f, thin) >= 0 else -thin
    else:
        straps = [r.centroid for r in (hand_r, fore_r) if r is not None]
        if straps:
            s = np.mean(straps, axis=0)
            n_f = -thin if np.dot(s - m_mean, thin) > 0 else thin
        else:
            # Domed shields bulge toward the front: the centroid sits on
            # the front side of the bounding-box centre.
            bb = 0.5 * (allpts.min(0) + allpts.max(0))
            n_f = thin if np.dot(m_mean - bb, thin) >= 0 else -thin
            res.warnings.append("shield front taken from its dome; check overlays")
    in_plane = _perp(m_vecs[0], n_f)
    t_back = float(((allpts - m_mean) @ -n_f).max())
    back_centre = m_mean - n_f * t_back
    extent = float(np.ptp((allpts - m_mean) @ in_plane))
    diam_m = extent * metres_per_unit
    sep_m = float(np.clip(0.25 * diam_m, 0.14, 0.24))
    sep = sep_m / metres_per_unit
    standoff = 0.03 / metres_per_unit

    def strap(reg: Optional[Region], fallback: np.ndarray) -> tuple[np.ndarray, str]:
        if reg is not None:
            return reg.centroid, "vision"
        return fallback, "synthesized"

    hand_pos, hc = strap(hand_r, back_centre + in_plane * (0.6 * sep) - n_f * standoff)
    fore_pos, fc = strap(fore_r, back_centre - in_plane * (0.4 * sep) - n_f * standoff)
    if hand_r is not None and fore_r is None:
        line = _perp(back_centre - hand_pos, n_f)
        fore_pos, fc = hand_pos + line * sep, "synthesized"
    elif fore_r is not None and hand_r is None:
        line = _perp(fore_pos - back_centre, n_f)
        if np.linalg.norm(fore_pos - back_centre) < 1e-6:
            line = -in_plane
        hand_pos, hc = fore_pos - line * sep, "synthesized"
    L = _unit(fore_pos - hand_pos)          # hand strap -> forearm strap (toward elbow)
    tube = _unit(np.cross(L, n_f))
    # Right hand: the palm faces the player (away from the shield's front),
    # so +X (into the palm) is the front normal.
    z = -tube
    x = n_f
    y = np.cross(z, x)
    res.sockets[hand_name] = SolvedSocket(hand_name, SocketKind.hand_strap.value, hand_pos,
                                          np.column_stack([x, y, z]), confidence=hc,
                                          extras={"mirror": "flip_z180"})
    res.sockets[fore_name] = SolvedSocket(fore_name, SocketKind.forearm_strap.value, fore_pos,
                                          frame_from_zy(L, n_f), confidence=fc)
    if "synthesized" in (hc, fc):
        res.synthesized.append("straps")
    res.forward, res.up = L, n_f
    d = float(np.linalg.norm(fore_pos - hand_pos)) * metres_per_unit
    if not (0.08 <= d <= 0.45):
        res.warnings.append(f"shield straps are {d * 100:.0f} cm apart (8-45 cm expected)")
