"""ProjectState: bookkeeping for an individual local project."""

from __future__ import annotations

from datetime import datetime, timezone
from enum import Enum
from typing import Dict, List, Optional

from pydantic import Field

from .common import IdStr, StrictModel


class ProjectStage(str, Enum):
    created = "created"
    build_spec = "build_spec"
    asset_manifest = "asset_manifest"
    mechanic_plan = "mechanic_plan"
    scene_spec = "scene_spec"
    reference_images = "reference_images"
    tripo_models = "tripo_models"
    asset_normalization = "asset_normalization"
    code_generation = "code_generation"
    scene_compile = "scene_compile"
    cmake_configure = "cmake_configure"
    cmake_build = "cmake_build"
    ready_to_launch = "ready_to_launch"
    failed = "failed"


class ProjectStatus(str, Enum):
    idle = "idle"
    running = "running"
    paused = "paused"
    succeeded = "succeeded"
    failed = "failed"


class StageRecord(StrictModel):
    stage: ProjectStage
    status: ProjectStatus
    started_at: Optional[datetime] = None
    ended_at: Optional[datetime] = None
    message: str = ""
    artifacts: List[str] = Field(default_factory=list)


def _utcnow() -> datetime:
    return datetime.now(tz=timezone.utc)


class ProjectState(StrictModel):
    project_id: IdStr
    title: str = Field(..., max_length=128)
    prompt: str = Field(..., max_length=16_000)
    created_at: datetime = Field(default_factory=_utcnow)
    updated_at: datetime = Field(default_factory=_utcnow)

    current_stage: ProjectStage = ProjectStage.created
    status: ProjectStatus = ProjectStatus.idle

    stages: List[StageRecord] = Field(default_factory=list)
    env_snapshot: Dict[str, str] = Field(default_factory=dict)

    latest_build_exe: Optional[str] = None
    latest_build_dir: Optional[str] = None
    latest_scene_json: Optional[str] = None
