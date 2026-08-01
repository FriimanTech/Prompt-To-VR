"""Deterministic controller-binding normalization for the MechanicPlan.

The MechanicPlan agent assigns a :class:`ControllerBinding` to every
input-driven mechanic (from the prompt when it names one, otherwise a
VR-best-practice default). This module normalizes + sanity-checks those
bindings BEFORE scene compile:

* Face-button hand correctness -- A/B live on the RIGHT Touch controller,
  X/Y on the LEFT. A wrong hand is auto-normalized (with a warning) rather
  than failing the run.
* Engine-reserved controls -- the built-in mechanics already own the left
  thumbstick (locomotion), the right thumbstick (snap turn), and both grips
  (grab / throw). A plan mechanic that reuses one of those gets a warning
  unless it IS that locomotion/grab mechanic.

Sharing a control is NOT a conflict: multiple mechanics may legitimately
bind the same control on the same hand (e.g. a UI pointer that clicks with
the trigger while that same hand also grabs / fires a weapon). The
validator therefore never rejects a plan on binding grounds; it only
normalizes hands and emits advisory warnings.

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

# Physical controls the built-in mechanics already consume. A generated
# mechanic that rebinds one of these fights the built-in system.
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


def _looks_reserved_owner(item_id: str, builtin_id: str | None) -> bool:
    hay = f"{item_id} {builtin_id or ''}".lower()
    return any(h in hay for h in _RESERVED_OK_HINTS)


def validate_bindings(plan: MechanicPlan) -> BindingValidationResult:
    """Normalize all controller bindings in ``plan`` in place.

    Face-button hands are normalized to the correct controller. Sharing a
    control across mechanics is allowed and is NOT reported as an error, so
    this never populates ``errors``; it only emits advisory ``warnings``.
    Returns a :class:`BindingValidationResult`.
    """
    res = BindingValidationResult()

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
