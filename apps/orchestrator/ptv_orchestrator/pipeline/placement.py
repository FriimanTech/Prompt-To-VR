"""World-aware placement: intents -> world anchors -> resolved transforms.

Why this exists. The scene composer used to lay the gameplay layer out
around the spawn point with no knowledge of the world, while the world
architect -- separately -- built most of the same things: in the shooting
range it built two tables with a pad per weapon at table-top height, a
lane-2 console, a HUD backplate and red drop rods to hang targets from.
Gameplay then put its own copies of the tables beside the spawn, its guns
above THOSE tables, its targets on the floor, and nothing linked the two.
On top of that every Tripo GLB is centred on its origin, so anything placed
"at the floor" sank by half its height.

So placement is a contract between the stages:

* the scene composer says WHERE each item belongs (``EntitySpec.placement``:
  on a surface, on the floor, hanging, on a wall, at a point; plus the
  anchor id it needs) instead of inventing coordinates;
* static, non-interactive furniture is the WORLD's (``world_owned_assets``);
  the composer never places it;
* the world architect builds that furniture and declares an anchor for
  every request (``gameplay_anchor``), or an anchor SET for runtime spawns
  (``anchor_set``); :func:`validate_anchors` checks every request is
  satisfied by real geometry before the world is accepted;
* the scene compiler rests each item on its anchor using the mesh's
  measured bounds (:func:`resolve`), lifts any prop still sunk into its
  support (:func:`settle`) and fails on anything left floating, sunk or
  interpenetrating (:func:`violations`).
"""

from __future__ import annotations

import json
import math
from pathlib import Path
from typing import Any, Callable, Iterable, Optional

WarnFn = Callable[[str], None]

#: Relations that rest an item ON something (its base on the support).
RESTING = ("on_surface", "on_floor")
#: A support top may be this far below an anchor that rests on it.
SUPPORT_TOL_M = 0.02
#: A hanging anchor's support may be this far above it.
HANG_TOL_M = 0.04
#: Clearance left between a resting base and its support (no initial contact).
REST_CLEARANCE_M = 0.002
#: Sunk deeper than this into a support -> lifted / reported.
SUNK_TOL_M = 0.02
#: Floating higher than this above any support -> reported.
FLOAT_TOL_M = 0.03
#: Two props overlapping more than this on every axis interpenetrate.
OVERLAP_TOL_M = 0.01


# ---------------------------------------------------------------------------
# Math
# ---------------------------------------------------------------------------

def _qmul(a: list[float], b: list[float]) -> list[float]:
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return [aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw,
            aw * bw - ax * bx - ay * by - az * bz]


def _qrot(q: list[float], v: list[float]) -> list[float]:
    x, y, z, w = q
    vx, vy, vz = v
    tx = 2.0 * (y * vz - z * vy)
    ty = 2.0 * (z * vx - x * vz)
    tz = 2.0 * (x * vy - y * vx)
    return [vx + w * tx + (y * tz - z * ty),
            vy + w * ty + (z * tx - x * tz),
            vz + w * tz + (x * ty - y * tx)]


def yaw_quat(yaw: float) -> list[float]:
    return [0.0, math.sin(0.5 * yaw), 0.0, math.cos(0.5 * yaw)]


def yaw_from_forward(f: Iterable[float]) -> Optional[float]:
    """ptv::yaw_from_forward: the yaw that turns -Z onto ``f``."""
    fx, _, fz = [float(v) for v in list(f)[:3]]
    if math.hypot(fx, fz) < 1e-6:
        return None
    return math.atan2(-fx, -fz)


def yaw_of(q: list[float]) -> float:
    f = _qrot(q, [0.0, 0.0, -1.0])
    y = yaw_from_forward(f)
    return 0.0 if y is None else y


def _without_yaw(q: list[float]) -> list[float]:
    return _qmul(yaw_quat(-yaw_of(q)), q)


def _corners(lo: list[float], hi: list[float]) -> list[list[float]]:
    return [[(hi if i & 1 else lo)[0], (hi if i & 2 else lo)[1], (hi if i & 4 else lo)[2]]
            for i in range(8)]


def rotated_bounds(lo: list[float], hi: list[float], rot: list[float],
                   scale: float = 1.0) -> tuple[list[float], list[float]]:
    """AABB, relative to the origin, of a local box after scale and rotation."""
    pts = [_qrot(rot, [c * scale for c in p]) for p in _corners(lo, hi)]
    return ([min(p[i] for p in pts) for i in range(3)],
            [max(p[i] for p in pts) for i in range(3)])


# ---------------------------------------------------------------------------
# Measured bounds
# ---------------------------------------------------------------------------

