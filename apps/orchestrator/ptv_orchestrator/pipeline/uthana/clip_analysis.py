"""Measurements on an animation clip, evaluated through a character's rig.

* :func:`toe_heights` -- lowest toe/foot joint world height per frame;
* :func:`measure_ground_speed` -- the ground speed an IN-PLACE locomotion clip
  looks like it travels at. With the root's horizontal motion removed, a
  planted foot slides backwards at exactly the speed the body would cover, so
  the median horizontal speed of whichever foot is lowest is the clip's
  apparent speed;
* :func:`loop_seam_deg` -- the largest rotation jump between the last and the
  first frame.

Channels must already point at nodes of ``glb`` (the merge re-points them).
"""

from __future__ import annotations

import math
import statistics
from typing import Optional, Sequence

from .gltf_io import (
    Channel,
    Glb,
    animation_duration,
    pose_world,
    quat_angle_deg,
    read_animation,
)

FPS = 30.0
_FOOT_KEYS = ("toebase", "toe", "foot")


def _strip(name: str) -> str:
    return name.rsplit(":", 1)[-1].lower()


def foot_joints(glb: Glb) -> list[int]:
    """One contact joint per foot: the toe joint when present, else the foot."""
    joints = glb.skin_joint_set() or set(range(len(glb.nodes)))
    names = {i: _strip(str(glb.nodes[i].get("name") or "")) for i in joints}
    out: list[int] = []
    for side in ("left", "right"):
        for key in _FOOT_KEYS:
            hits = [i for i, n in names.items() if n.startswith(side) and key in n
                    and not n.endswith("_end")]
            if hits:
                out.append(sorted(hits)[0])
                break
    return out


def frame_times(channels: Sequence[Channel], fps: float = FPS) -> list[float]:
    duration = animation_duration(channels)
    n = max(1, int(round(duration * fps)))
    return [min(duration, k / fps) for k in range(n + 1)]


def toe_heights(glb: Glb, channels: Sequence[Channel], fps: float = FPS) -> list[float]:
    feet = foot_joints(glb)
    if not feet:
        return []
    out = []
    for t in frame_times(channels, fps):
        world = pose_world(glb, channels, t)
        out.append(min(world[j][1][3] for j in feet))
    return out


def measure_ground_speed(glb: Glb, channels: Sequence[Channel], fps: float = FPS) -> Optional[float]:
    """Apparent ground speed of an in-place locomotion clip, in GLB metres/s."""
    feet = foot_joints(glb)
    if len(feet) < 2:
        return None
    times = frame_times(channels, fps)
    if len(times) < 3:
        return None
    positions = []
    for t in times:
        world = pose_world(glb, channels, t)
        positions.append([(world[j][0][3], world[j][1][3], world[j][2][3]) for j in feet])
    all_y = [p[1] for frame in positions for p in frame]
    floor = min(all_y)
    stance_band = 0.02 + 0.1 * (max(all_y) - floor)
    speeds = []
    for k in range(1, len(positions)):
        dt = times[k] - times[k - 1]
        if dt <= 0:
            continue
        for f in range(len(feet)):
            a, b = positions[k - 1][f], positions[k][f]
            if max(a[1], b[1]) - floor > stance_band:
                continue
            speeds.append(math.hypot(b[0] - a[0], b[2] - a[2]) / dt)
    if not speeds:
        return None
    return statistics.median(speeds)


def measure_root_speed(glb: Glb) -> Optional[float]:
    """Average ground speed of a ROOT-MOTION clip: hips travel / duration.

    The ground truth for how fast a locomotion clip moves. It is measured on
    a second, root-motion download of the same motion (downloads are free);
    the in-place clip the game plays has that travel removed.
    """
    if not glb.doc.get("animations"):
        return None
    channels = read_animation(glb, 0)
    duration = animation_duration(channels)
    if duration <= 1e-3:
        return None
    # A no-mesh download has no skin, so find the root joint by name.
    names = glb.node_index_by_name()
    hips = next((i for n, i in names.items() if n.rsplit(":", 1)[-1].lower() == "hips"), -1)
    if hips < 0:
        return None
    w0 = pose_world(glb, channels, 0.0)[hips]
    w1 = pose_world(glb, channels, duration)[hips]
    return math.hypot(w1[0][3] - w0[0][3], w1[2][3] - w0[2][3]) / duration


def loop_seam_deg(channels: Sequence[Channel]) -> float:
    worst = 0.0
    for ch in channels:
        if ch.path != "rotation" or len(ch.times) < 2:
            continue
        first = ch.values[1] if ch.interp == "CUBICSPLINE" else ch.values[0]
        last = ch.values[-2] if ch.interp == "CUBICSPLINE" else ch.values[-1]
        worst = max(worst, quat_angle_deg(first, last))
    return worst
