"""API request/response models."""

from __future__ import annotations

from pydantic import BaseModel, Field


class CreateProjectRequest(BaseModel):
    title: str = Field(..., min_length=1, max_length=128)
    prompt: str = Field(..., min_length=10, max_length=16_000)


class RunStageRequest(BaseModel):
    # Run exactly one stage.
    stage: str | None = None
    # Auto-resume from the first not-yet-succeeded stage.
    resume: bool = False
    # Resume from an explicit stage: run this stage and everything after it.
    resume_from: str | None = None


class LaunchRequest(BaseModel):
    scene_json: str | None = None