def glb_bounds(path: Path) -> Optional[tuple[list[float], list[float]]]:
    """Raw-unit AABB of every unskinned mesh in a GLB, node transforms
    applied, from the accessor min/max (fast: no vertex decode)."""
    from .uthana.gltf_io import Glb, mat_point  # noqa: PLC0415

    try:
        glb = Glb.load(path)
    except Exception:  # noqa: BLE001 - unreadable -> unknown
        return None
    world = glb.world_matrices()
    lo = [math.inf] * 3
    hi = [-math.inf] * 3
    for ni in glb.mesh_nodes():
        mesh = glb.doc["meshes"][glb.nodes[ni]["mesh"]]
        for prim in mesh.get("primitives") or []:
            idx = (prim.get("attributes") or {}).get("POSITION")
            if idx is None:
                continue
            acc = glb.doc["accessors"][idx]
            mn, mx = acc.get("min"), acc.get("max")
            if not mn or not mx:
                continue
            for c in _corners([float(v) for v in mn], [float(v) for v in mx]):
                p = mat_point(world[ni], c)
                for i in range(3):
                    lo[i] = min(lo[i], p[i])
                    hi[i] = max(hi[i], p[i])
    if lo[0] == math.inf:
        return None
    return lo, hi


def bounds_m_from_glb(path: Path, target_scale_m: float) -> Optional[dict[str, list[float]]]:
    """Metres-at-scene-size bounds of a prop GLB (scaled by its longest axis,
    the same rule the renderer and optimizer use)."""
    b = glb_bounds(path)
    if b is None:
        return None
    lo, hi = b
    longest = max(hi[i] - lo[i] for i in range(3))
    if longest <= 1e-9 or target_scale_m <= 0.0:
        return None
    s = float(target_scale_m) / longest
    return {"aabb_min": [round(v * s, 5) for v in lo],
            "aabb_max": [round(v * s, 5) for v in hi]}


def entity_local_bounds(entity: dict) -> Optional[tuple[list[float], list[float]]]:
    """Local (entity-space, scale 1) AABB of a scene entity, metres."""
    mesh = entity.get("mesh") or {}
    coll = mesh.get("collision") if isinstance(mesh, dict) else None
    if isinstance(coll, dict) and coll.get("aabb_min") and coll.get("aabb_max"):
        return ([float(v) for v in coll["aabb_min"]], [float(v) for v in coll["aabb_max"]])
    if isinstance(mesh, dict) and mesh.get("aabb_min") and mesh.get("aabb_max"):
        return ([float(v) for v in mesh["aabb_min"]], [float(v) for v in mesh["aabb_max"]])
    prim = entity.get("primitive") or {}
    if isinstance(prim, dict) and prim.get("size"):
        h = [0.5 * float(v) for v in prim["size"][:3]]
        return ([-h[0], -h[1], -h[2]], h)
    inline = entity.get("mesh_inline") or {}
    pos = inline.get("positions") if isinstance(inline, dict) else None
    if pos and len(pos) >= 3:
        xs, ys, zs = pos[0::3], pos[1::3], pos[2::3]
        return ([min(xs), min(ys), min(zs)], [max(xs), max(ys), max(zs)])
    return None


def _transform(entity: dict) -> tuple[list[float], list[float], float]:
    t = entity.get("transform") or {}
    pos = [float(v) for v in (t.get("position") or [0.0, 0.0, 0.0])[:3]]
    rot = [float(v) for v in (t.get("rotation") or [0.0, 0.0, 0.0, 1.0])[:4]]
    scale = float(t.get("scale") or 1.0)
    return pos, rot, scale


def world_bounds(entity: dict) -> Optional[tuple[list[float], list[float]]]:
    lb = entity_local_bounds(entity)
    if lb is None:
        return None
    pos, rot, scale = _transform(entity)
    # Primitive sizes are already full extents; inline meshes carry scale in
    # their vertices (the world host bakes it), meshes use the transform scale.
    s = 1.0 if (entity.get("primitive") or entity.get("mesh_inline")) else scale
    lo, hi = rotated_bounds(lb[0], lb[1], rot, s)
    return ([pos[i] + lo[i] for i in range(3)], [pos[i] + hi[i] for i in range(3)])


# ---------------------------------------------------------------------------
# Ownership + requests
# ---------------------------------------------------------------------------

def _usage(item: Any) -> str:
    u = getattr(item, "usage", None)
    return getattr(u, "value", str(u or ""))


def world_owned_assets(manifest: Any, design: Any = None, plan: Any = None,
                       npc_plan: Any = None, build_spec: Any = None) -> set[str]:
    """Static, non-interactive objects the WORLD places (tables, racks,
    consoles, posts). The scene composer must not place them: a second copy
    beside the spawn is how the shooting range got four tables and guns
    sitting on the wrong two.

    Everything else -- characters, anything held, pressed, shot, displayed
    or bound to a mechanic -- is gameplay's."""
    if manifest is None:
        return set()
    interactive_usage = {"held_weapon", "held_tool", "held_shield", "thrown", "worn", "ammo",
                         "projectile", "pressable", "display"}
    by_asset = {i.asset_id: i for i in getattr(design, "items", []) or []}
    npc_assets = {n.asset_id for n in getattr(npc_plan, "npcs", []) or []}
    mech_assets: set[str] = set()
    for m in getattr(build_spec, "mechanics", []) or []:
        mech_assets.update(getattr(m, "depends_on_assets", []) or [])
    out: set[str] = set()
    for a in manifest.items:
        cat = getattr(a.category, "value", str(a.category))
        if cat not in ("prop",):
            continue
        if a.id in npc_assets:
            continue
        item = by_asset.get(a.id)
        if item is None:
            continue  # no design -> no evidence it is scenery
        if _usage(item) in interactive_usage:
            continue
        if getattr(item, "destructible", None) is not None:
            continue
        if a.id in mech_assets:
            continue
        out.add(a.id)
    return out


