"""Deterministic controller-binding validation for the MechanicPlan.

The MechanicPlan agent assigns a :class:`ControllerBinding` to every
input-driven mechanic (from the prompt when it names one, otherwise a
VR-best-practice default). This module checks those bindings BEFORE scene
compile:

* Face-button hand correctness -- A/B live on the RIGHT Touch controller,
  X/Y on the LEFT. A wrong hand is auto-normalized (with a warning) rather
  than failing the run.
* Conflicts -- two DIFFERENT mechanics may not claim the same physical
  control on the same hand. This is a hard error (the stage raises).
* Engine-reserved controls -- the runtime already owns the left thumbstick
  (locomotion), the right thumbstick (snap turn), and both grips (grab /
  throw) in ``Application.cpp``. A plan mechanic that reuses one of those
  gets a warning unless it IS that locomotion/grab mechanic.

The validator is pure/deterministic so it can run without any network or
model call, and is unit-testable in isolation.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import List

from ptv_schemas import (
    ControllerControl,
    ControllerHand,
    MechanicPlan,
)

_RIGHT_ONLY = {ControllerControl.a, ControllerControl.b}
_LEFT_ONLY = {ControllerControl.x, ControllerControl.y}
_FACE_BUTTONS = _RIGHT_ONLY | _LEFT_ONLY

# Physical controls the engine core (Application.cpp) already consumes. A
# generated mechanic that rebinds one of these fights the built-in system.
# Keyed by (control, hand-as-string).
_ENGINE_RESERVED = {
    (ControllerControl.thumbstick, "left"): "locomotion (slide / teleport aim)",
    (ControllerControl.thumbstick, "right"): "snap turn",
    (ControllerControl.grip, "left"): "grab / throw (left hand)",
    (ControllerControl.grip, "right"): "grab / throw (right hand)",
}

# Built-in ids (or plan item ids) that are ALLOWED to use the reserved
# controls because they ARE that engine system.
_RESERVED_OK_HINTS = ("locomotion", "grab", "throw", "snap_turn", "teleport")


@dataclass
class BindingValidationResult:
    errors: List[str] = field(default_factory=list)
    warnings: List[str] = field(default_factory=list)

    @property
    def ok(self) -> bool:
        return not self.errors


def _slots_for(control: ControllerControl, hand: ControllerHand) -> list[str]:
    """Return the normalized occupancy slot key(s) for a (control, hand).

    Face buttons are per-controller singletons (hand implied). ``either``
    on an analog control occupies BOTH hands so it conflicts with a
    same-control binding on either specific hand.
    """
    if control in _RIGHT_ONLY:
        return [f"{control.value}:right"]
    if control in _LEFT_ONLY:
        return [f"{control.value}:left"]
    if hand == ControllerHand.either:
        return [f"{control.value}:left", f"{control.value}:right"]
    return [f"{control.value}:{hand.value}"]


def _looks_reserved_owner(item_id: str, builtin_id: str | None) -> bool:
    hay = f"{item_id} {builtin_id or ''}".lower()
    return any(h in hay for h in _RESERVED_OK_HINTS)


def validate_bindings(plan: MechanicPlan) -> BindingValidationResult:
    """Validate + normalize all controller bindings in ``plan`` in place.

    Face-button hands are normalized to the correct controller. Returns a
    :class:`BindingValidationResult`; the caller raises on ``errors`` and
    logs ``warnings``.
    """
    res = BindingValidationResult()
    used: dict[str, str] = {}  # slot -> owning mechanic id

    for item in plan.items:
        b = item.binding
        if b is None or b.control == ControllerControl.none:
            continue

        # Normalize face-button hands to the physically correct controller.
        if b.control in _RIGHT_ONLY and b.hand != ControllerHand.right:
            res.warnings.append(
                f"{item.id}: '{b.control.value}' is right-controller only; "
                f"normalized hand '{b.hand.value}' -> 'right'"
            )
            b.hand = ControllerHand.right
        elif b.control in _LEFT_ONLY and b.hand != ControllerHand.left:
            res.warnings.append(
                f"{item.id}: '{b.control.value}' is left-controller only; "
                f"normalized hand '{b.hand.value}' -> 'left'"
            )
            b.hand = ControllerHand.left

        # Conflict detection across plan mechanics.
        for slot in _slots_for(b.control, b.hand):
            prev = used.get(slot)
            if prev is not None and prev != item.id:
                res.errors.append(
                    f"binding conflict: mechanics '{prev}' and '{item.id}' both "
                    f"bind {slot.replace(':', ' on ')} controller"
                )
            else:
                used[slot] = item.id

        # Engine-reserved control reuse (soft warning unless the mechanic IS
        # the owning locomotion/grab system).
        reserved_key = (b.control, b.hand.value)
        if reserved_key in _ENGINE_RESERVED and not _looks_reserved_owner(
            item.id, item.builtin_id or item.base_builtin_id
        ):
            res.warnings.append(
                f"{item.id}: binds {b.control.value} on {b.hand.value} which the "
                f"engine already uses for {_ENGINE_RESERVED[reserved_key]}; "
                f"confirm this is intended"
            )

    return res
