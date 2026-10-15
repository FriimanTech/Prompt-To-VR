"""The canonical name of a Uthana locomotion motion.

Format: ``style-str{N}-ms{speed}-deg{angle}``, e.g. ``zombie-str5-ms1.4-deg0``.

The name is how a motion is found again anywhere in the Uthana organisation:
the Motion Verification step looks for an exact name match before anything
is generated, so the formatting must be deterministic. Speed is always one
decimal place (rounded half-up), the angle an integer when it is whole, the
style lower-case.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from decimal import ROUND_HALF_UP, Decimal

_NAME_RE = re.compile(
    r"^(?P<style>[A-Za-z0-9_]+)-str(?P<strides>[1-5])-ms(?P<ms>\d+(?:\.\d+)?)"
    r"-deg(?P<deg>-?\d+(?:\.\d+)?)$"
)

#: What :func:`format_locomotion_name` produces, for validation.
CANONICAL_RE = re.compile(r"^[a-z0-9_]+-str[1-5]-ms\d+\.\d-deg-?\d+(\.\d+)?$")


@dataclass(frozen=True)
class LocomotionParams:
    style: str
    strides: int
    move_speed: float
    travel_angle: float

    def as_api_kwargs(self, character_id: str) -> dict:
        """Map name fields to Generate Locomotion parameters."""
        return {
            "character_id": character_id,
            "style_id": self.style,
            "strides": self.strides,
            "move_speed": self.move_speed,
            "travel_angle": self.travel_angle,
        }


def round_speed(speed: float) -> float:
    return float(Decimal(str(speed)).quantize(Decimal("0.1"), rounding=ROUND_HALF_UP))


def _fmt_angle(angle: float) -> str:
    a = float(angle)
    return str(int(a)) if a.is_integer() else f"{a:g}"


def format_locomotion_name(style: str, strides: int, move_speed: float, travel_angle: float = 0.0) -> str:
    return (
        f"{style.strip().lower()}-str{int(strides)}-ms{round_speed(move_speed):.1f}"
        f"-deg{_fmt_angle(travel_angle)}"
    )


def parse_locomotion_name(name: str) -> LocomotionParams:
    """``zombie-str3-ms1.4-deg0`` -> style/strides/move_speed/travel_angle.

    Raises ``ValueError`` for anything not in the format.
    """
    m = _NAME_RE.match(name.strip())
    if not m:
        raise ValueError(
            f"{name!r} is not a locomotion motion name "
            "(expected style-str{N}-ms{speed}-deg{angle})"
        )
    return LocomotionParams(
        style=m.group("style").lower(),
        strides=int(m.group("strides")),
        move_speed=float(m.group("ms")),
        travel_angle=float(m.group("deg")),
    )


def is_locomotion_name(name: str) -> bool:
    return bool(_NAME_RE.match(name.strip()))


# Uthana's own default name for a fresh locomotion motion, e.g.
# ``neutral_non_gendered-4.1-5steps-0deg`` (style-speed-Nsteps-angle deg).
_DEFAULT_RE = re.compile(
    r"^(?P<style>[A-Za-z0-9_]+)-(?P<ms>\d+(?:\.\d+)?)-(?P<strides>\d+)steps"
    r"-(?P<deg>-?\d+(?:\.\d+)?)deg$"
)


def parse_uthana_default_name(name: str) -> LocomotionParams | None:
    """Parameters encoded in a motion's Uthana-assigned default name, if any.

    A motion created but never renamed (an interrupted run, another tool)
    still describes exactly what it is; reusing it avoids paying for the same
    generation twice.
    """
    m = _DEFAULT_RE.match((name or "").strip())
    if not m:
        return None
    return LocomotionParams(
        style=m.group("style").lower(),
        strides=int(m.group("strides")),
        move_speed=float(m.group("ms")),
        travel_angle=float(m.group("deg")),
    )


def same_params(a: LocomotionParams, b: LocomotionParams) -> bool:
    return (
        a.style == b.style
        and a.strides == b.strides
        and abs(round_speed(a.move_speed) - round_speed(b.move_speed)) < 1e-9
        and abs(a.travel_angle - b.travel_angle) < 1e-6
    )
