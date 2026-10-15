"""ProjectState: bookkeeping for an individual local project."""

from __future__ import annotations

from datetime import datetime, timezone
from enum import Enum
from typing import Dict, List, Optional

from pydantic import Field

from .common import IdStr, StrictModel
from .build_spec import PlayerExperience, TargetDevice


class ProjectStage(str, Enum):
    created = "created"
    build_spec = "build_spec"
    asset_manifest = "asset_manifest"
    # Reads the prompt for what every NPC is supposed to DO, before rigging
    # spends credits on animating something the game wants to slide.
    npc_plan = "npc_plan"
    mechanic_plan = "mechanic_plan"
    scene_spec = "scene_spec"
    reference_images = "reference_images"
    tripo_models = "tripo_models"
    # Decides per character whether Tripo or Uthana rigs and animates it,
    # once the Tripo meshes exist (rig-check needs the model).
    character_routing = "character_routing"
    # Uthana auto-rig (with finger joints) for the player avatar and the
    # humanoid NPCs routed to Uthana.
    uthana_rigging = "uthana_rigging"
    asset_rigging = "asset_rigging"
    asset_animation = "asset_animation"
    # Plans every Uthana motion (style, speed, strides, name) before any is
    # generated.
    uthana_motion_plan = "uthana_motion_plan"
    # Resolves, downloads and merges the planned Uthana motions into one
    # animated GLB per character.
    uthana_animation = "uthana_animation"
    asset_normalization = "asset_normalization"
    # Derives each asset's named sockets (muzzle, grip) from its geometry and
    # writes asset_contract.json. Deterministic, no model calls. Runs BEFORE
    # code_generation because the codegen agent needs the contract in context:
    # it writes ctx.sockets->socket_world(self, "muzzle") against socket names
    # it can actually see, rather than guessing an offset.
    asset_contract = "asset_contract"
    # Decimates GLBs + generates LOD chains against the device triangle
    # budget. Runs after world_generation, before scene_compile.
    asset_optimization = "asset_optimization"
    code_generation = "code_generation"
    world_generation = "world_generation"
    scene_compile = "scene_compile"
    cmake_configure = "cmake_configure"
    cmake_build = "cmake_build"
    # Quest (standalone Android APK) build path. Used instead of
    # cmake_configure/cmake_build when target_device == quest.
    quest_build = "quest_build"
    quest_install = "quest_install"
    engine_promotion = "engine_promotion"
    ready_to_launch = "ready_to_launch"
    failed = "failed"


class ProjectStatus(str, Enum):
    idle = "idle"
    running = "running"
    paused = "paused"
    succeeded = "succeeded"
    failed = "failed"
    # The user pressed Abort, or the orchestrator was restarted mid-run.
    # Distinct from ``failed`` because nothing is broken: the project is
    # immediately re-runnable, and the UI says so instead of showing an
    # error the user has to interpret.
    aborted = "aborted"


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

    # Which VR target this project builds for. Chosen at create time (PC VR /
    # Quest toggle) and used to route the build + launch stages. Defaults to
    # pcvr so existing projects load unchanged.
    target_device: TargetDevice = TargetDevice.pcvr

    # Hands Only vs Full Player Avatar. Chosen at create time like
    # target_device and mirrored into BuildSpec.player_experience. Defaults to
    # full_avatar so existing projects keep the body they were planned with.
    player_experience: PlayerExperience = PlayerExperience.full_avatar

    stages: List[StageRecord] = Field(default_factory=list)
    env_snapshot: Dict[str, str] = Field(default_factory=dict)

    latest_build_exe: Optional[str] = None
    latest_build_dir: Optional[str] = None
    latest_scene_json: Optional[str] = None
    # Quest builds: path to the produced debug APK.
    latest_build_apk: Optional[str] = None