def placement_requests(scene_specs: Iterable[Any], bounds_by_asset: dict[str, dict]) -> list[dict]:
    """What each scene needs the world to declare, in the world's terms."""
    out: list[dict] = []
    for spec in scene_specs:
        for e in spec.entities:
            p = getattr(e, "placement", None)
            if p is None or p.relation.value == "free":
                continue
            asset = e.mesh.asset_id if e.mesh is not None else ""
            b = bounds_by_asset.get(asset) or {}
            size = None
            if b.get("aabb_min") and b.get("aabb_max"):
                size = [round(b["aabb_max"][i] - b["aabb_min"][i], 3) for i in range(3)]
            req = {"scene_id": spec.scene_id, "entity": e.id, "asset_id": asset,
                   "relation": p.relation.value, "group": p.group,
                   "description": p.description, "size_m": size}
            if p.anchor:
                out.append({**req, "anchor": p.anchor})
            if p.anchor_set:
                out.append({**req, "anchor_set": p.anchor_set, "count": max(1, p.count)})
    return out


def requests_brief(requests: list[dict]) -> list[str]:
    """The architect's view of the placement requests."""
    if not requests:
        return []
    lines = [
        "GAMEPLAY PLACEMENT REQUESTS (required). Gameplay items rest on, hang from or "
        "mount on things YOU build. For EVERY request below build the support it needs "
        "(a table, a pedestal, a pole, a wall, a console) and declare the anchor with "
        "EXACTLY the given id:",
        "  w.gameplay_anchor(ptv::world::GameplayAnchorDesc{id, position, facing, normal, "
        "relation, support, tags}) -- position is the point the item's BASE rests on "
        "(on_surface / on_floor: ON the top face of the support, y = its top), the point "
        "it HANGS from (hang_from: the bottom end of the pole / hook), or the point on the "
        "wall face (mount_on_wall, normal pointing away from the wall). support = the "
        "name you gave the supporting geometry.",
        "  w.anchor_set(ptv::world::AnchorSetDesc{id, relation, points, facing, tags}) for "
        "runtime spawn points (at least `count` points, each satisfying the relation).",
        "Leave room: an anchor on a table top must have the item's whole footprint "
        "(size_m x and z) on the table. Never place the requested items yourself.",
    ]
    for r in requests:
        size = r.get("size_m")
        size_s = f" size {size[0]:.2f}x{size[1]:.2f}x{size[2]:.2f} m" if size else ""
        what = (f"anchor '{r['anchor']}'" if r.get("anchor")
                else f"anchor_set '{r['anchor_set']}' with {r.get('count', 1)} point(s)")
        lines.append(
            f"  - {what}: {r['relation']} for {r['entity']} ({r['asset_id']}{size_s})"
            + (f", group {r['group']}" if r.get("group") else "")
            + (f": {r['description']}" if r.get("description") else ""))
    return lines


# ---------------------------------------------------------------------------
# Supports
# ---------------------------------------------------------------------------

class _Support:
    __slots__ = ("id", "lo", "hi")

    def __init__(self, eid: str, lo: list[float], hi: list[float]) -> None:
        self.id, self.lo, self.hi = eid, lo, hi

    def covers_xz(self, x: float, z: float, pad: float = 0.01) -> bool:
        return (self.lo[0] - pad <= x <= self.hi[0] + pad
                and self.lo[2] - pad <= z <= self.hi[2] + pad)


def _is_support(e: dict) -> bool:
    phys = e.get("physics") or {}
    if not phys or phys.get("kind") not in (None, "static"):
        return False
    tags = e.get("tags") or []
    if "template" in tags or e.get("attached_to"):
        return False
    if phys.get("collision_shape") == "none":
        return False
    return True


def supports(entities: Iterable[dict], exclude: Optional[set[str]] = None) -> list[_Support]:
    out: list[_Support] = []
    for e in entities:
        if not isinstance(e, dict) or not _is_support(e):
            continue
        if exclude and str(e.get("id")) in exclude:
            continue
        b = world_bounds(e)
        if b is None:
            continue
        out.append(_Support(str(e.get("id")), b[0], b[1]))
    return out


def top_below(sup: list[_Support], x: float, z: float, y: float,
              tol: float = SUPPORT_TOL_M) -> Optional[_Support]:
    """Highest support whose top is at or below ``y + tol`` under (x, z)."""
    best = None
    for s in sup:
        if not s.covers_xz(x, z):
            continue
        if s.hi[1] <= y + tol and (best is None or s.hi[1] > best.hi[1]):
            best = s
    return best


