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
    # The user's "Custom Quest Optimizations" (and the same wording in the
    # main prompt) mapped onto the settings a user may lock. Never shapes
    # gameplay; no model call when no optimization wording appears.
    optimization_preferences = "optimization_preferences"
    # Every atomic thing the prompt asks for, each with its verbatim quote.
    # Every later planning stage claims the requirements it satisfies and a
    # deterministic gate fails a stage that leaves one unclaimed.
    requirements = "requirements"
    asset_manifest = "asset_manifest"
    # How every object is used: hold profile, the sockets to find on it, and
    # for weapons how they fire and what they fire (appending projectile
    # assets to the manifest before any image or Tripo money is spent).
    interaction_design = "interaction_design"
    # Reads the prompt for what every NPC is supposed to DO, before rigging
    # spends credits on animating something the game wants to slide.
    npc_plan = "npc_plan"
    # The game loop's rules (health, lives, respawn, objectives, win/lose,
    # end screens). After npc_plan because who can hurt the player and which
    # waves exist come from there; before mechanic_plan, which maps the loop
    # onto builtin.game_flow instead of generating one.
    game_design = "game_design"
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
    # Finds the sockets interaction_design asked for on each mesh: multi-view
    # renders, Gemini part segmentation + keypoints, exact triangle-ID
    # back-projection, per-kind geometric solvers and a render-and-verify
    # pass. Cached by GLB hash, so a resume never re-bills.
    socket_generation = "socket_generation"
    # Merges socket_generation's sockets with each character's measured model
    # frame and writes asset_contract.json. Runs BEFORE
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
    # Renders the compiled scene offline from the spawn and from every layout
    # requirement's viewpoint, checks placement deterministically, then has a
    # vision model confirm each layout requirement is visibly met.
    scene_review = "scene_review"
    cmake_configure = "cmake_configure"
    cmake_build = "cmake_build"
    # Quest (standalone Android APK) build path. Used instead of
    # cmake_configure/cmake_build when target_device == quest.
    quest_build = "quest_build"
    # Quest only, report-only: install + launch the APK on the connected
    # headset in verification mode and check, from the engine's NPC
    # telemetry, that what the game asked for actually happens. Skipped --
    # never failed -- when no headset is connected.
    quest_runtime_verify = "quest_runtime_verify"
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

    # Free text from the New Project page's collapsed "Custom Quest
    # Optimizations" box. Empty by default (everything automatic); parsed by
    # the optimization_preferences stage.
    custom_quest_optimizations: str = Field(default="", max_length=4000)

    stages: List[StageRecord] = Field(default_factory=list)
    env_snapshot: Dict[str, str] = Field(default_factory=dict)

    latest_build_exe: Optional[str] = None
    latest_build_dir: Optional[str] = None
    latest_scene_json: Optional[str] = None
    # Quest builds: path to the produced debug APK.
    latest_build_apk: Optional[str] = None
