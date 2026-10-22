"""Keep the gameplay layer with the player when the world moves the spawn.

The SceneSpec composer lays out the gameplay layer -- the weapon within
arm's reach, the HUD panel in front, the ally a couple of metres away --
around ITS player spawn, which is normally the origin. The world architect
then builds the level and puts the player wherever the level wants them to
start. Merging took the world's spawn but left every gameplay entity where
the composer put it, so in one game the blaster, the mission panel and the
friendly NPC were all thirty metres from the player, in the middle of a
level the player had never seen.

This module moves the layer as one rigid body -- the same yaw and
translation that takes the SceneSpec spawn onto the world spawn -- and then
nudges anything that landed inside world geometry out onto walkable floor.
"""

from __future__ import annotations

import math
from typing import Any, Callable, Iterable, Optional

#: Entities that must not be moved with the layer.
_SKIP_TAGS = ("template", "player_avatar", "player_rig", "environment")
#: A spawn closer than this to the world's is the same spawn.
_SAME_SPAWN_M = 0.25
_SAME_YAW_RAD = math.radians(2.0)
#: Search for a free spot this far out, in these steps.
_NUDGE_MAX_M = 8.0
_NUDGE_STEP_M = 0.5
_NUDGE_ANGLES = 16
#: Bodies standing on the floor collide with boxes between these heights
#: above their base.
_BODY_LOW_M = 0.25
_BODY_HIGH_M = 1.6
#: Characters keep this clearance from blocking geometry.
_CLEARANCE_M = 0.35

WarnFn = Callable[[str], None]


# ---------------------------------------------------------------------------
# Small maths on plain lists (scene JSON stores [x, y, z] / [x, y, z, w]).
# ---------------------------------------------------------------------------


def yaw_from_quat(q: Iterable[float]) -> float:
    """Heading of a rotation: the yaw that turns -Z onto its forward."""
    x, y, z, w = (list(q) + [0.0, 0.0, 0.0, 1.0])[:4]
    fx = -(2.0 * (x * z + w * y))
    fz = -(1.0 - 2.0 * (x * x + y * y))
    return math.atan2(-fx, -fz)


def yaw_from_direction(d: Iterable[float]) -> Optional[float]:
    dx, _dy, dz = (list(d) + [0.0, 0.0, 0.0])[:3]
    if math.hypot(dx, dz) < 1e-6:
        return None
    return math.atan2(-dx, -dz)


def _quat_yaw(yaw: float) -> list[float]:
    return [0.0, math.sin(yaw / 2.0), 0.0, math.cos(yaw / 2.0)]


def _quat_mul(a: list[float], b: list[float]) -> list[float]:
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return [
        aw * bx + ax * bw + ay * bz - az * by,
        aw * by - ax * bz + ay * bw + az * bx,
        aw * bz + ax * by - ay * bx + az * bw,
        aw * bw - ax * bx - ay * by - az * bz,
    ]


def _rotate(q: list[float], v: list[float]) -> list[float]:
    qv = [v[0], v[1], v[2], 0.0]
    qc = [-q[0], -q[1], -q[2], q[3]]
    r = _quat_mul(_quat_mul(q, qv), qc)
    return r[:3]


def _rotate_yaw_xz(x: float, z: float, yaw: float) -> tuple[float, float]:
    c, s = math.cos(yaw), math.sin(yaw)
    # Same handedness as a quaternion about +Y: (x, z) -> (x c + z s, -x s + z c)
    return x * c + z * s, -x * s + z * c


# ---------------------------------------------------------------------------
# Relocation
# ---------------------------------------------------------------------------


def _skip(entity: dict) -> bool:
    tags = entity.get("tags") or []
    if any(t in tags for t in _SKIP_TAGS):
        return True
    if entity.get("attached_to"):
        return True  # follows its parent
    pos = ((entity.get("transform") or {}).get("position")) or [0.0, 0.0, 0.0]
    return float(pos[1]) < -10.0  # parked off-map on purpose


def relocate_gameplay_layer(
    entities: list[dict],
    spec_spawn: list[float],
    spec_yaw: float,
    world_spawn: list[float],
    world_yaw: float,
    on_warning: Optional[WarnFn] = None,
) -> int:
    """Move every gameplay entity from the SceneSpec spawn frame into the
    world spawn frame. Returns how many moved. Mutates ``entities``."""

    dx = float(world_spawn[0]) - float(spec_spawn[0])
    dz = float(world_spawn[2]) - float(spec_spawn[2])
    dyaw = math.atan2(math.sin(world_yaw - spec_yaw), math.cos(world_yaw - spec_yaw))
    if math.hypot(dx, dz) < _SAME_SPAWN_M and abs(dyaw) < _SAME_YAW_RAD:
        return 0
    # The SceneSpec stands its player on y = 0 (its spawn y is eye height);
    # the world reports the spawn at the feet.
    spec_floor = float(spec_spawn[1]) if float(spec_spawn[1]) < 1.0 else 0.0
    dy = float(world_spawn[1]) - spec_floor
    qd = _quat_yaw(dyaw)

    moved = 0
    for e in entities:
        if _skip(e):
            continue
        t = e.get("transform")
        if not isinstance(t, dict):
            continue
        p = [float(v) for v in (t.get("position") or [0.0, 0.0, 0.0])[:3]]
        rx, rz = _rotate_yaw_xz(p[0] - spec_spawn[0], p[2] - spec_spawn[2], dyaw)
        t["position"] = [world_spawn[0] + rx, p[1] + dy, world_spawn[2] + rz]
        r = [float(v) for v in (t.get("rotation") or [0.0, 0.0, 0.0, 1.0])[:4]]
        t["rotation"] = _quat_mul(qd, r)
        moved += 1
    if moved and on_warning:
        on_warning(
            f"world spawn is {math.hypot(dx, dz):.1f} m and "
            f"{math.degrees(dyaw):.0f} deg from the SceneSpec spawn; moved "
            f"{moved} gameplay entities with the player"
        )
    return moved