def bottom_above(sup: list[_Support], x: float, z: float, y: float,
                 tol: float = HANG_TOL_M) -> Optional[_Support]:
    best = None
    for s in sup:
        if not s.covers_xz(x, z, pad=0.03):
            continue
        if s.lo[1] >= y - tol and (best is None or s.lo[1] < best.lo[1]):
            best = s
    return best


# ---------------------------------------------------------------------------
# World-side validation
# ---------------------------------------------------------------------------

def _anchor_index(doc: dict) -> tuple[dict[str, dict], dict[str, dict]]:
    anchors = {str(a.get("id")): a for a in doc.get("anchors") or [] if isinstance(a, dict)}
    sets = {str(s.get("id")): s for s in doc.get("anchor_sets") or [] if isinstance(s, dict)}
    return anchors, sets


def _check_point(relation: str, p: list[float], sup: list[_Support], floor_y: Optional[float],
                 footprint: Optional[tuple[float, float]]) -> Optional[str]:
    x, y, z = p
    if relation in RESTING:
        s = top_below(sup, x, z, y)
        if s is None or y - s.hi[1] > SUPPORT_TOL_M:
            if relation == "on_floor" and floor_y is not None and abs(y - floor_y) <= SUPPORT_TOL_M:
                return None
            return f"nothing to rest on: no support top within {SUPPORT_TOL_M * 100:.0f} cm below"
        if footprint is not None:
            hx, hz = footprint
            inside = all(s.covers_xz(x + dx, z + dz, pad=0.02)
                         for dx in (-hx, hx) for dz in (-hz, hz))
            if not inside and relation == "on_surface":
                return (f"the item's {2 * hx:.2f} x {2 * hz:.2f} m footprint does not fit on "
                        f"'{s.id}'")
        return None
    if relation == "hang_from":
        s = bottom_above(sup, x, z, y)
        if s is None or s.lo[1] - y > HANG_TOL_M:
            return f"nothing to hang from: no geometry bottom within {HANG_TOL_M * 100:.0f} cm above"
        return None
    if relation == "mount_on_wall":
        for s in sup:
            if all(s.lo[i] - 0.04 <= p[i] <= s.hi[i] + 0.04 for i in range(3)):
                return None
        return "not on a wall: no geometry within 4 cm of the point"
    return None


def validate_anchors(doc: dict, requests: list[dict]) -> list[str]:
    """Every placement request satisfied by a real, supported anchor."""
    if not requests:
        return []
    anchors, sets = _anchor_index(doc)
    sup = supports(doc.get("entities") or [])
    floor_y = None
    for e in doc.get("entities") or []:
        if isinstance(e, dict) and "ground" in (e.get("tags") or []):
            b = world_bounds(e)
            floor_y = b[1][1] if b else 0.0
    problems: list[str] = []
    for r in requests:
        size = r.get("size_m")
        footprint = (0.5 * size[0], 0.5 * size[2]) if size else None
        rel = r["relation"]
        if r.get("anchor"):
            a = anchors.get(r["anchor"])
            if a is None:
                problems.append(f"anchor '{r['anchor']}' for {r['entity']} was not declared "
                                f"(call w.gameplay_anchor with that id)")
                continue
            p = [float(v) for v in (a.get("position") or [0, 0, 0])[:3]]
            why = _check_point(rel, p, sup, floor_y, footprint)
            if why:
                problems.append(f"anchor '{r['anchor']}' ({rel}) at "
                                f"({p[0]:.2f}, {p[1]:.2f}, {p[2]:.2f}): {why}")
        if r.get("anchor_set"):
            s = sets.get(r["anchor_set"])
            if s is None:
                problems.append(f"anchor set '{r['anchor_set']}' for {r['entity']} was not "
                                f"declared (call w.anchor_set with that id)")
                continue
            pts = [[float(v) for v in q[:3]] for q in s.get("points") or [] if len(q) >= 3]
            if len(pts) < int(r.get("count", 1)):
                problems.append(f"anchor set '{r['anchor_set']}' has {len(pts)} point(s); "
                                f"{r.get('count', 1)} requested")
            bad = [(i, w) for i, q in enumerate(pts)
                   if (w := _check_point(rel, q, sup, floor_y, None))]
            if bad:
                i, why = bad[0]
                problems.append(f"anchor set '{r['anchor_set']}': {len(bad)} point(s) fail "
                                f"{rel}, e.g. point {i}: {why}")
    return problems


def requirement_claim_problems(doc: dict, layout_reqs: list[Any]) -> list[str]:
    """Layout requirements the world did not claim with ``w.satisfies``."""
    claimed = {str((c or {}).get("id")) for c in doc.get("requirements") or []}
    return [f"layout requirement '{r.id}' not addressed: {r.statement} -- build it and call "
            f"w.satisfies(\"{r.id}\", \"how\")"
            for r in layout_reqs if r.id not in claimed]


