"""OptimizationPreferences: the user's own Quest optimization choices.

Every optimization is automatic by default. A user may override a defined set
of quality / performance settings in the "Custom Quest Optimizations" box on
the New Project page, or with the same wording in the main prompt; the box
wins when the two disagree. Each override becomes a LOCK in
``PerformanceProfile.locks``: the runtime governor never moves a locked knob.

What is user-controllable, and why the rest is not, is documented in
``docs/quest-optimization.md``. Memory safety (Phase 13) is never
overridable; requests beyond a hard limit are clamped and reported here.
"""

from __future__ import annotations

from typing import List, Literal, Optional

from pydantic import Field

from .common import StrictModel
from .performance import LockValue, PerformanceLocks

PrefSource = Literal["custom_box", "main_prompt"]

#: Allowed values per setting. "auto" is the default and never a lock.
ALLOWED_VALUES: dict[str, tuple[str, ...]] = {
    "ffr": ("auto", "off", "fixed", "max"),
    "resolution": ("auto", "off", "high"),
    "lod": ("auto", "detail", "performance"),
    "texture_quality": ("auto", "high", "memory_saver"),
    "draw_distance": ("auto", "far", "performance"),
    "room_culling": ("auto", "off"),
    "anti_aliasing": ("auto", "high"),
    "frame_rate": ("auto", "90", "120"),
}

#: FFR level words -> the engine's 1..3 scale.
FFR_LEVELS = {"low": 1, "medium": 2, "high": 3}


class PrefSetting(StrictModel):
    """One overridden setting, with where it came from."""

    value: str
    level: Optional[int] = Field(default=None, ge=1, le=3,
                                 description="FFR fixed/max level: 1 low, 2 medium, 3 high")
    source: PrefSource
    quote: str = Field(..., description="Verbatim words from the source text.")
    differs_from_auto: bool = True


class UnsupportedRequest(StrictModel):
    request: str
    reason: str
    closest: str = Field(default="", description="The closest supported setting, if any.")
    source: PrefSource = "custom_box"
    quote: str = ""


class ClampedRequest(StrictModel):
    setting: str
    requested: str
    applied: str
    reason: str


class OptimizationPreferences(StrictModel):
    ffr: Optional[PrefSetting] = None
    resolution: Optional[PrefSetting] = None
    lod: Optional[PrefSetting] = None
    texture_quality: Optional[PrefSetting] = None
    draw_distance: Optional[PrefSetting] = None
    room_culling: Optional[PrefSetting] = None
    anti_aliasing: Optional[PrefSetting] = None
    frame_rate: Optional[PrefSetting] = None
    unsupported: List[UnsupportedRequest] = Field(default_factory=list)
    clamped: List[ClampedRequest] = Field(default_factory=list)
    #: False when the keyword gate found nothing and no model was called.
    model_called: bool = False

    def settings(self) -> dict[str, PrefSetting]:
        return {k: getattr(self, k) for k in ALLOWED_VALUES if getattr(self, k) is not None}

    def any_override(self) -> bool:
        return any(s.differs_from_auto for s in self.settings().values())

    def to_locks(self) -> PerformanceLocks:
        locks = PerformanceLocks()
        for name, s in self.settings().items():
            if s.value == "auto":
                continue
            setattr(locks, name, LockValue(value=s.value, level=s.level, source=s.source,
                                           quote=s.quote))
        return locks

    def summary_lines(self) -> list[str]:
        out = []
        for name, s in self.settings().items():
            lvl = f" ({next(k for k, v in FFR_LEVELS.items() if v == s.level)})" if s.level else ""
            out.append(f"{name.replace('_', ' ')}: {s.value}{lvl} [{s.source}: \"{s.quote}\"]")
        for u in self.unsupported:
            out.append(f"unsupported: {u.request} -- {u.reason}"
                       + (f"; closest: {u.closest}" if u.closest else ""))
        for c in self.clamped:
            out.append(f"clamped: {c.setting} {c.requested} -> {c.applied} ({c.reason})")
        return out
