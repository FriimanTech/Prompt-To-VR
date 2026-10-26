"""Measure a character's MODEL FRAME: where its soles are, how tall it is and
which way it faces, in the GLB's own bind pose.

Every character source disagrees about all three. Tripo puts the origin at
the feet (bind y from 0 to h) and faces +X; Uthana centres the hips (y from
-h/2 to +h/2) and faces +Z, the glTF convention. The engine faces -Z and
stands a character on its soles. Guessing any of these per provider is what
left Tripo NPCs floating by half their height and walking sideways, so they
are measured here, once, from the skeleton and the skinned bind pose, and the
scene compiler hands the runtime a single pivot that puts every character in
the same frame.

Units are the GLB's own (metres for both providers in practice). The scene
compiler converts with the same scale the runtime uses.
"""

from __future__ import annotations

import math
import re
from pathlib import Path
from typing import Any, Optional

from .uthana.gltf_io import Glb, GltfError, aabb, raw_position_bounds, skinned_bind_positions

# Forward axes a manifest may declare for an asset that has no skeleton to
# measure. Model space, y up.
FORWARD_HINTS: dict[str, tuple[float, float]] = {
    "+x": (1.0, 0.0),
    "-x": (-1.0, 0.0),
    "+z": (0.0, 1.0),
    "-z": (0.0, -1.0),
}

# glTF's convention, and what Tripo image-to-model emits for an unrigged mesh
# as far as anything documents it. Used only with a loud warning.
DEFAULT_UNRIGGED_FORWARD = "+z"

_SNAP_DEG = 12.0
_MAX_SAMPLED_VERTICES = 20000


class ModelFrameError(RuntimeError):
    """A rigged character whose feet cannot be found."""


def _norm(name: str) -> str:
    name = name.rsplit(":", 1)[-1]
    return re.sub(r"[\s\-\.]", "_", name).lower()


def _side(name: str) -> Optional[str]:
    n = _norm(name)
    if n.startswith(("l_", "left")) or n.endswith(("_l", "left")):
        return "l"
    if n.startswith(("r_", "right")) or n.endswith(("_r", "right")):
        return "r"
    return None


def _bare(name: str) -> str:
    """Name with its side marker removed: ``L_Upperarm`` -> ``upperarm``."""
    n = _norm(name)
    for pre in ("left_", "right_", "left", "right", "l_", "r_"):
        if n.startswith(pre):
            n = n[len(pre):]
            break
    for suf in ("_left", "_right", "_l", "_r"):
        if n.endswith(suf):
            n = n[: -len(suf)]
            break
    return n


def _classify(name: str) -> Optional[str]:
    b = _bare(name)
    if "twist" in b or "roll" in b:
        return None
    if b in ("upperarm", "upper_arm", "arm"):
        return "upper_arm"
    if b in ("shoulder", "clavicle"):
        return "shoulder"
    if b in ("hand", "wrist"):
        return "hand"
    if "toe" in b:
        return "toe"
    if b in ("foot", "ankle"):
        return "foot"
    return None


def _snap(fx: float, fz: float) -> tuple[float, float, bool]:
    length = math.hypot(fx, fz)
    if length < 1e-9:
        return 0.0, 1.0, False
    fx, fz = fx / length, fz / length
    for ax, az in FORWARD_HINTS.values():
        if fx * ax + fz * az >= math.cos(math.radians(_SNAP_DEG)):
            return ax, az, True
    return fx, fz, False


def yaw_offset_for_forward(fx: float, fz: float) -> float:
    """Rotation about +Y (radians) that turns model forward ``(fx, 0, fz)``
    onto the engine's -Z. Same convention as ``ptv::yaw_from_forward``:
    yaw(f) = atan2(-f.x, -f.z), and the correction is its negation."""
    return -math.atan2(-fx, -fz)