# ---------------------------------------------------------------------------
# Compile: resolve, settle, validate
# ---------------------------------------------------------------------------

def _set_transform(entity: dict, pos: list[float], rot: list[float]) -> None:
    t = entity.setdefault("transform", {})
    t["position"] = [round(v, 5) for v in pos]
    t["rotation"] = [round(v, 7) for v in rot]


#: A loose item taller than this many times its narrowest footprint is
#: standing on an edge: physics will topple it (the Navy SEAL rifles rested
#: on their magazines and fell 22 cm the moment the game started).
UNSTABLE_RATIO = 1.5


def _loose_item(entity: dict) -> bool:
    """A dynamic item the player picks up (a gun, a grenade, a tool)."""
    if (entity.get("physics") or {}).get("kind") != "dynamic":
        return False
    tags = entity.get("tags") or []
    bound = {str(b.get("builtin_id") or b.get("id")) for b in entity.get("mechanic_bindings") or []
             if isinstance(b, dict)}
    return "grabbable" in tags or "builtin.grab" in bound


#: Hull points within this band of the lowest one are what the item rests
#: on: 10% of its height, clamped. Measured hulls are sparse (~40 points), so
#: a flat side rarely has three points at exactly the same height.
CONTACT_BAND_M = (0.012, 0.03)


def _hull_points(entity: dict) -> list[list[float]]:
    coll = (entity.get("mesh") or {}).get("collision")
    pts = coll.get("hull") if isinstance(coll, dict) else None
    return [[float(v) for v in p[:3]] for p in pts or [] if len(p) >= 3]


def _convex_xz(points: list[list[float]]) -> list[tuple[float, float]]:
    """Monotone-chain convex hull of the points' (x, z)."""
    pts = sorted({(round(p[0], 5), round(p[2], 5)) for p in points})
    if len(pts) < 3:
        return pts

    def cross(o, a, b):
        return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0])

    lower: list[tuple[float, float]] = []
    for p in pts:
        while len(lower) >= 2 and cross(lower[-2], lower[-1], p) <= 0:
            lower.pop()
        lower.append(p)
    upper: list[tuple[float, float]] = []
    for p in reversed(pts):
        while len(upper) >= 2 and cross(upper[-2], upper[-1], p) <= 0:
            upper.pop()
        upper.append(p)
    return lower[:-1] + upper[:-1]


def _inside_polygon(poly: list[tuple[float, float]], x: float, z: float, margin: float) -> bool:
    """Inside a counter-clockwise convex polygon, at least ``margin`` from
    every edge."""
    if len(poly) < 3:
        return False
    for i, a in enumerate(poly):
        b = poly[(i + 1) % len(poly)]
        ex, ez = b[0] - a[0], b[1] - a[1]
        length = math.hypot(ex, ez)
        if length < 1e-9:
            continue
        if (ex * (z - a[1]) - ez * (x - a[0])) / length < margin:
            return False
    return True


def tips_over(entity: dict, rot: Optional[list[float]] = None) -> bool:
    """Would this item fall over resting as rotated? Its centre (of the
    measured box) must stand over the footprint of the hull points it rests
    on. A rifle upright on its magazine and grip is 1.49x as tall as it is
    wide -- under any ratio rule -- and still topples."""
    hull = _hull_points(entity)
    lb = entity_local_bounds(entity)
    if len(hull) < 4 or lb is None:
        return False
    _, base_rot, scale = _transform(entity)
    q = rot if rot is not None else base_rot
    pts = [_qrot(q, [c * scale for c in p]) for p in hull]
    bottom = min(p[1] for p in pts)
    band = min(max(0.1 * (max(p[1] for p in pts) - bottom), CONTACT_BAND_M[0]),
               CONTACT_BAND_M[1])
    contact = [p for p in pts if p[1] <= bottom + band]
    centre = _qrot(q, [0.5 * (lb[0][i] + lb[1][i]) * scale for i in range(3)])
    return not _inside_polygon(_convex_xz(contact), centre[0], centre[2], 0.004)


def _free_loose_item(entity: dict) -> bool:
    """A loose item physics will move at load: not attached to anything,
    not a hidden spawn template."""
    return (_loose_item(entity) and not entity.get("attached_to")
            and "template" not in (entity.get("tags") or []))


