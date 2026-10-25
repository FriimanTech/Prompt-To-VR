"""What happened when the built game actually ran on the headset.

Written by the ``quest_runtime_verify`` stage (Quest projects only) to
``projects/<id>/runtime_verification.json``. Every check is derived from what
THIS game asked for -- its NPC plan and compiled scene -- and only runs when
the game calls for it, so a game with no NPCs, or with NPCs that are meant to
stand still, is judged on what it is supposed to do.

The stage is report-only: it records evidence and passes or fails; it never
repairs anything.
"""

from __future__ import annotations

from enum import Enum
from typing import List, Optional

from pydantic import Field

from .common import StrictModel


class VerificationStatus(str, Enum):
    passed = "passed"
    failed = "failed"
    #: No headset (or no adb, an unauthorized or ambiguous device, or one
    #: that disconnected mid-run). Never a failure.
    skipped = "skipped"


class CheckOutcome(str, Enum):
    passed = "pass"
    failed = "fail"
    #: The game does not call for this check (no NPCs, no waves, ...).
    not_applicable = "not_applicable"


class VerificationCheck(StrictModel):
    name: str = Field(..., max_length=120)
    #: ``base``, ``persistent``, ``stationary``, ``follower``, ``encounter``,
    #: ``enemy``, ``npc``.
    category: str = Field(default="base", max_length=32)
    #: The NPC asset or entity this check is about, when it is about one.
    subject: str = Field(default="", max_length=120)
    outcome: CheckOutcome
    detail: str = Field(default="", max_length=2000)
    #: Log lines and measured values that justify the outcome.
    evidence: List[str] = Field(default_factory=list, max_length=20)


class RuntimeVerification(StrictModel):
    project_id: str
    status: VerificationStatus
    #: Why the run was skipped, or the one-line summary of a pass / fail.
    reason: str = Field(default="", max_length=2000)
    device_serial: Optional[str] = None
    package: Optional[str] = None
    capture_seconds: float = 0.0
    #: "not applicable: this game has no NPCs" when the plan is empty.
    npc_summary: str = Field(default="", max_length=500)
    checks: List[VerificationCheck] = Field(default_factory=list)
    log_path: Optional[str] = None
    started_at: Optional[str] = None
    finished_at: Optional[str] = None
