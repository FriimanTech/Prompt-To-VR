"""Which provider rigs and animates each character, and with which motions.

Tripo creates every character mesh. After that, humanoid characters can go
one of two ways:

* **Uthana** rigs bipedal humanoids WITH finger joints and generates
  locomotion in any of ~110 styles at a chosen speed. The player's avatar
  body always goes here; humanoid NPCs go here when Tripo cannot rig them or
  when Uthana's motions serve the NPC better.
* **Tripo** rigs whole creatures (biped, quadruped, hexapod, octopod, avian,
  serpentine, aquatic) and retargets fixed preset clips.

``character_routing.json`` records the decision per character so every later
stage (both providers' rigging and animation, codegen, scene compile) reads
one answer instead of re-deriving it.
"""

from __future__ import annotations

from enum import Enum
from typing import List, Optional

from pydantic import Field

from .common import IdStr, StrictModel
from .npc_plan import AnimationFallback

#: Animation key: an intent ("walk"), a Tripo preset key ("box_01"), or a
#: gait variant ("run.punching") -- the ``<gait>.<variant>`` form the engine's
#: locomotion selector understands.
MOTION_KEY_PATTERN = r"^[a-z0-9_]{1,48}(\.[a-z0-9_]{1,48})?$"


class BodyPlan(str, Enum):
    """The body a character has, as far as rigging is concerned."""

    #: Upright, two legs, two arms, a head and a torso -- human or not. A
    #: fish-man, an alien, a robot and a transforming car all count.
    biped_humanoid = "biped_humanoid"
    #: Two legs but not humanoid (no arms): a bird on foot, a T-rex.
    biped_non_humanoid = "biped_non_humanoid"
    quadruped = "quadruped"
    hexapod = "hexapod"
    octopod = "octopod"
    avian = "avian"
    serpentine = "serpentine"
    aquatic = "aquatic"
    #: Not a creature at all (a drone, a turret, a floating orb).
    non_creature = "non_creature"


class RigProvider(str, Enum):
    uthana = "uthana"
    tripo = "tripo"
    none = "none"


class MotionSource(str, Enum):
    """Where one motion comes from.

    New sources are added here, each with a resolver in
    ``pipeline/uthana/motion_library.py`` that turns a planned motion into a
    Uthana ``motion_id``. Download and merge only ever see that id.
    """

    #: Generated (or reused by name) with Uthana's locomotion API.
    uthana_locomotion = "uthana_locomotion"
    #: An existing motion already in the Uthana organisation, by id. The
    #: shared idle is the first one.
    uthana_library = "uthana_library"
    #: A Tripo preset clip retargeted onto a Tripo rig.
    tripo_preset = "tripo_preset"
    #: No clip; generated C++ drives joints itself.
    engine_procedural = "engine_procedural"
    none = "none"


class MotionKind(str, Enum):
    idle = "idle"
    #: Travels across the ground (walk, run, crouch-walk, zombie shamble).
    locomotion = "locomotion"
    #: A one-shot action or gesture (attack, hurt, die, wave).
    action = "action"


class TripoRigCheckRecord(StrictModel):
    riggable: bool = False
    rig_type: str = ""
    task_id: str = ""
    error: str = ""


class RequiredMotion(StrictModel):
    key: str = Field(..., pattern=MOTION_KEY_PATTERN)
    kind: MotionKind
    purpose: str = Field(default="", max_length=400)
    source: MotionSource
    #: Uthana locomotion style the routing agent had in mind (the motion-plan
    #: agent makes the final choice).
    style_hint: Optional[str] = Field(default=None, max_length=64)
    #: Exact Tripo v1.0 preset id when ``source`` is ``tripo_preset``.
    tripo_preset: Optional[str] = Field(default=None, max_length=96)
    #: Existing Uthana motion id when ``source`` is ``uthana_library``.
    library_motion_id: Optional[str] = Field(default=None, max_length=64)


class UnmetMotion(StrictModel):
    key: str = Field(..., pattern=MOTION_KEY_PATTERN)
    reason: str = Field(default="", max_length=400)
    fallback: AnimationFallback = AnimationFallback.none


class CharacterRoute(StrictModel):
    asset_id: IdStr
    character_role: str = Field(default="npc", pattern=r"^(npc|player_avatar)$")
    body_plan: BodyPlan
    tripo_rig_check: Optional[TripoRigCheckRecord] = None
    rig_provider: RigProvider
    animation_provider: RigProvider
    required_motions: List[RequiredMotion] = Field(default_factory=list, max_length=12)
    unmet: List[UnmetMotion] = Field(default_factory=list, max_length=12)
    rationale: str = Field(default="", max_length=2000)
    #: ``code`` for the deterministic rules, ``agent`` when the routing agent
    #: chose between two capable providers.
    decided_by: str = Field(default="code", pattern=r"^(code|agent)$")
    #: Set by ``uthana_rigging`` when the Uthana rig failed or was not
    #: trusted and the character falls back to another provider.
    fallback: Optional[RigProvider] = None
    fallback_reason: str = Field(default="", max_length=600)

    def effective_rig_provider(self) -> RigProvider:
        return self.fallback or self.rig_provider

    def effective_animation_provider(self) -> RigProvider:
        if self.fallback is not None:
            return self.fallback
        return self.animation_provider

    def motions_from(self, *sources: MotionSource) -> list[RequiredMotion]:
        return [m for m in self.required_motions if m.source in sources]


class CharacterRoutingPlan(StrictModel):
    prompt_id: IdStr
    routes: List[CharacterRoute] = Field(default_factory=list, max_length=48)

    def route_for(self, asset_id: str) -> Optional[CharacterRoute]:
        for r in self.routes:
            if r.asset_id == asset_id:
                return r
        return None

    def uses(self, provider: RigProvider, asset_id: str) -> bool:
        route = self.route_for(asset_id)
        return route is not None and route.effective_rig_provider() == provider


# ---------------------------------------------------------------------------
# What the routing agent returns (one entry per NPC it was asked about)
# ---------------------------------------------------------------------------


class RoutingDecision(StrictModel):
    asset_id: IdStr
    body_plan: BodyPlan
    #: Only honoured for biped humanoids Tripo can rig; every other case is
    #: decided by fixed rules and this field is ignored.
    rig_provider: RigProvider
    required_motions: List[RequiredMotion] = Field(default_factory=list, max_length=12)
    unmet: List[UnmetMotion] = Field(default_factory=list, max_length=12)
    rationale: str = Field(default="", max_length=2000)


class RoutingAgentOutput(StrictModel):
    decisions: List[RoutingDecision] = Field(default_factory=list, max_length=48)
