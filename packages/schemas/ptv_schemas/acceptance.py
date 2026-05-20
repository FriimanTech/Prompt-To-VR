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
    # No upper bound: the check count tracks BuildSpec.acceptance_tests
    # which itself scales with the number of generated mechanics in a
    # full VR game.
    checks: List[AcceptanceCheck] = Field(..., min_length=1)