# ---------------------------------------------------------------------------
# Overlap repair
# ---------------------------------------------------------------------------


class _Box:
    def __init__(self, entity: dict) -> None:
        t = entity.get("transform") or {}
        self.center = [float(v) for v in (t.get("position") or [0, 0, 0])[:3]]
        self.rot = [float(v) for v in (t.get("rotation") or [0, 0, 0, 1])[:4]]
        prim = entity.get("primitive") or {}
        scale = float(t.get("scale") or 1.0)
        size = [float(v) * scale for v in (prim.get("size") or [1, 1, 1])[:3]]
        self.half = [s * 0.5 for s in size]
        self.id = entity.get("id")

    def contains(self, p: list[float], pad: float) -> bool:
        inv = [-self.rot[0], -self.rot[1], -self.rot[2], self.rot[3]]
        d = [p[i] - self.center[i] for i in range(3)]
        local = _rotate(inv, d)
        return (abs(local[0]) <= self.half[0] + pad
                and abs(local[2]) <= self.half[2] + pad
                and abs(local[1]) <= self.half[1])

    def blocks_body_at(self, p: list[float], pad: float) -> bool:
        """Does this box occupy the space of a body standing at ``p``?"""
        for h in (_BODY_LOW_M, 0.9, _BODY_HIGH_M):
            if self.contains([p[0], p[1] + h, p[2]], pad):
                return True
        return False


def _static_boxes(world_entities: list[dict]) -> list[_Box]:
    out: list[_Box] = []
    for e in world_entities:
        phys = e.get("physics") or {}
        prim = e.get("primitive") or {}
        if phys.get("kind") != "static":
            continue
        if (prim.get("kind") or prim.get("shape")) not in ("box", "cube", None):
            continue
        if not prim.get("size"):
            continue
        out.append(_Box(e))
    return out


def _in_regions(p: list[float], regions: list[dict]) -> bool:
    if not regions:
        return True
    for r in regions:
        lo, hi = r.get("min") or [0, 0, 0], r.get("max") or [0, 0, 0]
        if lo[0] <= p[0] <= hi[0] and lo[2] <= p[2] <= hi[2]:
            return True
    return False


def _region_floor(p: list[float], regions: list[dict]) -> Optional[float]:
    for r in regions:
        lo, hi = r.get("min") or [0, 0, 0], r.get("max") or [0, 0, 0]
        if lo[0] <= p[0] <= hi[0] and lo[2] <= p[2] <= hi[2]:
            return float(r.get("floor_y", lo[1]))
    return None


def _is_character(entity: dict) -> bool:
    tags = entity.get("tags") or []
    mesh = entity.get("mesh") or {}
    return bool(mesh.get("skinned")) or any(
        t in tags for t in ("npc", "ally", "enemy", "friendly", "companion"))


def free_spot(
    p: list[float],
    boxes: list[_Box],
    regions: list[dict],
    pad: float,
    need_region: bool,
) -> Optional[list[float]]:
    """Nearest point to ``p`` (ring search) that no box blocks and, when
    asked, that lies on declared walkable floor."""

    def ok(q: list[float]) -> bool:
        if need_region and not _in_regions(q, regions):
            return False
        return not any(b.blocks_body_at(q, pad) for b in boxes)

    if ok(p):
        return p
    r = _NUDGE_STEP_M
    while r <= _NUDGE_MAX_M + 1e-6:
        for k in range(_NUDGE_ANGLES):
            a = 2.0 * math.pi * k / _NUDGE_ANGLES
            q = [p[0] + r * math.cos(a), p[1], p[2] + r * math.sin(a)]
            if ok(q):
                return q
        r += _NUDGE_STEP_M
    return None


def resolve_overlaps(
    gameplay: list[dict],
    world_entities: list[dict],
    regions: list[dict],
    on_warning: Optional[WarnFn] = None,
) -> int:
    """Move gameplay entities out of world geometry. Characters are also
    put on declared walkable floor. Returns how many moved."""

    boxes = _static_boxes(world_entities)
    if not boxes and not regions:
        return 0
    moved = 0
    for e in gameplay:
        if _skip(e):
            continue
        t = e.get("transform")
        if not isinstance(t, dict):
            continue
        p = [float(v) for v in (t.get("position") or [0, 0, 0])[:3]]
        character = _is_character(e)
        pad = _CLEARANCE_M if character else 0.0
        need_region = character and bool(regions)
        q = free_spot(p, boxes, regions, pad, need_region)
        if q is None or (abs(q[0] - p[0]) < 1e-6 and abs(q[2] - p[2]) < 1e-6):
            continue
        if character:
            floor = _region_floor(q, regions)
            if floor is not None:
                q[1] = floor
        t["position"] = q
        moved += 1
        if on_warning:
            on_warning(
                f"'{e.get('id')}' was inside world geometry at "
                f"({p[0]:.1f}, {p[2]:.1f}); moved to ({q[0]:.1f}, {q[2]:.1f})"
            )
    return moved


def spawn_pose(spawn: Any) -> tuple[Optional[list[float]], Optional[float]]:
    """Position and yaw of a scene.json ``player.spawn`` block."""
    if not isinstance(spawn, dict) or "position" not in spawn:
        return None, None
    pos = [float(v) for v in spawn["position"][:3]]
    yaw = yaw_from_quat(spawn.get("rotation") or [0, 0, 0, 1])
    return pos, yaw
