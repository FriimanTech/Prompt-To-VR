"""Measured collision geometry for every prop GLB.

The runtime used to give a GLB prop a collider that had nothing to do with
its mesh: ``"mesh"`` only worked for inline world meshes, so every Tripo
asset fell through to a 0.5 m box or a 1 m sphere centred on its origin. A
9 cm grenade became a ball the player's body shoved across the room, a 1.8 m
table a cube its pistols fell straight through, and the rifle in an NPC's
hand a box its own floor probe kept standing on.

This module measures, once per asset, what the collider should be:

* ``aabb_min`` / ``aabb_max`` -- the mesh's own bounds, including how far the
  bounds centre sits from the origin (Tripo GLBs are centred; many other
  sources stand on their origin);
* ``hull`` -- up to :data:`HULL_DIRECTIONS` points that lie ON the convex hull
  (the most extreme vertex along a fixed set of directions), from which the
  runtime builds a Jolt ``ConvexHullShape`` for anything that moves;
* ``mesh_lod`` -- which LOD of the asset's chain is accurate enough to be a
  static triangle collider (the coarsest whose simplification error is at
  most :data:`MAX_COLLISION_ERROR_REL`).

Everything is recorded in GLB units, exactly as the runtime reads the file
(node world transforms applied, no re-centring). The scene compiler
multiplies by the asset's ``unit_scale`` to get metres in entity space --
the same conversion the renderer applies to the drawn vertices, so the
collider sits where the mesh is drawn.
"""

from __future__ import annotations

import math
from pathlib import Path
from typing import Any, Optional, Sequence

from .uthana.gltf_io import Glb, GltfError, mat_point

#: Bump when the measurement changes so stale records are re-measured.
COLLISION_VERSION = 1
#: A static triangle collider may be at most this far (relative to the mesh
#: extent) from the drawn surface: 3 cm on a 1 m table.
MAX_COLLISION_ERROR_REL = 0.03
#: Directions sampled for the hull. Every point found is a true hull vertex,
#: so this bounds the hull's size, not its correctness.
HULL_DIRECTIONS = 96


def _fibonacci_directions(n: int) -> list[tuple[float, float, float]]:
    out: list[tuple[float, float, float]] = []
    golden = math.pi * (3.0 - math.sqrt(5.0))
    for i in range(n):
        y = 1.0 - 2.0 * (i + 0.5) / n
        r = math.sqrt(max(0.0, 1.0 - y * y))
        a = golden * i
        out.append((math.cos(a) * r, y, math.sin(a) * r))
    # The six axis directions too, so the hull always reaches the AABB faces.
    out += [(1.0, 0.0, 0.0), (-1.0, 0.0, 0.0), (0.0, 1.0, 0.0),
            (0.0, -1.0, 0.0), (0.0, 0.0, 1.0), (0.0, 0.0, -1.0)]
    return out


def glb_points(path: Path) -> list[tuple[float, float, float]]:
    """Every vertex of every unskinned mesh node, in the file's world space
    -- what the runtime's static loader draws before ``unit_scale``."""
    glb = Glb.load(path)
    world = glb.world_matrices()
    out: list[tuple[float, float, float]] = []
    for ni in glb.mesh_nodes():
        node = glb.nodes[ni]
        mesh = glb.doc["meshes"][node["mesh"]]
        for prim in mesh.get("primitives") or []:
            mode = prim.get("mode", 4)
            if mode != 4:
                continue
            attrs = prim.get("attributes") or {}
            if "POSITION" not in attrs:
                continue
            wm = world[ni]
            for p in glb.read_accessor(attrs["POSITION"]):
                out.append(mat_point(wm, p))
    if not out:
        raise GltfError(f"{path.name}: no triangle geometry")
    return out


def hull_points(points: Sequence[Sequence[float]],
                directions: int = HULL_DIRECTIONS) -> list[list[float]]:
    """Extreme vertex along each sampled direction, deduplicated."""
    dirs = _fibonacci_directions(directions)
    best: list[tuple[float, int]] = [(-math.inf, -1)] * len(dirs)
    for i, p in enumerate(points):
        x, y, z = p[0], p[1], p[2]
        for k, (dx, dy, dz) in enumerate(dirs):
            d = x * dx + y * dy + z * dz
            if d > best[k][0]:
                best[k] = (d, i)
    seen: set[int] = set()
    out: list[list[float]] = []
    for _, i in best:
        if i < 0 or i in seen:
            continue
        seen.add(i)
        p = points[i]
        out.append([float(p[0]), float(p[1]), float(p[2])])
    return out


