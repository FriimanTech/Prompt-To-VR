"""API request/response models."""

from __future__ import annotations

from pydantic import BaseModel, Field


class CreateProjectRequest(BaseModel):
    title: str = Field(..., min_length=1, max_length=128)
    prompt: str = Field(..., min_length=10, max_length=16_000)
    # Which VR target to build for: "pcvr" (Windows .exe over Quest Link) or
    # "quest" (standalone Android APK). Defaults to pcvr for back-compat.
    target_device: str = Field(default="pcvr")
    # What the player is: "hands_only" (the engine's default rigged hands, no
    # body) or "full_avatar" (a generated full-body GLB viewed first person).
    player_experience: str = Field(default="full_avatar")


class RunStageRequest(BaseModel):
    # Run exactly one stage.
    stage: str | None = None
    # Auto-resume from the first not-yet-succeeded stage.
    resume: bool = False
    # Resume from an explicit stage: run this stage and everything after it.
    resume_from: str | None = None


class LaunchRequest(BaseModel):
    scene_json: str | None = None
