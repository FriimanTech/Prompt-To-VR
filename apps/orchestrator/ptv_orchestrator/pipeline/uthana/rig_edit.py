"""Edits to a rigged GLB's skeleton that keep the skin consistent.

The engine's origin contract: a character's origin sits at its vertical
MIDDLE (the hips), so the engine plants it half its height above the ground.
Tripo rigs already follow it. Uthana can produce either convention (its
``auto_rig_feet_on_floor`` flag, and de-duplicated uploads that keep the
FIRST upload's flags), and its locomotion clips put the feet at the origin.

Both are fixed by moving the root joint (``Hips``) by a constant WORLD offset.
The offset has to be expressed in the Hips parent's local frame -- for a
Uthana rig that parent is an ``Armature`` node scaled by 0.01 and rotated 90
degrees about X, so world "up" is a centimetre-scaled local -Z, not +Y.
Inverse bind matrices are left untouched: every joint's world matrix moves by
the same offset, so the skinned mesh moves with them.
"""

from __future__ import annotations

from typing import Optional, Sequence

from .gltf_io import Glb, GltfError, inverse3, linear3, mul3_vec

HIPS_NAMES = ("hips", "pelvis", "root", "hip")


def _strip_ns(name: str) -> str:
    return name.rsplit(":", 1)[-1].lower()


def find_hips(glb: Glb) -> int:
    """Index of the root joint: a joint named Hips/Pelvis, else the skin root."""
    joints = glb.skin_joint_set()
    for key in HIPS_NAMES:
        for i in sorted(joints):
            if _strip_ns(str(glb.nodes[i].get("name") or "")) == key:
                return i
    parent = glb.parents()
    roots = [j for j in sorted(joints) if parent[j] not in joints]
    if not roots:
        raise GltfError("skin has no root joint")
    return roots[0]


def world_to_parent_local(glb: Glb, node: int, delta_world: Sequence[float]) -> tuple[float, float, float]:
    """Express a world-space displacement in ``node``'s parent frame."""
    parent = glb.parents()[node]
    if parent < 0:
        return (float(delta_world[0]), float(delta_world[1]), float(delta_world[2]))
    world = glb.world_matrices()
    return mul3_vec(inverse3(linear3(world[parent])), delta_world)


def shift_rest(glb: Glb, node: int, delta_local: Sequence[float]) -> None:
    """Add ``delta_local`` to a node's rest translation."""
    n = glb.nodes[node]
    if "matrix" in n:
        m = list(n["matrix"])
        m[12] += delta_local[0]
        m[13] += delta_local[1]
        m[14] += delta_local[2]
        n["matrix"] = m
        return
    t = list(n.get("translation") or [0.0, 0.0, 0.0])
    n["translation"] = [t[0] + delta_local[0], t[1] + delta_local[1], t[2] + delta_local[2]]


def shift_hips_world(glb: Glb, dy_world: float, hips: Optional[int] = None) -> tuple[float, float, float]:
    """Move the rest pose vertically by ``dy_world`` metres. Returns the local delta."""
    hips = find_hips(glb) if hips is None else hips
    delta = world_to_parent_local(glb, hips, (0.0, dy_world, 0.0))
    shift_rest(glb, hips, delta)
    return delta