def lay_flat(entity: dict) -> bool:
    """Turn a loose item standing on an edge onto its widest face, keeping
    its heading. Returns True when it was turned."""
    if not _loose_item(entity):
        return False
    lb = entity_local_bounds(entity)
    if lb is None:
        return False
    pos, rot, scale = _transform(entity)
    lo, hi = rotated_bounds(lb[0], lb[1], rot, scale)
    ext = [hi[i] - lo[i] for i in range(3)]
    tall = ext[1] >= 0.06 and ext[1] > UNSTABLE_RATIO * min(ext[0], ext[2])
    if not tall and not (ext[1] >= 0.06 and tips_over(entity)):
        return False
    local = [lb[1][i] - lb[0][i] for i in range(3)]
    thin = min(range(3), key=lambda i: local[i])
    axis = [1.0 if i == thin else 0.0 for i in range(3)]
    if abs(_qrot(rot, axis)[1]) > 0.95:
        return False  # already lying on its widest face
    half = math.sqrt(0.5)
    # Rotate the thinnest local axis onto +Y: x -> y about z, z -> y about x.
    sides = ([[0.0, 0.0, half, half], [0.0, 0.0, -half, half]] if thin == 0
             else [[-half, 0.0, 0.0, half], [half, 0.0, 0.0, half]])
    yaw = yaw_of(rot)
    candidates = [_qmul(yaw_quat(yaw), lay) for lay in sides]
    new_rot = next((q for q in candidates if not tips_over(entity, q)), candidates[0])
    nlo, nhi = rotated_bounds(lb[0], lb[1], new_rot, scale)
    if nhi[1] - nlo[1] >= ext[1]:
        return False
    # Keep the bottom where it was; settle / the anchor rests it.
    pos[1] += lo[1] - nlo[1]
    _set_transform(entity, pos, new_rot)
    return True


def _place_on_anchor(entity: dict, relation: str, anchor_pos: list[float],
                     facing: Optional[list[float]], normal: Optional[list[float]]) -> bool:
    if relation in RESTING:
        lay_flat(entity)
    lb = entity_local_bounds(entity)
    pos, rot, scale = _transform(entity)
    base_rot = _without_yaw(rot)
    face = normal if relation == "mount_on_wall" and normal else facing
    yaw = yaw_from_forward(face) if face else None
    new_rot = _qmul(yaw_quat(yaw), base_rot) if yaw is not None else rot
    if lb is None:
        _set_transform(entity, list(anchor_pos), new_rot)
        return True
    lo, hi = rotated_bounds(lb[0], lb[1], new_rot, scale)
    c = [0.5 * (lo[i] + hi[i]) for i in range(3)]
    ax, ay, az = anchor_pos
    if relation in RESTING:
        p = [ax - c[0], ay - lo[1] + REST_CLEARANCE_M, az - c[2]]
    elif relation == "hang_from":
        p = [ax - c[0], ay - hi[1], az - c[2]]
    elif relation == "mount_on_wall" and normal:
        # Back face on the wall plane through the anchor, centred on it:
        # p = anchor - c + n * (c.n - min(corner.n)).
        n = [float(v) for v in normal[:3]]
        ln = math.sqrt(sum(v * v for v in n)) or 1.0
        n = [v / ln for v in n]
        back = min(sum(q[i] * n[i] for i in range(3)) for q in _corners(lo, hi))
        cdot = sum(c[i] * n[i] for i in range(3))
        p = [anchor_pos[i] - c[i] + n[i] * (cdot - back) for i in range(3)]
    else:
        p = [ax, ay, az]
    _set_transform(entity, p, new_rot)
    return True


def resolve(doc: dict, on_warning: Optional[WarnFn] = None) -> dict:
    """Rest every gameplay entity that declared a placement on its anchor.

    Returns a report ``{resolved: [...], missing: [...]}``. Entities whose
    anchor the world did not declare keep their (relocated) transform and
    are listed as missing -- :func:`violations` turns that into an error."""
    anchors, sets = _anchor_index(doc)
    report: dict[str, list] = {"resolved": [], "missing": []}
    for e in doc.get("entities") or []:
        if not isinstance(e, dict):
            continue
        p = e.get("placement")
        if not isinstance(p, dict):
            continue
        rel = str(p.get("relation") or "free")
        aid = str(p.get("anchor") or "")
        if aid:
            a = anchors.get(aid)
            if a is None:
                report["missing"].append({"entity": e.get("id"), "anchor": aid})
                continue
            _place_on_anchor(e, rel, [float(v) for v in (a.get("position") or [0, 0, 0])[:3]],
                             a.get("facing"), a.get("normal"))
            if rel in RESTING and (e.get("physics") or {}).get("kind") == "dynamic":
                tags = e.setdefault("tags", [])
                if "settled" not in tags:
                    tags.append("settled")
            report["resolved"].append({"entity": e.get("id"), "anchor": aid, "relation": rel})
        aset = str(p.get("anchor_set") or "")
        if aset and aset not in sets:
            report["missing"].append({"entity": e.get("id"), "anchor_set": aset})
    if on_warning and report["missing"]:
        on_warning(f"placement: {len(report['missing'])} anchor(s) the world never declared: "
                   + ", ".join(str(m.get("anchor") or m.get("anchor_set"))
                               for m in report["missing"][:8]))
    return report


def _is_prop(e: dict) -> bool:
    mesh = e.get("mesh") or {}
    if not isinstance(mesh, dict) or not mesh.get("asset_id"):
        return False
    if mesh.get("skinned") or mesh.get("model_frame"):
        return False
    tags = e.get("tags") or []
    if any(t in tags for t in ("template", "player_avatar", "npc", "enemy", "ally", "hud",
                               "vfx", "environment")):
        return False
    if e.get("attached_to"):
        return False
    pos = ((e.get("transform") or {}).get("position")) or [0, 0, 0]
    if float(pos[1]) < -10.0:
        return False
    return bool(e.get("physics"))


