"""API request/response models."""

from __future__ import annotations

from pydantic import BaseModel, Field


class CreateProjectRequest(BaseModel):
    title: str = Field(..., min_length=1, max_length=128)
    prompt: str = Field(..., min_length=10, max_length=16_000)


class RunStageRequest(BaseModel):
    stage: str | None = None
    resume: bool = False


class LaunchRequest(BaseModel):
    scene_json: str | None = None
