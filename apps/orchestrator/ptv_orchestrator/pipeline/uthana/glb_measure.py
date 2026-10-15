#!/usr/bin/env python3
"""Measure the world-space height (metres) of a rigged character GLB.

Offline analysis only -- no API calls.

Why bind-space POSITION spans are not enough: Uthana GLBs place the skeleton
under an ``Armature`` node with scale ~0.01 and usually a 90 degree X
rotation, with bones authored in centimetres. The real height only appears
after walking the node hierarchy, applying every node's TRS (or matrix) and
skinning the bind-pose vertices through the joint matrices.

Reported values:

* bind (raw POSITION) span and its longest axis -- what the engine
  normalises by: in-engine scale = ``target_scale_meters / raw_longest``;
* world mesh AABB height (metres) -- the PRIMARY value: Uthana animates the
  character at this size, so Uthana ``move_speed`` is computed from it
  (walk = 0.78 x height, run = 2.2 x height, the engine's own factors);
* skeleton toe-to-head height -- a secondary sanity check.

Usage::

    python -m ptv_orchestrator.pipeline.uthana.glb_measure rigged_character.glb \
        --expected-target 1.8
"""

from __future__ import annotations

import argparse
import math
import sys
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Optional, Sequence

from .gltf_io import Glb, GltfError, aabb, raw_position_bounds, skinned_bind_positions

WALK_SPEED_PER_M = 0.78
RUN_SPEED_PER_M = 2.2

_TOE_NAMES = ("toe_end", "toeend", "toebase", "toe", "foot")
_HEAD_TOP_NAMES = ("headtop_end", "head_end", "headtop", "head_top")


@dataclass
class HeightReport:
    path: str
    bind_raw_min: list[float]
    bind_raw_max: list[float]
    bind_raw_longest_axis: float
    bind_raw_longest_axis_name: str
    bind_raw_height: float
    world_min: list[float]
    world_max: list[float]
    world_height_m: float
    world_center_y_m: float
    toe_to_head_m: Optional[float]
    lowest_toe_y_m: Optional[float]
    head_top_y_m: Optional[float]
    expected_target_m: Optional[float]
    engine_scale: Optional[float]
    in_engine_height_m: Optional[float]
    height_for_motion_m: float
    walk_move_speed: float
    run_move_speed: float
    notes: list[str]

    def as_dict(self) -> dict:
        return asdict(self)


def _joint_names(glb: Glb) -> dict[int, str]:
    joints = glb.skin_joint_set()
    return {i: str(glb.nodes[i].get("name") or "") for i in joints}


def _strip_ns(name: str) -> str:
    return name.rsplit(":", 1)[-1].lower()


def lowest_toe_y(glb: Glb, world: Sequence) -> Optional[float]:
    names = _joint_names(glb)
    best: Optional[float] = None
    for prio in _TOE_NAMES:
        ys = [world[i][1][3] for i, n in names.items() if prio in _strip_ns(n)]
        if ys:
            best = min(ys)
            break
    return best


def head_top_y(glb: Glb, world: Sequence, height_hint: float) -> Optional[float]:
    names = _joint_names(glb)
    for key in _HEAD_TOP_NAMES:
        ys = [world[i][1][3] for i, n in names.items() if _strip_ns(n) == key]
        if ys:
            return max(ys)
    heads = [world[i][1][3] for i, n in names.items() if _strip_ns(n) == "head"]
    if heads:
        # The head joint sits at the base of the skull; the crown is roughly
        # 6% of body height above it.
        return max(heads) + 0.06 * height_hint
    return None