def settle(doc: dict, on_warning: Optional[WarnFn] = None) -> int:
    """Lift every prop that is sunk into what it stands on.

    A Tripo GLB is centred on its origin, so a table authored "at the floor"
    has half of itself under it. This rests each prop's real bottom on the
    highest support beneath its footprint centre, static props first (so a
    lifted table can then carry the pistol placed on it). Props resolved
    onto an anchor keep the anchor's x / z and are moved only onto the real
    top of the support under them (:func:`_rerest_anchored`)."""
    ents = [e for e in doc.get("entities") or [] if isinstance(e, dict)]
    resolved = {str(e.get("id")) for e in ents
                if isinstance(e.get("placement"), dict) and e["placement"].get("anchor")}
    moved = 0
    for e in ents:
        if str(e.get("id")) not in resolved and _is_prop(e) and lay_flat(e):
            moved += 1
            if on_warning:
                on_warning(f"'{e.get('id')}' stood on an edge; laid on its side")
    for phase in ("static", "dynamic"):
        sup = supports(ents)
        for e in ents:
            if str(e.get("id")) in resolved or not _is_prop(e):
                continue
            kind = (e.get("physics") or {}).get("kind") or "static"
            if (phase == "static") != (kind == "static"):
                continue
            b = world_bounds(e)
            if b is None:
                continue
            lo, hi = b
            height = hi[1] - lo[1]
            cx, cz = 0.5 * (lo[0] + hi[0]), 0.5 * (lo[2] + hi[2])
            others = [s for s in sup if s.id != str(e.get("id"))]
            loose = kind == "dynamic" and _free_loose_item(e)
            # The support it is sunk into: highest top between its bottom
            # and 60 % of the way up it. A loose item looks up to its own
            # top: a pistol sunk 10 cm into a table top belongs ON the
            # table, and the floor under the table is not its support.
            s = top_below(others, cx, cz, (hi[1] if loose else lo[1] + 0.6 * height), tol=0.0)
            if s is not None and loose and lo[1] - s.hi[1] > FLOAT_TOL_M:
                # A loose item in mid-air: physics drops it on frame one and
                # it bounces to wherever. Rest it on what is under it instead
                # (the Navy SEAL rifles were authored at chest height in
                # front of the spawn and rolled about on the sand).
                dy = s.hi[1] - lo[1] + REST_CLEARANCE_M
                pos, rot, _ = _transform(e)
                pos[1] += dy
                _set_transform(e, pos, rot)
                tags = e.setdefault("tags", [])
                if "settled" not in tags:
                    tags.append("settled")
                moved += 1
                if on_warning:
                    on_warning(f"'{e.get('id')}' floated {-dy * 100:.0f} cm above '{s.id}'; "
                               "rested on it")
                continue
            if s is None or s.hi[1] - lo[1] <= SUNK_TOL_M:
                continue
            dy = s.hi[1] - lo[1] + REST_CLEARANCE_M
            pos, rot, _ = _transform(e)
            pos[1] += dy
            _set_transform(e, pos, rot)
            if kind == "dynamic":
                tags = e.setdefault("tags", [])
                if "settled" not in tags:
                    tags.append("settled")
            moved += 1
            if on_warning:
                on_warning(f"'{e.get('id')}' was {dy * 100:.0f} cm into '{s.id}'; "
                           f"rested on it")
    moved += _rerest_anchored(ents, resolved, on_warning)
    return moved


#: How far an anchored item's bottom may be from the real top of the support
#: under it and still be moved onto that top.
REANCHOR_TOL_M = 0.12


def _rerest_anchored(ents: list[dict], resolved: set[str],
                     on_warning: Optional[WarnFn]) -> int:
    """Rest anchored items on the support actually under them.

    A world anchor's height is the architect's idea of the furniture; the
    furniture itself may be a prop whose measured top is a little higher
    (settled above, after the anchors resolved). Every rifle on the rerun's
    tables sat 4 cm into them. Within ``REANCHOR_TOL_M`` the geometry wins;
    a bigger gap is a wrong anchor and :func:`violations` reports it."""
    moved = 0
    sup = supports(ents)
    for e in ents:
        eid = str(e.get("id"))
        p = e.get("placement")
        if eid not in resolved or not isinstance(p, dict) or str(p.get("relation")) not in RESTING:
            continue
        b = world_bounds(e)
        if b is None:
            continue
        lo, hi = b
        cx, cz = 0.5 * (lo[0] + hi[0]), 0.5 * (lo[2] + hi[2])
        s = top_below([x for x in sup if x.id != eid], cx, cz, lo[1] + REANCHOR_TOL_M, tol=0.0)
        if s is None or s.lo[1] > lo[1] + SUNK_TOL_M:
            continue  # nothing under it, or an overhang above it
        dy = s.hi[1] - lo[1] + REST_CLEARANCE_M
        if abs(dy) <= SUNK_TOL_M or abs(dy) > REANCHOR_TOL_M:
            continue
        pos, rot, _ = _transform(e)
        pos[1] += dy
        _set_transform(e, pos, rot)
        moved += 1
        if on_warning:
            on_warning(f"'{eid}' rested on the real top of '{s.id}' "
                       f"({dy * 100:+.0f} cm from its anchor)")
    return moved


