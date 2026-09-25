"""MechanicPlan: per-mechanic classification and (optionally) a CodeTask."""

from __future__ import annotations

from enum import Enum
from typing import Dict, List, Optional

from pydantic import Field

from .code_task import CodeTask
from .common import IdStr, StrictModel


class MechanicClassification(str, Enum):
    #: A catalog built-in satisfies the request exactly as-is.
    builtin = "builtin"
    #: A catalog built-in satisfies the request after runtime PARAMETER
    #: tuning only (no code changes). Set ``builtin_id`` + ``config``.
    builtin_config = "builtin+config"
    #: A catalog built-in is the right FOUNDATION, but params alone cannot
    #: satisfy the prompt -- the built-in's source must be copied and edited
    #: into a NEW generated mechanic. Set ``base_builtin_id`` (the source to
    #: copy) AND a ``code_task``. The new mechanic never replaces the built-in.
    builtin_config_code_edit = "builtin+config_code_edit"
    #: A brand-new mechanic with no suitable built-in foundation. Set a
    #: ``code_task``.
    generate = "generate"


class ControllerControl(str, Enum):
    """Which physical control on the Meta Quest 3 Touch Plus controller
    drives an input-driven mechanic. ``none`` means the mechanic is not
    directly input-driven (e.g. a wave timer, health system)."""

    trigger = "trigger"
    grip = "grip"
    thumbstick = "thumbstick"
    a = "a"
    b = "b"
    x = "x"
    y = "y"
    none = "none"


class ControllerHand(str, Enum):
    left = "left"
    right = "right"
    either = "either"


class ControllerBinding(StrictModel):
    """The controller control that activates an input-driven mechanic.

    Set by the MechanicPlan agent (from the prompt when it names a binding,
    otherwise a VR-best-practice default), normalized deterministically, and
    materialized into the entity's ``mechanic_bindings.params`` at scene
    compile so the running mechanic reads ``control`` + ``hand`` from its
    params and checks the live ``InputSnapshot``. Multiple mechanics may
    share the same control on the same hand -- that is not a conflict.

    Layout constraints (enforced by the binding validator): A/B live on the
    RIGHT controller only; X/Y live on the LEFT controller only.
    """

    control: ControllerControl = ControllerControl.none
    hand: ControllerHand = ControllerHand.right
    # Optional human-readable rationale (e.g. "prompt specified right trigger",
    # "default: primary action -> right trigger").
    notes: str = Field(default="", max_length=400)


# Canonical ids of built-in reusable mechanics. Keep this list in sync with
# the PTV_REGISTER_MECHANIC calls under
# engine/runtime/src/mechanics/builtin/<mechanic>/.
BUILTIN_MECHANIC_IDS = {
    "builtin.teleport",
    "builtin.locomotion.teleport",
    "builtin.locomotion.slide.head_relative",
    "builtin.locomotion.slide.controller_relative",
    "builtin.snap_turn",
    "builtin.locomotion.snap_turn",
    "builtin.locomotion.smooth_turn",
    "builtin.movement_settings_menu",
    "builtin.grab",
    "builtin.throw",
    "builtin.recoil",
    "builtin.hand_pose",
    "builtin.character_locomotion",
    "builtin.avatar_body",
    "builtin.mirror_surface",
    "builtin.equip",
    "builtin.ui_panel",
    "builtin.head_interior",
    "builtin.door",
    "builtin.switchable",
    "builtin.container",
    "builtin.mover",
    "builtin.breakable",
    "builtin.harvestable",
}


class MechanicPlanItem(StrictModel):
    id: IdStr
    classification: MechanicClassification
    builtin_id: Optional[str] = Field(
        default=None,
        description=(
            "When classification is 'builtin' or 'builtin+config', the id of"
            " the reused built-in mechanic (e.g. 'builtin.grab')."
        ),
    )
    base_builtin_id: Optional[str] = Field(
        default=None,
        description=(
            "When classification is 'builtin+config_code_edit', the id of the"
            " built-in mechanic whose SOURCE should be copied and edited into"
            " the new generated mechanic (e.g. 'builtin.locomotion.teleport')."
            " The code-generation stage injects that source into the agent's"
            " context so it edits rather than rewrites from scratch."
        ),
    )
    config: Dict[str, object] = Field(
        default_factory=dict,
        description="Free-form config passed to the built-in mechanic.",
    )
    binding: Optional[ControllerBinding] = Field(
        default=None,
        description=(
            "Controller binding for input-driven mechanics. None (or"
            " control='none') for mechanics that are not directly driven by a"
            " controller control. Normalized before scene compile and"
            " materialized into scene.json params. Multiple mechanics may"
            " share the same control on the same hand."
        ),
    )
    code_task: Optional[CodeTask] = Field(
        default=None,
        description=(
            "Present when classification == 'generate' or"
            " 'builtin+config_code_edit' (the new C++ module to author)."
        ),
    )


class MechanicPlan(StrictModel):
    prompt_id: IdStr
    # No upper bound on items: a full VR game can map to dozens of
    # mechanics (locomotion variants, combat, abilities, UI, save /
    # load, NPC interaction, dialogue hooks, ...). The planner is
    # responsible for keeping each entry meaningful.
    items: List[MechanicPlanItem] = Field(..., min_length=1)