def measure(glb_path: Path, *, rigged: bool, forward_hint: Optional[str] = None) -> dict[str, Any]:
    """The frame record written into ``asset_contract.json`` under ``frame``.

    Raises :class:`ModelFrameError` for a rigged character with no
    measurable feet -- that asset would otherwise be stood at a guess.
    """
    glb = Glb.load(glb_path)
    warnings: list[str] = []

    points = skinned_bind_positions(glb, max_vertices=_MAX_SAMPLED_VERTICES)
    lo, hi = aabb(points)
    raw_lo, raw_hi = raw_position_bounds(glb)
    raw_longest = max(raw_hi[i] - raw_lo[i] for i in range(3))
    sole_y = lo[1]
    height = hi[1] - lo[1]
    if height <= 1e-6:
        raise ModelFrameError(f"{glb_path.name}: degenerate height {height}")

    joints: dict[str, dict[str, tuple[float, float, float]]] = {}
    has_skin = bool(glb.doc.get("skins"))
    if has_skin:
        world = glb.world_matrices()
        for j in glb.skin_joint_set():
            name = glb.nodes[j].get("name") or ""
            kind = _classify(name)
            side = _side(name)
            if kind is None or side is None:
                continue
            m = world[j]
            pos = (m[0][3], m[1][3], m[2][3])
            slot = joints.setdefault(kind, {})
            # Keep the first found per side (the joint, not its end sites).
            slot.setdefault(side, pos)

    def pair(kind: str) -> Optional[tuple[tuple[float, ...], tuple[float, ...]]]:
        s = joints.get(kind) or {}
        if "l" in s and "r" in s:
            return s["l"], s["r"]
        return None

    feet = [p for k in ("foot", "toe") for p in (joints.get(k) or {}).values()]
    feet_y = min((p[1] for p in feet), default=None)

    method = "hint"
    fx, fz = 0.0, 0.0
    shoulders = pair("upper_arm") or pair("shoulder")
    if shoulders is not None:
        (lx, _, lz), (rx, _, rz) = shoulders
        right_x, right_z = rx - lx, rz - lz
        # forward = cross(up, right)
        fx, fz = right_z, -right_x
        method = "shoulders"

    toe_dir: Optional[tuple[float, float]] = None
    foot_s = joints.get("foot") or {}
    toe_s = joints.get("toe") or {}
    common = [s for s in ("l", "r") if s in foot_s and s in toe_s]
    if common:
        tx = sum(toe_s[s][0] - foot_s[s][0] for s in common) / len(common)
        tz = sum(toe_s[s][2] - foot_s[s][2] for s in common) / len(common)
        if math.hypot(tx, tz) > 0.02 * height:
            toe_dir = (tx, tz)

    if toe_dir is not None:
        if method == "shoulders":
            dot = (fx * toe_dir[0] + fz * toe_dir[1]) / (
                (math.hypot(fx, fz) or 1.0) * math.hypot(*toe_dir)
            )
            if dot < -0.3:
                # Toes cannot be mirrored; side NAMES can (some rigs name
                # joints by the viewer's left).
                warnings.append(
                    "shoulder line and toes disagree on forward; trusting toes"
                    " (the rig's left/right names are probably mirrored)"
                )
                fx, fz = toe_dir
                method = "toes"
            else:
                method = "shoulders+toes"
        else:
            fx, fz = toe_dir
            method = "toes"

    if method == "hint":
        key = (forward_hint or "").lower()
        if key in FORWARD_HINTS:
            fx, fz = FORWARD_HINTS[key]
        else:
            fx, fz = FORWARD_HINTS[DEFAULT_UNRIGGED_FORWARD]
            method = "default"
            warnings.append(
                "no skeleton to measure forward from and no forward_hint in the"
                f" manifest; assuming {DEFAULT_UNRIGGED_FORWARD} (glTF convention)"
            )

    fx, fz, snapped = _snap(fx, fz)

    is_biped = shoulders is not None and (pair("foot") or pair("toe")) is not None
    if rigged and has_skin and feet_y is None:
        if shoulders is not None:
            raise ModelFrameError(
                f"{glb_path.name}: rigged character has arm joints but no foot or"
                " toe joint, so its soles cannot be located"
            )
        warnings.append("no foot joints (non-biped rig); sole taken from the mesh minimum")
    elif feet_y is not None and feet_y - sole_y > 0.25 * height:
        warnings.append(
            f"lowest foot joint is {feet_y - sole_y:.3f} above the mesh bottom"
            f" ({(feet_y - sole_y) / height:.0%} of height); something hangs below"
            " the feet"
        )

    t_posed = False
    hands = pair("hand")
    arms = pair("upper_arm") or pair("shoulder")
    if hands and arms:
        span = math.hypot(hands[0][0] - hands[1][0], hands[0][2] - hands[1][2])
        level = max(abs(hands[i][1] - arms[i][1]) for i in (0, 1))
        t_posed = span > 0.7 * height and level < 0.15 * height

    return {
        "status": "measured" if method not in ("hint", "default") else method,
        "units": "glb",
        "method": method,
        "forward": [round(fx, 5), 0.0, round(fz, 5)],
        "forward_snapped": snapped,
        "yaw_offset_rad": round(yaw_offset_for_forward(fx, fz), 6),
        "sole_y": round(sole_y, 6),
        "top_y": round(hi[1], 6),
        "height": round(height, 6),
        "center_xz": [round(0.5 * (lo[0] + hi[0]), 6), round(0.5 * (lo[2] + hi[2]), 6)],
        "lowest_foot_joint_y": None if feet_y is None else round(feet_y, 6),
        "raw_longest_axis": round(raw_longest, 6),
        "biped": is_biped,
        "t_posed": t_posed,
        "warnings": warnings,
        "source_glb": str(glb_path).replace("\\", "/"),
    }


def try_measure(glb_path: Path, *, rigged: bool, forward_hint: Optional[str] = None) -> dict[str, Any]:
    """:func:`measure`, with failures captured as ``status: failed``."""
    try:
        return measure(glb_path, rigged=rigged, forward_hint=forward_hint)
    except (ModelFrameError, GltfError, KeyError, ValueError) as exc:
        return {"status": "failed", "reason": str(exc)[:400], "source_glb": str(glb_path).replace("\\", "/")}
