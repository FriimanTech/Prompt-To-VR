"""Render-and-verify: draw the solved sockets on the mesh and ask whether
they are right.

A proxy fist (an orange band around the gripped part, a cyan arrow from the
little finger to the thumb, a magenta arrow toward the fingers' side), a red
arrow out of each muzzle, blue / green bars for shield straps and yellow
spheres for blade ends and bow points are composited into the same ten views
in depth, and the vision model answers per socket: does this look like how a
person holds / uses this object? The answer is a gate, not a nudge: a socket
that fails is re-segmented once with the critique, then falls back.
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Callable, Optional, Sequence

import numpy as np

from .solvers import SolvedSocket

ORANGE = [1.0, 0.55, 0.05]
RED = [1.0, 0.08, 0.08]
BLUE = [0.15, 0.35, 1.0]
GREEN = [0.1, 0.85, 0.25]
YELLOW = [1.0, 0.9, 0.1]

_HAND = {"hand_grip", "support_grip", "support_wrap", "hand_strap"}


def _q(rot: np.ndarray) -> list[float]:
    from .solvers import mat_to_quat  # noqa: PLC0415

    return mat_to_quat(rot)


def markers_for(sockets: dict[str, SolvedSocket], metres_per_unit: float,
                item_length_m: float = 0.3) -> list[dict]:
    m = 1.0 / max(metres_per_unit, 1e-9)
    # Point markers grow with the item, so they stay visible on a 1.5 m bow.
    # ...and shrink with it, so a 1.5 cm bullet is not hidden by its marker.
    dot_m = min(max(0.012, 0.018 * item_length_m), 0.08 * item_length_m)
    out: list[dict] = []
    for s in sockets.values():
        p = [float(v) for v in s.position]
        if s.kind == "support_wrap":
            # Drawn where the runtime puts it for a right-handed shooter: the
            # support (left) hand beside the shooting fist, toward the gun's
            # left, slightly larger so it reads as wrapping over the first.
            lat = float(s.extras.get("lateral_offset", 0.04 * m))
            pos = (s.position - s.rotation[:, 0] * lat).tolist()
            r = max(0.75 * float(sockets.get(s.extras.get("wrap_of", ""), s)
                                 .extras.get("handle_thickness", 0.03 * m)), 0.022 * m)
            out.append({"type": "fist", "position": pos, "rotation": _q(s.rotation),
                        "radius": r * 1.1, "width": 0.08 * m, "color": [1.0, 0.8, 0.3]})
        elif s.kind in _HAND and s.kind != "hand_strap":
            r = max(0.75 * float(s.extras.get("handle_thickness", 0.03 * m)), 0.022 * m)
            out.append({"type": "fist", "position": p, "rotation": _q(s.rotation),
                        "radius": r, "width": 0.085 * m,
                        "color": ORANGE if s.kind == "hand_grip" else [1.0, 0.8, 0.3]})
        elif s.kind == "hand_strap":
            z = s.rotation[:, 2]
            a = (s.position + z * 0.05 * m).tolist()
            b = (s.position - z * 0.05 * m).tolist()
            out.append({"type": "bar", "a": a, "b": b, "radius": 0.01 * m, "color": BLUE})
        elif s.kind == "forearm_strap":
            x = s.rotation[:, 0]
            a = (s.position + x * 0.06 * m).tolist()
            b = (s.position - x * 0.06 * m).tolist()
            out.append({"type": "bar", "a": a, "b": b, "radius": 0.012 * m, "color": GREEN})
        elif s.kind == "muzzle":
            length = min(0.12, 0.6 * item_length_m)
            out.append({"type": "arrow", "position": p,
                        "direction": [float(v) for v in s.rotation[:, 2]],
                        "length": length * m, "radius": length * 0.033 * m, "color": RED})
        else:
            out.append({"type": "sphere", "position": p, "radius": dot_m * m,
                        "color": YELLOW})
            out.append({"type": "arrow", "position": p,
                        "direction": [float(v) for v in s.rotation[:, 2]],
                        "length": 5.0 * dot_m * m, "radius": 0.25 * dot_m * m,
                        "color": YELLOW})
    return out


def write_markers(path: Path, markers: list[dict]) -> Path:
    path.write_text(json.dumps({"markers": markers}, indent=1), encoding="utf-8")
    return path


LEGEND = (
    "Markers drawn on the model: an ORANGE band is a virtual fist closed around the "
    "part it holds (a pale orange band is the second hand); the CYAN arrow on it "
    "points from the little finger toward the thumb; the MAGENTA arrow points toward "
    "the side the fingers wrap to (the trigger side of a gun's handle; for a second "
    "hand cupping a handguard from below it points SIDEWAYS, around the handguard, "
    "which is correct). A RED arrow "
    "starts at a muzzle and points where shots leave. A BLUE bar is the shield strap "
    "the hand grips, a GREEN bar the strap the forearm passes through. YELLOW spheres "
    "mark blade ends and bow points, with short arrows for their direction."
)


def verify_schema(names: Sequence[str]) -> dict:
    return {
        "type": "object",
        "properties": {
            "checks": {
                "type": "array",
                "items": {
                    "type": "object",
                    "properties": {
                        "socket": {"type": "string", "enum": list(names)},
                        "ok": {"type": "boolean"},
                        "problem": {"type": "string"},
                    },
                    "required": ["socket", "ok", "problem"],
                },
            }
        },
        "required": ["checks"],
    }


def build_verify_prompt(asset_name: str, questions: dict[str, str]) -> str:
    lines = [f"These images show a 3D model of: {asset_name}, from several angles.",
             LEGEND, "Answer for each marker, judging from all images together:"]
    for name, q in questions.items():
        lines.append(f"- {name}: {q}")
    lines.append("ok = true only if a person really would hold / use the object that way. "
                 "If not, describe the problem in a short sentence naming the part the "
                 "marker should be on instead; otherwise problem = \"\".")
    return "\n".join(lines)


def questions_for(sockets: dict[str, SolvedSocket], usage_desc: dict[str, str]) -> dict[str, str]:
    q: dict[str, str] = {}
    for name, s in sockets.items():
        what = usage_desc.get(name, name.replace("_", " "))
        if s.kind == "hand_grip":
            q[name] = (f"is the orange fist closed around {what}, at the height a palm "
                       "would hold it, with the cyan arrow pointing toward the thumb end?")
        elif s.kind == "support_wrap":
            q[name] = ("is the pale orange fist beside the orange fist on the handle, on "
                       "the other side of it, where a second hand would wrap around the "
                       "shooting hand in a two-handed grip? (It is SUPPOSED to touch and "
                       "overlap the orange fist.)")
        elif s.kind == "support_grip":
            q[name] = f"is the pale orange fist where the second hand holds it ({what})?"
        elif s.kind == "muzzle":
            q[name] = "does the red arrow start at the barrel's opening and point out of it?"
        elif s.kind == "hand_strap":
            q[name] = "is the blue bar on the strap / handle the hand grips, on the BACK?"
        elif s.kind == "forearm_strap":
            q[name] = "is the green bar where the forearm strap is, on the BACK?"
        else:
            q[name] = f"is the yellow marker on {what}?"
    return q


VerifyFn = Callable[[str, list[bytes], dict], dict]


def verify(asset_name: str, sockets: dict[str, SolvedSocket], overlay_images: list[Path],
           usage_desc: dict[str, str], vision_multi: VerifyFn,
           max_images: int = 6) -> dict[str, tuple[bool, str]]:
    """Returns ``{socket: (ok, problem)}``. Sockets the model did not answer
    for are reported ok=False with problem 'not assessed' only if every
    answer is missing; partial answers keep the unanswered ones ok."""

    names = list(sockets)
    if not names:
        return {}
    imgs = [p.read_bytes() for p in overlay_images[:max_images]]
    prompt = build_verify_prompt(asset_name, questions_for(sockets, usage_desc))
    resp = vision_multi(prompt, imgs, verify_schema(names))
    out: dict[str, tuple[bool, str]] = {}
    for c in resp.get("checks") or []:
        n = c.get("socket")
        if n in sockets:
            out[n] = (bool(c.get("ok")), str(c.get("problem") or ""))
    if not out:
        return {n: (False, "not assessed") for n in names}
    for n in names:
        out.setdefault(n, (True, ""))
    return out


def pick_overlay_views(n_views: int) -> list[int]:
    # Two side views, one top, two three-quarter views and one end.
    order = [0, 1, 6, 8, 2, 4, 7, 9, 3, 5]
    return [i for i in order if i < n_views]


def overlay_paths(out_dir: Path, indices: Optional[Sequence[int]] = None) -> list[Path]:
    idx = indices if indices is not None else pick_overlay_views(10)
    return [out_dir / f"overlay_{i}.png" for i in idx if (out_dir / f"overlay_{i}.png").exists()]
