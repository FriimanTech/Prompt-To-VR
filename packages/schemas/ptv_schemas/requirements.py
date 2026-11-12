"""RequirementsLedger: every atomic thing the prompt asks for.

The pipeline used to lose requirements silently. The shooting-range prompt
asked for lane-1 targets destroyed by bullets OR grenades and for grenades
that explode; the mechanic plan had neither, nothing noticed, and the APK
shipped without them. The ledger makes each requirement a first-class item:

* the ``requirements`` stage extracts them, each with the prompt's VERBATIM
  quote (checked, like every other ``explicit`` decision in the pipeline);
* every later planning stage CLAIMS the ones it satisfies (``satisfies`` on
  asset items, mechanic-plan items, scene entities; ``ctx.satisfies`` in a
  world builder), and a deterministic gate fails the stage -- after one
  repair round -- when a requirement it owns is unclaimed;
* scene review and on-headset verification record what was actually seen,
  and ``requirements_report.json`` reports pass / fail / unverified per
  requirement.
"""

from __future__ import annotations

from enum import Enum
from typing import Dict, List, Optional

from pydantic import Field

from .common import IdStr, StrictModel


class RequirementKind(str, Enum):
    #: An object or character must exist ("a Glock", "huge bulky goblins").
    asset = "asset"
    #: How something looks ("an all-black tactical outfit").
    visual = "visual"
    #: Where something is ("guns on the tables", "targets hang from the ceiling").
    placement = "placement"
    #: The environment's structure ("three lanes", "lane 3 is wider").
    layout = "layout"
    #: How the player uses an object ("throw the grenade", "press the red button").
    interaction = "interaction"
    #: A gameplay system ("a 20-second timed challenge that counts hits").
    mechanic = "mechanic"
    #: A rule or constraint ("no manual reload", "infinite ammo").
    rule = "rule"
    #: What NPCs are and do ("regular goblins in front, killable, respawn").
    npc = "npc"


class RequirementPriority(str, Enum):
    must = "must"
    should = "should"


class Requirement(StrictModel):
    id: IdStr = Field(..., description="Stable id, e.g. 'req_lane2_timer'.")
    kind: RequirementKind
    priority: RequirementPriority = RequirementPriority.must
    statement: str = Field(..., max_length=400,
                           description="One atomic, checkable statement in plain words.")
    quote: str = Field(..., max_length=800,
                       description="The prompt's own words, copied verbatim.")
    subjects: List[str] = Field(
        default_factory=list, max_length=12,
        description="The objects, characters or areas it is about, in the prompt's words.")
    acceptance: List[str] = Field(
        default_factory=list, max_length=6,
        description="What an observer would see when it is met.")


class RequirementClaim(StrictModel):
    """One artifact item saying it satisfies a requirement."""

    stage: str = Field(..., max_length=64)
    item: str = Field(..., max_length=128)
    note: str = Field(default="", max_length=300)


class RequirementOutcome(str, Enum):
    passed = "pass"
    failed = "fail"
    unverified = "unverified"


class RequirementResult(StrictModel):
    """What was actually observed for one requirement."""

    id: IdStr
    outcome: RequirementOutcome = RequirementOutcome.unverified
    evidence: List[str] = Field(default_factory=list, max_length=20)
    source: str = Field(default="", max_length=64,
                        description="scene_review | quest_runtime_verify | plan")


class RequirementsLedger(StrictModel):
    prompt_id: str = Field(..., max_length=128)
    requirements: List[Requirement] = Field(default_factory=list, max_length=200)
    #: requirement id -> who claimed it, filled by the coverage gates.
    claims: Dict[str, List[RequirementClaim]] = Field(default_factory=dict)
    #: requirement id -> latest observed outcome.
    results: Dict[str, RequirementResult] = Field(default_factory=dict)
    notes: List[str] = Field(default_factory=list, max_length=100)

    def by_id(self) -> Dict[str, Requirement]:
        return {r.id: r for r in self.requirements}

    def get(self, req_id: str) -> Optional[Requirement]:
        return self.by_id().get(req_id)