def measure(path: Path | str, expected_target: Optional[float] = None) -> HeightReport:
    """Measure one GLB. Raises :class:`GltfError` when it cannot be measured."""
    glb = Glb.load(path)
    if not glb.mesh_nodes():
        raise GltfError(f"{path}: no mesh")
    if not glb.doc.get("skins"):
        raise GltfError(f"{path}: no skin -- not a rigged character")
    if not glb.skin_joint_set():
        raise GltfError(f"{path}: skin has no joints")
    notes: list[str] = []

    raw_lo, raw_hi = raw_position_bounds(glb)
    extents = [raw_hi[c] - raw_lo[c] for c in range(3)]
    longest = max(extents)
    axis = "xyz"[extents.index(longest)]

    world = glb.world_matrices()
    points = skinned_bind_positions(glb)
    lo, hi = aabb(points)
    height = hi[1] - lo[1]
    for v in (*lo, *hi, height, longest):
        if not math.isfinite(v):
            raise GltfError(f"{path}: non-finite result -- transforms did not resolve")
    if height <= 1e-6:
        raise GltfError(f"{path}: zero world height")

    toe = lowest_toe_y(glb, world)
    head = head_top_y(glb, world, height)
    toe_to_head = (head - toe) if (toe is not None and head is not None) else None
    if toe_to_head is None:
        notes.append("no toe/foot or head joint found; toe-to-head skipped")
    elif abs(toe_to_head - height) > 0.15 * height:
        notes.append(
            f"toe-to-head {toe_to_head:.3f} m differs from mesh height {height:.3f} m "
            "by more than 15%; check the rig"
        )
    if axis != "y":
        notes.append(
            f"the raw longest axis is {axis} (arm span?), so the engine scales this "
            "character by its width, not its height"
        )

    engine_scale = (expected_target / longest) if (expected_target and longest > 1e-9) else None
    in_engine_height = None
    if engine_scale is not None:
        raw_world_ratio = height / (raw_hi[1] - raw_lo[1]) if raw_hi[1] > raw_lo[1] else 1.0
        in_engine_height = (raw_hi[1] - raw_lo[1]) * engine_scale * raw_world_ratio
    return HeightReport(
        path=str(path),
        bind_raw_min=raw_lo,
        bind_raw_max=raw_hi,
        bind_raw_longest_axis=longest,
        bind_raw_longest_axis_name=axis,
        bind_raw_height=raw_hi[1] - raw_lo[1],
        world_min=lo,
        world_max=hi,
        world_height_m=height,
        world_center_y_m=0.5 * (lo[1] + hi[1]),
        toe_to_head_m=toe_to_head,
        lowest_toe_y_m=toe,
        head_top_y_m=head,
        expected_target_m=expected_target,
        engine_scale=engine_scale,
        in_engine_height_m=in_engine_height,
        height_for_motion_m=height,
        walk_move_speed=WALK_SPEED_PER_M * height,
        run_move_speed=RUN_SPEED_PER_M * height,
        notes=notes,
    )


def _fmt(v: Optional[float]) -> str:
    return "n/a" if v is None else f"{v:.4f}"


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("glb", type=Path, help="Uthana-rigged character GLB")
    parser.add_argument("--expected-target", type=float, default=None,
                        help="the asset's target_scale_meters, for comparison")
    args = parser.parse_args(argv)
    try:
        r = measure(args.glb, args.expected_target)
    except (GltfError, OSError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    print(f"file                         : {r.path}")
    print(f"bind raw POSITION span       : {[round(v, 4) for v in r.bind_raw_min]} .. "
          f"{[round(v, 4) for v in r.bind_raw_max]}")
    print(f"bind raw height (y span)     : {_fmt(r.bind_raw_height)}")
    print(f"bind raw longest axis        : {_fmt(r.bind_raw_longest_axis)} ({r.bind_raw_longest_axis_name})")
    print(f"world mesh AABB height (m)   : {_fmt(r.world_height_m)}   <- primary")
    print(f"world mesh vertical centre   : {_fmt(r.world_center_y_m)}")
    print(f"skeleton toe-to-head (m)     : {_fmt(r.toe_to_head_m)}")
    if r.expected_target_m is not None:
        print(f"expected target_scale_meters : {_fmt(r.expected_target_m)}")
        print(f"engine scale (target/longest): {_fmt(r.engine_scale)}")
        print(f"in-engine height (m)         : {_fmt(r.in_engine_height_m)}")
    print(f"height_for_motion_m          : {_fmt(r.height_for_motion_m)}  "
          "(input to the Uthana move_speed calculation)")
    print(f"  walk move_speed 0.78 x h   : {r.walk_move_speed:.2f} m/s")
    print(f"  run  move_speed 2.2  x h   : {r.run_move_speed:.2f} m/s")
    for n in r.notes:
        print(f"note: {n}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
