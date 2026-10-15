#!/usr/bin/env python3
"""Merge downloaded Uthana motions into the rigged character: one animated GLB.

Runs only when EVERY motion a character needs has been downloaded, so the
result is never missing a clip. For each motion GLB (downloaded with
``no_mesh=True, in_place=True``):

* copy each sampler's keyframes (input/output accessors and their bytes)
  into the base file's buffer, with the ``min``/``max`` the spec requires on
  animation inputs;
* re-point each channel to the base node with the same name;
* keep the motion's own name as the clip name -- it is NOT renamed to an
  intent; the intent -> clip mapping is recorded separately.

Origin fix. Uthana clips put the feet at the origin; the engine (and every
Tripo character) puts the origin at the character's middle, so a Uthana clip
played as-is floats the character half its height above the floor. One
constant per character is added to the Hips translation keys of EVERY clip
(idle, walk, run alike, so they stay consistent):

    k = bind_lowest_toe_y - median(per-clip lowest toe height)

The offset is world-vertical; it is converted into the Hips parent's local
frame first, because a Uthana rig's parent (``Armature``) is scaled by 0.01
and rotated 90 degrees about X. Validation: after the shift the lowest toe
on frame 0 of every clip lands within a few centimetres of the bind pose's
lowest toe, which itself sits at roughly minus half the model's height.

Usage::

    python -m ptv_orchestrator.pipeline.uthana.glb_merge base.glb out.glb \
        walk=zombie-str5-ms1.4-deg0.glb idle=idle.glb
"""

from __future__ import annotations

import argparse
import statistics
import sys
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Optional, Sequence

from .clip_analysis import loop_seam_deg, measure_ground_speed, toe_heights
from .glb_measure import lowest_toe_y
from .gltf_io import (
    Channel,
    Glb,
    GltfError,
    aabb,
    quat_slerp,
    read_animation,
    skinned_bind_positions,
)
from .rig_edit import find_hips, world_to_parent_local

#: Allowed distance between clip feet and bind feet after the shift (m).
FEET_TOLERANCE_M = 0.03
#: Allowed distance between the bind sole and minus half the height (m).
SOLE_TOLERANCE_M = 0.05
#: Loops whose last/first frame differ by more than this get a crossfade.
SEAM_CROSSFADE_DEG = 8.0
CROSSFADE_S = 0.25


@dataclass
class MotionInput:
    key: str
    clip_name: str
    path: Path
    looping: bool = True


@dataclass
class ClipReport:
    key: str
    clip_name: str
    channels: int
    skipped_channels: list[str]
    duration_s: float
    seam_deg: float
    crossfaded: bool
    frame0_toe_y_m: Optional[float]
    min_toe_y_m: Optional[float]
    ground_speed_mps: Optional[float]


@dataclass
class MergeReport:
    out_path: str
    hips_offset_m: float
    hips_offset_local: list[float]
    bind_toe_y_m: Optional[float]
    bind_height_m: float
    bind_sole_y_m: float
    clips: list[ClipReport] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)
    ok: bool = True

    def as_dict(self) -> dict:
        return asdict(self)


def _remap(base: Glb, motion: Glb, channels: Sequence[Channel]) -> tuple[list[Channel], list[str]]:
    by_name = base.node_index_by_name()
    joints = base.skin_joint_set()
    out: list[Channel] = []
    skipped: list[str] = []
    for ch in channels:
        name = str(motion.nodes[ch.node].get("name") or "")
        target = by_name.get(name)
        if target is None or target not in joints:
            skipped.append(f"{name}.{ch.path}")
            continue
        out.append(Channel(node=target, path=ch.path, interp=ch.interp,
                           times=list(ch.times), values=list(ch.values)))
    return out, skipped


def _shift_translation(ch: Channel, delta: Sequence[float]) -> None:
    cubic = ch.interp == "CUBICSPLINE"
    vals = []
    for i, v in enumerate(ch.values):
        if cubic and i % 3 != 1:
            vals.append(v)  # tangents are unchanged by a constant offset
        else:
            vals.append(tuple(a + b for a, b in zip(v, delta)))
    ch.values = vals


def _crossfade(channels: Sequence[Channel], seconds: float) -> None:
    """Blend the tail of a loop toward its first frame so it wraps cleanly."""
    for ch in channels:
        if ch.interp == "CUBICSPLINE" or len(ch.times) < 3:
            continue
        end = ch.times[-1]
        start = max(ch.times[0], end - seconds)
        first = ch.values[0]
        vals = list(ch.values)
        for i, t in enumerate(ch.times):
            if t < start or end <= start:
                continue
            w = (t - start) / (end - start)
            if ch.path == "rotation":
                vals[i] = quat_slerp(vals[i], first, w)
            else:
                vals[i] = tuple(a + (b - a) * w for a, b in zip(vals[i], first))
        ch.values = vals