def choose_mesh_lod(lod_errors_rel: Sequence[float], lod_count: int) -> int:
    """Coarsest LOD accurate enough to stand things on; 0 when unknown."""
    if lod_count <= 0:
        return -1
    best = 0
    for i in range(min(lod_count, len(lod_errors_rel))):
        try:
            err = float(lod_errors_rel[i])
        except (TypeError, ValueError):
            continue
        if err <= MAX_COLLISION_ERROR_REL:
            best = i
    return best


def measure(lod_paths: Sequence[str], lod_errors_rel: Sequence[float],
            repo_root: Path) -> dict[str, Any]:
    """The ``collision`` record for one asset's LOD chain (GLB units)."""
    if not lod_paths:
        raise GltfError("no LOD chain to measure")
    mesh_lod = choose_mesh_lod(lod_errors_rel, len(lod_paths))
    lod0 = Path(lod_paths[0])
    lod0 = lod0 if lod0.is_absolute() else repo_root / lod0
    collision_file = Path(lod_paths[max(0, mesh_lod)])
    collision_file = (collision_file if collision_file.is_absolute()
                      else repo_root / collision_file)
    # Bounds from LOD0 (what the player sees up close); the hull from the
    # collision LOD, whose vertices all lie on LOD0's surface within the
    # recorded error and are far fewer to scan.
    pts0 = glb_points(lod0)
    lo = [min(p[c] for p in pts0) for c in range(3)]
    hi = [max(p[c] for p in pts0) for c in range(3)]
    pts = pts0 if collision_file == lod0 else glb_points(collision_file)
    return {
        "version": COLLISION_VERSION,
        "aabb_min": [round(v, 6) for v in lo],
        "aabb_max": [round(v, 6) for v in hi],
        "hull": [[round(c, 6) for c in p] for p in hull_points(pts)],
        "mesh_lod": mesh_lod,
    }


def try_measure(lod_paths: Sequence[str], lod_errors_rel: Sequence[float],
                repo_root: Path) -> Optional[dict[str, Any]]:
    try:
        return measure(lod_paths, lod_errors_rel, repo_root)
    except (GltfError, OSError, ValueError, KeyError, IndexError):
        return None


def ensure_records(normalization: dict[str, Any], repo_root: Path) -> list[str]:
    """Measure the collider of every optimized, unskinned asset whose record
    is missing or stale. Returns the asset ids measured.

    Lets a scene compiled from an older ``asset_normalization.json`` (one
    written before colliders were measured) get real colliders without
    re-running the optimizer."""
    measured: list[str] = []
    for asset_id, info in normalization.items():
        if str(asset_id).startswith("__") or not isinstance(info, dict):
            continue
        if info.get("skinned"):
            continue
        lods = info.get("lod_paths") or []
        if not lods:
            continue
        rec = info.get("collision")
        if isinstance(rec, dict) and rec.get("version") == COLLISION_VERSION:
            continue
        rec = try_measure(lods, info.get("lod_errors_rel") or [], repo_root)
        if rec is not None:
            info["collision"] = rec
            measured.append(str(asset_id))
    return measured


def to_meters(record: dict[str, Any], unit_scale: float) -> dict[str, Any]:
    """Scene-ready collision block: everything in metres, entity space."""
    s = float(unit_scale)
    return {
        "aabb_min": [round(v * s, 5) for v in record["aabb_min"]],
        "aabb_max": [round(v * s, 5) for v in record["aabb_max"]],
        "hull": [[round(c * s, 5) for c in p] for p in record.get("hull") or []],
        "mesh_lod": int(record.get("mesh_lod", -1)),
    }


def bottom_offset_m(collision_m: dict[str, Any]) -> float:
    """How far below the origin the mesh's lowest point is, in metres
    (positive when the mesh extends below its origin)."""
    return -float(collision_m["aabb_min"][1])


def top_offset_m(collision_m: dict[str, Any]) -> float:
    """How far above the origin the mesh's highest point is, in metres."""
    return float(collision_m["aabb_max"][1])
