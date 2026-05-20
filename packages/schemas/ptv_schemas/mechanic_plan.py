"""MechanicPlan: per-mechanic classification and (optionally) a CodeTask."""

from __future__ import annotations

from enum import Enum
from typing import Dict, List, Optional

from pydantic import Field

from .code_task import CodeTask
from .common import IdStr, StrictModel


class MechanicClassification(str, Enum):
    builtin = "builtin"
    builtin_config = "builtin+config"
    generate = "generate"


# Canonical ids of built-in reusable mechanics. Keep this list in sync with
# engine/runtime/src/mechanics/MechanicRegistry.cpp.
BUILTIN_MECHANIC_IDS = {
    "builtin.teleport",
    "builtin.smooth_locomotion",
    "builtin.snap_turn",
    "builtin.grab",
    "builtin.throw",
    "builtin.use_interact",
    "builtin.push_pull",
    "builtin.trigger_zone",
    "builtin.spawner",
    "builtin.inventory",
    "builtin.dialogue_trigger",
    "builtin.health_damage",
    "builtin.enemy_ai_hook",
    "builtin.projectile_hook",
    "builtin.hud_anchor",
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
    config: Dict[str, object] = Field(
        default_factory=dict,
        description="Free-form config passed to the built-in mechanic.",
    )
    code_task: Optional[CodeTask] = Field(
        default=None,
        description="Present only when classification == 'generate'.",
    )


class MechanicPlan(StrictModel):
    prompt_id: IdStr
    # No upper bound on items: a full VR game can map to dozens of
    # mechanics (locomotion variants, combat, abilities, UI, save /
    # load, NPC interaction, dialogue hooks, ...). The planner is
    # responsible for keeping each entry meaningful.
    items: List[MechanicPlanItem] = Field(..., min_length=1)