def violations(doc: dict, strict_ids: Optional[set[str]] = None) -> tuple[list[str], list[str]]:
    """``(errors, warnings)`` for placement. Entities that DECLARED a
    placement (``strict_ids``, default: every entity with one) fail the
    build; legacy entities only warn."""
    ents = [e for e in doc.get("entities") or [] if isinstance(e, dict)]
    strict = strict_ids if strict_ids is not None else {
        str(e.get("id")) for e in ents if isinstance(e.get("placement"), dict)}
    errors: list[str] = []
    warnings: list[str] = []
    anchors, sets = _anchor_index(doc)
    sup = supports(ents)

    def report(eid: str, msg: str) -> None:
        (errors if eid in strict else warnings).append(f"'{eid}': {msg}")

    props = [e for e in ents if _is_prop(e)]
    boxes: dict[str, tuple[list[float], list[float]]] = {}
    for e in props:
        b = world_bounds(e)
        if b is None:
            continue
        eid = str(e.get("id"))
        boxes[eid] = b
        p = e.get("placement") if isinstance(e.get("placement"), dict) else {}
        rel = str(p.get("relation") or "")
        if p.get("anchor") and str(p["anchor"]) not in anchors:
            report(eid, f"its anchor '{p['anchor']}' was never declared by the world")
            continue
        if p.get("anchor_set") and str(p["anchor_set"]) not in sets:
            report(eid, f"its anchor set '{p['anchor_set']}' was never declared by the world")
        lo, hi = b
        cx, cz = 0.5 * (lo[0] + hi[0]), 0.5 * (lo[2] + hi[2])
        others = [s for s in sup if s.id != eid]
        kind = (e.get("physics") or {}).get("kind") or "static"
        if rel == "hang_from":
            s = bottom_above(others, cx, cz, hi[1])
            if s is None:
                report(eid, "hangs from nothing")
            continue
        if rel == "mount_on_wall":
            continue
        s = top_below(others, cx, cz, lo[1] + 0.6 * (hi[1] - lo[1]), tol=0.0)
        if s is not None and s.hi[1] - lo[1] > SUNK_TOL_M:
            report(eid, f"sunk {100 * (s.hi[1] - lo[1]):.0f} cm into '{s.id}'")
            continue
        s = top_below(others, cx, cz, lo[1] + SUPPORT_TOL_M)
        gap = lo[1] - s.hi[1] if s is not None else None
        if rel in RESTING or kind == "dynamic":
            if s is None:
                report(eid, "has nothing under it")
            elif gap is not None and gap > FLOAT_TOL_M and rel in RESTING:
                report(eid, f"floats {100 * gap:.0f} cm above '{s.id}'")
    ids = sorted(boxes)
    for i, a in enumerate(ids):
        for b in ids[i + 1:]:
            (alo, ahi), (blo, bhi) = boxes[a], boxes[b]
            if all(min(ahi[k], bhi[k]) - max(alo[k], blo[k]) > OVERLAP_TOL_M for k in range(3)):
                if a in strict or b in strict:
                    errors.append(f"'{a}' and '{b}' interpenetrate")
                else:
                    warnings.append(f"'{a}' and '{b}' interpenetrate")
    return errors, warnings


def drop_world_owned_copies(doc: dict, world_entities: list[dict], owned: set[str],
                            on_warning: Optional[WarnFn] = None) -> int:
    """Remove a gameplay copy of world-owned furniture the world also placed
    (only for scenes composed with placements)."""
    placed = {(e.get("mesh") or {}).get("asset_id") for e in world_entities
              if isinstance(e, dict)}
    keep, dropped = [], []
    world_ids = {str(e.get("id")) for e in world_entities if isinstance(e, dict)}
    for e in doc.get("entities") or []:
        aid = (e.get("mesh") or {}).get("asset_id") if isinstance(e, dict) else None
        if (aid in owned and aid in placed and str(e.get("id")) not in world_ids
                and not e.get("mechanic_bindings")):
            dropped.append(str(e.get("id")))
            continue
        keep.append(e)
    if dropped:
        doc["entities"] = keep
        if on_warning:
            on_warning(f"dropped gameplay copies of world-built furniture: {dropped}")
    return len(dropped)


def write_report(path: Path, report: dict, errors: list[str], warnings: list[str],
                 settled: int) -> None:
    path.write_text(json.dumps({**report, "settled": settled, "errors": errors,
                                "warnings": warnings}, indent=2), encoding="utf-8")
