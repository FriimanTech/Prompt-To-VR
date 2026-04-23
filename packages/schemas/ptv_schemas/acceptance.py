"""AcceptanceTestPlan: human-runnable VR acceptance checks."""

from __future__ import annotations

from typing import List

from pydantic import Field

from .common import IdStr, StrictModel


class AcceptanceCheck(StrictModel):
    id: IdStr
    description: str = Field(..., max_length=2000)
    expected_outcome: str = Field(..., max_length=2000)
    manual: bool = True


class AcceptanceTestPlan(StrictModel):
    prompt_id: IdStr
    checks: List[AcceptanceCheck] = Field(..., min_length=1, max_length=32)