def merge_character(base_path: Path, motions: Sequence[MotionInput], out_path: Path) -> MergeReport:
    base = Glb.load(base_path)
    if not base.doc.get("skins"):
        raise GltfError(f"{base_path}: base GLB has no skin")
    dupes = base.duplicate_node_names()
    if dupes:
        raise GltfError(f"{base_path}: duplicate node names {dupes[:5]} -- cannot re-point by name")
    hips = find_hips(base)

    bind_world = base.world_matrices()
    bind_toe = lowest_toe_y(base, bind_world)
    lo, hi = aabb(skinned_bind_positions(base, max_vertices=20000))
    height = hi[1] - lo[1]

    loaded: list[tuple[MotionInput, list[Channel], list[str]]] = []
    for m in motions:
        mg = Glb.load(m.path)
        if not mg.doc.get("animations"):
            raise GltfError(f"{m.path}: motion GLB has no animation")
        channels, skipped = _remap(base, mg, read_animation(mg, 0))
        if not channels:
            raise GltfError(f"{m.path}: no channel matches a joint of the base rig")
        loaded.append((m, channels, skipped))

    # One constant per character, from where each clip puts the feet.
    floors = []
    for _, channels, _ in loaded:
        heights = toe_heights(base, channels)
        if heights:
            floors.append(min(heights))
    report_warnings: list[str] = []
    if bind_toe is None or not floors:
        k = 0.0
        report_warnings.append("no toe/foot joints found; hips offset not applied")
    else:
        k = bind_toe - statistics.median(floors)
    delta_local = world_to_parent_local(base, hips, (0.0, k, 0.0))

    report = MergeReport(out_path=str(out_path), hips_offset_m=k,
                         hips_offset_local=list(delta_local), bind_toe_y_m=bind_toe,
                         bind_height_m=height, bind_sole_y_m=lo[1],
                         warnings=report_warnings)
    if abs(lo[1] + 0.5 * height) > SOLE_TOLERANCE_M:
        report.warnings.append(
            f"bind sole at {lo[1]:.3f} m is not near -h/2 ({-0.5 * height:.3f} m); "
            "the origin is not at the character's middle"
        )

    animations = list(base.doc.get("animations") or [])
    for m, channels, skipped in loaded:
        for ch in channels:
            if ch.node == hips and ch.path == "translation":
                _shift_translation(ch, delta_local)
        seam = loop_seam_deg(channels)
        crossfaded = False
        if m.looping and seam > SEAM_CROSSFADE_DEG:
            _crossfade(channels, CROSSFADE_S)
            crossfaded = True
        heights = toe_heights(base, channels)
        speed = measure_ground_speed(base, channels)
        samplers, out_channels = [], []
        for ch in channels:
            comps = 4 if ch.path == "rotation" else 3
            in_acc = base.append_float_accessor([(t,) for t in ch.times], "SCALAR")
            out_acc = base.append_float_accessor(
                [tuple(v[:comps]) for v in ch.values], "VEC4" if comps == 4 else "VEC3",
                with_bounds=False,
            )
            samplers.append({"input": in_acc, "output": out_acc, "interpolation": ch.interp})
            out_channels.append({"sampler": len(samplers) - 1,
                                 "target": {"node": ch.node, "path": ch.path}})
        animations.append({"name": m.clip_name, "samplers": samplers, "channels": out_channels})

        frame0 = heights[0] if heights else None
        clip = ClipReport(
            key=m.key, clip_name=m.clip_name, channels=len(channels),
            skipped_channels=skipped[:20], duration_s=max((c.times[-1] for c in channels), default=0.0),
            seam_deg=round(seam, 2), crossfaded=crossfaded, frame0_toe_y_m=frame0,
            min_toe_y_m=min(heights) if heights else None, ground_speed_mps=speed,
        )
        report.clips.append(clip)
        if bind_toe is not None and clip.min_toe_y_m is not None:
            if abs(clip.min_toe_y_m - bind_toe) > FEET_TOLERANCE_M:
                report.ok = False
                report.warnings.append(
                    f"{m.clip_name}: lowest toe {clip.min_toe_y_m:.3f} m vs bind {bind_toe:.3f} m "
                    f"(more than {FEET_TOLERANCE_M * 100:.0f} cm apart)"
                )

    base.doc["animations"] = animations
    base.save(out_path)
    return report


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="Merge Uthana motions into a rigged GLB")
    parser.add_argument("base", type=Path)
    parser.add_argument("out", type=Path)
    parser.add_argument("motions", nargs="+", help="key=path.glb (clip named after the file)")
    args = parser.parse_args(argv)
    inputs = []
    for spec in args.motions:
        key, _, path = spec.partition("=")
        p = Path(path)
        inputs.append(MotionInput(key=key, clip_name=p.stem, path=p))
    try:
        rep = merge_character(args.base, inputs, args.out)
    except (GltfError, OSError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    print(f"wrote {rep.out_path}; hips offset {rep.hips_offset_m:.3f} m "
          f"(local {[round(v, 3) for v in rep.hips_offset_local]})")
    for c in rep.clips:
        print(f"  {c.key:>14} {c.clip_name}: frame0 toe {c.frame0_toe_y_m}, "
              f"speed {c.ground_speed_mps}, seam {c.seam_deg} deg"
              + (" (crossfaded)" if c.crossfaded else ""))
    for w in rep.warnings:
        print(f"warning: {w}")
    return 0 if rep.ok else 2


if __name__ == "__main__":
    raise SystemExit(main())
