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
from typing import Dict, List, Optional

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
    #: Reported, never failing: e.g. a frame-rate miss the user's own locked
    #: optimization settings caused.
    warning = "warning"


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


class MemoryStatus(str, Enum):
    #: Measured on a connected Quest 3 and every memory check passed.
    verified_safe = "verified_safe"
    #: The build-time estimate fits, but no headset ran it. Never shown as
    #: verified.
    estimated_safe_not_device_tested = "estimated_safe_not_device_tested"
    #: Still over the engine target after automatic corrections, no headset.
    at_risk = "at_risk"
    #: A measured failure (ceiling, leak or a low-memory kill) remains.
    failed_on_device = "failed_on_device"


class MemoryReport(StrictModel):
    """Quest 3 memory: Meta's 5.75 GiB PSS limit versus the engine's own
    4.5 GiB steady-state target and 5.0 GiB peak ceiling."""

    status: MemoryStatus
    reasons: List[str] = Field(default_factory=list)
    limit_mb: float = 5888.0
    ceiling_mb: float = 5120.0
    target_mb: float = 4608.0
    estimated_steady_mb: Optional[float] = None
    estimated_peak_mb: Optional[float] = None
    measured_peak_mb: Optional[float] = None
    measured_steady_mb: Optional[float] = None
    #: Category -> measured MB (dumpsys meminfo / gpumeminfo / engine mem:).
    measured: Dict[str, float] = Field(default_factory=dict)
    leak_mb_per_cycle: Optional[float] = None
    low_memory_kills: List[str] = Field(default_factory=list)
    #: Automatic corrections the build needed to fit (quality compromises).
    compromises: List[str] = Field(default_factory=list)


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
    #: Measured frame cost (medians of the base pass's ``perf:`` lines).
    performance: Dict[str, float] = Field(default_factory=dict)
    memory: Optional[MemoryReport] = None
