"""Geometry-only part regions for when no vision model can run.

These are deliberately simple guesses -- the reason the vision pass exists is
that geometry cannot reliably tell a hilt from a blade or the back of a
shield from its front -- but they run the SAME solvers on their regions, so a
project without a Google key still gets sockets in the right conventions,
every one labelled ``heuristic``.
"""

from __future__ import annotations

import numpy as np

from ptv_schemas import ItemInteraction, SocketKind

from .fusion import Fuser, Region, pca
from .views import ViewSet


def _region(fuser: Fuser, pid: str, mask: np.ndarray) -> Region:
    tris = np.flatnonzero(mask)
    return fuser._make(pid, tris, np.ones(fuser.mesh.tri_count), 0)


def heuristic_regions(vs: ViewSet, item: ItemInteraction) -> dict[str, Region]:
    fuser = Fuser(vs)
    mesh = vs.mesh
    c = mesh.centroids
    mean, vals, vecs = pca(c, mesh.areas)
    axis, side = vecs[0], vecs[1]
    t = (c - mean) @ axis
    span = float(t.max() - t.min()) or 1.0
    kinds = {s.kind for s in item.sockets}
    out: dict[str, Region] = {}
    primary = item.hold.primary_socket or next(
        (s.name for s in item.sockets if s.kind == SocketKind.hand_grip), "")

    def slice_spread(lo: float, hi: float) -> float:
        sel = (t >= lo) & (t <= hi)
        if sel.sum() < 4:
            return 0.0
        rel = c[sel] - mean
        flat = rel - np.outer(rel @ axis, axis)
        return float(np.linalg.norm(flat, axis=1).mean())

    if SocketKind.muzzle in kinds:
        # Mass asymmetry: the handle / magazine half sprawls sideways.
        pos_half = t >= 0
        off = c - mean - np.outer(t, axis)
        sprawl_pos = float(np.abs(off[pos_half] @ side).mean()) if pos_half.any() else 0
        sprawl_neg = float(np.abs(off[~pos_half] @ side).mean()) if (~pos_half).any() else 0
        if sprawl_pos > sprawl_neg:
            axis, t = -axis, -t
        grip_half = t < 0
        down = off[grip_half].mean(0) if grip_half.any() else -side
        down = down / (np.linalg.norm(down) or 1.0)
        h = off @ down
        hmax = float(h.max()) or 1.0
        handle = grip_half & (h > 0.35 * hmax)
        barrel = (h < 0.25 * hmax) & (t > -0.2 * span)
        muzzle = barrel & (t > t.max() - 0.05 * span)
        if primary:
            out[primary] = _region(fuser, primary, handle)
        out["barrel"] = _region(fuser, "barrel", barrel)
        name = next(s.name for s in item.sockets if s.kind == SocketKind.muzzle)
        out[name] = _region(fuser, name, muzzle)
        out["muzzle"] = out[name]
        for s in item.sockets:
            if s.kind == SocketKind.support_grip:
                out[s.name] = _region(fuser, s.name, barrel & (t > 0.05 * span) &
                                      (t < 0.35 * span))
        return out

    if kinds & {SocketKind.strike_tip, SocketKind.strike_base, SocketKind.strike_face} \
            or (primary and SocketKind.hand_strap not in kinds):
        end_a = slice_spread(t.max() - 0.15 * span, t.max())
        end_b = slice_spread(t.min(), t.min() + 0.15 * span)
        if end_a < end_b:          # the handle is the thinner end
            axis, t = -axis, -t
        handle = t < t.min() + 0.25 * span
        work = ~handle
        if primary:
            out[primary] = _region(fuser, primary, handle)
        pid = "head" if SocketKind.strike_face in kinds else "blade"
        out[pid] = _region(fuser, pid, work)
        out["working_end"] = out[pid]
        for s in item.sockets:
            if s.kind == SocketKind.support_grip:
                out[s.name] = _region(fuser, s.name, handle & (t < t.min() + 0.12 * span))
    if SocketKind.projectile_nose in kinds or SocketKind.arrow_nock in kinds:
        front = slice_spread(t.max() - 0.08 * span, t.max())
        back = slice_spread(t.min(), t.min() + 0.08 * span)
        # The pointed end (arrowhead, bullet nose) is the narrower one.
        if front > back:
            t = -t
        out["front_tip"] = _region(fuser, "front_tip", t > t.max() - 0.08 * span)
        out["back_end"] = _region(fuser, "back_end", t < t.min() + 0.08 * span)
    normals = getattr(mesh, "normals", None)
    for s in item.sockets:
        if s.kind == SocketKind.press_surface and normals is not None:
            # A button's cap is on top: the up-facing triangles of the top
            # tenth of the mesh.
            y = c[:, 1]
            top = (y > y.max() - 0.1 * (float(y.max() - y.min()) or 1.0)) & (normals[:, 1] > 0.5)
            out[s.name] = _region(fuser, s.name, top)
        elif s.kind == SocketKind.screen and normals is not None:
            # A display's face looks along its thinnest axis: the triangles
            # facing that way on the side with the most surface.
            thin = vecs[2]
            facing = normals @ thin
            pos, neg = facing > 0.7, facing < -0.7
            pick = pos if mesh.areas[pos].sum() >= mesh.areas[neg].sum() else neg
            out[s.name] = _region(fuser, s.name, pick)
    return out
