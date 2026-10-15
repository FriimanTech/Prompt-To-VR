"""Every Uthana motion each Uthana-animated character will be given.

Written by the ``uthana_motion_plan`` stage BEFORE anything is generated, so
the plan can be inspected (and costed) before a single second of motion is
billed. ``uthana_animation`` then resolves each entry to a motion id, downloads
it retargeted onto the character, and merges the clips.

Locomotion names follow ``style-str{N}-ms{speed}-deg{angle}`` (for example
``zombie-str5-ms1.4-deg0``). The name is the identity of a motion across the
whole Uthana organisation: a motion with that exact name is reused instead of
being generated again.
"""

from __future__ import annotations

from typing import List, Optional

from pydantic import Field

from .character_routing import MOTION_KEY_PATTERN, MotionSource
from .common import IdStr, StrictModel


class UthanaMotionSpec(StrictModel):
    #: Animation key the clip is exposed under (``idle``, ``walk``, ``run``,
    #: ``run.punching``).
    key: str = Field(..., pattern=MOTION_KEY_PATTERN)
    kind: MotionSource
    #: Which locomotion tier the clip plays as. ``None`` for idle.
    gait: Optional[str] = Field(default=None, pattern=r"^(walk|run)$")
    style_id: Optional[str] = Field(default=None, max_length=64)
    strides: Optional[int] = Field(default=None, ge=1, le=5)
    move_speed: Optional[float] = Field(default=None, gt=0.0, le=8.0)
    travel_angle: float = Field(default=0.0, ge=-180.0, le=180.0)
    motion_name: str = Field(..., max_length=128)
    #: Fixed id for ``uthana_library`` motions.
    motion_id: Optional[str] = Field(default=None, max_length=64)
    purpose: str = Field(default="", max_length=400)


class UthanaCharacterMotionPlan(StrictModel):
    asset_id: IdStr
    character_id: str = Field(..., max_length=64)
    character_role: str = Field(default="npc", pattern=r"^(npc|player_avatar)$")
    #: World height of the Uthana-rigged GLB -- the size Uthana animates.
    height_for_motion_m: float = Field(..., gt=0.0)
    target_scale_meters: float = Field(..., gt=0.0)
    #: Engine scale: target_scale_meters / raw longest axis of the GLB.
    engine_scale: float = Field(..., gt=0.0)
    motions: List[UthanaMotionSpec] = Field(default_factory=list, max_length=12)
    notes: List[str] = Field(default_factory=list, max_length=24)


class UthanaMotionPlan(StrictModel):
    prompt_id: IdStr
    locomotion_styles: List[str] = Field(default_factory=list)
    characters: List[UthanaCharacterMotionPlan] = Field(default_factory=list, max_length=48)

    def for_asset(self, asset_id: str) -> Optional[UthanaCharacterMotionPlan]:
        for c in self.characters:
            if c.asset_id == asset_id:
                return c
        return None


# ---------------------------------------------------------------------------
# What the motion-plan agent returns
# ---------------------------------------------------------------------------


class PlannedLocomotion(StrictModel):
    key: str = Field(..., pattern=MOTION_KEY_PATTERN)
    gait: str = Field(..., pattern=r"^(walk|run)$")
    style_id: str = Field(..., max_length=64)
    strides: int = Field(default=5, ge=1, le=5)
    move_speed: float = Field(..., gt=0.0)
    travel_angle: float = 0.0
    motion_name: str = Field(..., max_length=128)
    purpose: str = Field(default="", max_length=400)


class PlannedCharacterMotions(StrictModel):
    asset_id: IdStr
    motions: List[PlannedLocomotion] = Field(default_factory=list, max_length=10)
    rationale: str = Field(default="", max_length=1500)


class UthanaMotionAgentOutput(StrictModel):
    characters: List[PlannedCharacterMotions] = Field(default_factory=list, max_length=48)
