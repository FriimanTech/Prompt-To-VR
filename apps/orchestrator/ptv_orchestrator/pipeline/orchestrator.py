"""Top-level pipeline orchestrator."""

from __future__ import annotations

import threading
from typing import Callable

from ptv_schemas import ProjectStage, ProjectStatus, TargetDevice

from ..events import get_bus
from ..settings import Settings, get_settings
from ..storage import ProjectStore
from . import (
    stage_asset_contract,
    stage_asset_manifest,
    stage_asset_animation,
    stage_asset_normalize,
    stage_asset_optimize,
    stage_asset_rigging,
    stage_build_spec,
    stage_cmake,
    stage_code_generation,
    stage_engine_promotion,
    stage_mechanic_plan,
    stage_quest_build,
    stage_reference_images,
    stage_scene_compile,
    stage_scene_spec,
    stage_tripo_models,
    stage_world_generation,
)
from .context import PipelineContext, StageResult


# Per-project lock so two concurrent background tasks (e.g. the user hitting
# "Run full pipeline" and then "Resume" while the first is still running)
# can't both spawn cmake.exe against the same build dir. Two simultaneous
# cmake processes on the same project race on the vcpkg install lock and
# one inevitably gets killed with STATUS_CONTROL_C_EXIT.
_PROJECT_LOCKS: dict[str, threading.Lock] = {}
_PROJECT_LOCKS_GUARD = threading.Lock()


def _project_lock(project_id: str) -> threading.Lock:
    with _PROJECT_LOCKS_GUARD:
        lock = _PROJECT_LOCKS.get(project_id)
        if lock is None:
            lock = threading.Lock()
            _PROJECT_LOCKS[project_id] = lock
        return lock


StageFn = Callable[[PipelineContext], StageResult]


# Stages 1-9 are identical for both targets: prompt -> scene.json + generated
# C++ + GLBs. Only the build stage differs by target_device.
_SHARED_STAGES: list[tuple[ProjectStage, StageFn]] = [
    (ProjectStage.build_spec, stage_build_spec.run),
    (ProjectStage.asset_manifest, stage_asset_manifest.run),
    (ProjectStage.mechanic_plan, stage_mechanic_plan.run),
    (ProjectStage.scene_spec, stage_scene_spec.run),
    (ProjectStage.reference_images, stage_reference_images.run),
    (ProjectStage.tripo_models, stage_tripo_models.run),
    # Must precede asset_normalization: rigging REPLACES the primary GLB, so
    # everything downstream (validation, socket derivation, decimation, the
    # scene's mesh.skinned flag) has to see the rigged mesh. Rigging after
    # optimization would leave the entire LOD chain un-rigged.
    (ProjectStage.asset_rigging, stage_asset_rigging.run),
    # Retarget onto the skeleton the previous stage produced, still before
    # normalization so the ANIMATED GLB is the one everything downstream
    # validates, sockets, decimates and compiles. A rigged enemy with no
    # clips is a statue that slides across the floor.
    (ProjectStage.asset_animation, stage_asset_animation.run),
    (ProjectStage.asset_normalization, stage_asset_normalize.run),
    # Must precede code_generation: the codegen agent needs asset_contract.json
    # in context so it writes ctx.sockets->socket_world(self, "muzzle") against
    # socket names that actually exist, instead of guessing a hard-coded
    # offset. Sockets are derived in unit space from the SOURCE GLB, so running
    # ahead of asset_optimization is safe -- the optimizer preserves the source
    # coordinate system.
    (ProjectStage.asset_contract, stage_asset_contract.run),
    (ProjectStage.code_generation, stage_code_generation.run),
    (ProjectStage.world_generation, stage_world_generation.run),
    # Runs here (not next to asset_normalization) so the engine build tree
    # already exists from the codegen compile-verify step -- ptv_glb_optimize
    # is then a cheap incremental target rather than a cold engine build.
    (ProjectStage.asset_optimization, stage_asset_optimize.run),
    (ProjectStage.scene_compile, stage_scene_compile.run),
]

# PC VR: build a Windows .exe with CMake/MSVC.
_PCVR_TAIL: list[tuple[ProjectStage, StageFn]] = [
    (ProjectStage.cmake_build, stage_cmake.run),
    (ProjectStage.engine_promotion, stage_engine_promotion.run),
]

# Quest: build a standalone Android APK with Gradle + the NDK.
_QUEST_TAIL: list[tuple[ProjectStage, StageFn]] = [
    (ProjectStage.quest_build, stage_quest_build.run),
    (ProjectStage.engine_promotion, stage_engine_promotion.run),
]


def _stages_for(target_device: TargetDevice) -> list[tuple[ProjectStage, StageFn]]:
    """The ordered stage list for a target device. Shared stages 1-9 then the
    device-specific build tail."""
    tail = _QUEST_TAIL if target_device == TargetDevice.quest else _PCVR_TAIL
    return _SHARED_STAGES + tail


def _stages_for_project(ctx: PipelineContext) -> list[tuple[ProjectStage, StageFn]]:
    device = getattr(
        ctx.store.load_state(ctx.project_id), "target_device", TargetDevice.pcvr
    )
    return _stages_for(device)


def _make_ctx(project_id: str, settings: Settings | None = None) -> PipelineContext:
    settings = settings or get_settings()
    return PipelineContext(
        project_id=project_id,
        settings=settings,
        store=ProjectStore(settings.projects_dir),
        bus=get_bus(),
    )


def run_pipeline(project_id: str, settings: Settings | None = None) -> None:
    lock = _project_lock(project_id)
    if not lock.acquire(blocking=False):
        # Another pipeline run is in flight. Emit a UI event so the user
        # sees why nothing is happening and bail out cleanly; don't raise
        # (FastAPI BackgroundTasks would surface an unhelpful traceback).
        ctx = _make_ctx(project_id, settings)
        ctx.emit(
            ProjectStage.build_spec,
            "skipped",
            "another run is already in progress for this project",
        )
        return
    try:
        ctx = _make_ctx(project_id, settings)
        state = ctx.store.load_state(project_id)
        state.status = ProjectStatus.running
        ctx.store.save_state(state)
        try:
            for stage, fn in _stages_for(state.target_device):
                fn(ctx)
        except Exception as e:
            state = ctx.store.load_state(project_id)
            ctx.store.record_stage(
                state,
                state.current_stage,
                ProjectStatus.failed,
                message=str(e)[:1000],
            )
            ctx.emit(state.current_stage, "failed", str(e)[:300])
            raise
    finally:
        lock.release()


def resume_pipeline(project_id: str, settings: Settings | None = None) -> None:
    lock = _project_lock(project_id)
    if not lock.acquire(blocking=False):
        ctx = _make_ctx(project_id, settings)
        ctx.emit(
            ProjectStage.build_spec,
            "skipped",
            "another run is already in progress for this project",
        )
        return
    try:
        ctx = _make_ctx(project_id, settings)
        state = ctx.store.load_state(project_id)
        state.status = ProjectStatus.running
        ctx.store.save_state(state)

        # For each stage, find the LATEST (not just any) record. A stage is only
        # safely skippable if its most recent record is ``succeeded``. Once we hit
        # a stage that is not, we re-run it and everything downstream --
        # downstream artefacts depend on upstream ones, so skipping a middle stage
        # while rerunning a later one would produce an inconsistent build.
        latest_status: dict[ProjectStage, ProjectStatus] = {}
        for s in state.stages:
            latest_status[s.stage] = s.status  # records are appended in order

        resume_from: ProjectStage | None = None
        for stage, _fn in _stages_for(state.target_device):
            if latest_status.get(stage) != ProjectStatus.succeeded:
                resume_from = stage
                break

        _run_stages_from(ctx, resume_from)
    finally:
        lock.release()


def _run_stages_from(ctx: PipelineContext, resume_from: ProjectStage | None) -> None:
    """Run ``resume_from`` and every stage after it; skip the ones before.

    ``resume_from is None`` means everything upstream already succeeded, so all
    stages are skipped (nothing to do). Downstream artefacts depend on upstream
    ones, so once we start we run every remaining stage in order.
    """
    resuming = False
    for stage, fn in _stages_for_project(ctx):
        if not resuming:
            if resume_from is None or stage != resume_from:
                ctx.emit(stage, "skipped", "already done")
                continue
            resuming = True
        fn(ctx)


def resume_from_stage(
    project_id: str, stage: ProjectStage, settings: Settings | None = None
) -> None:
    """Resume the pipeline from an explicit stage: run ``stage`` and every
    stage after it (regardless of prior status); skip the ones before it.

    Unlike :func:`resume_pipeline` (which auto-detects the first not-yet-
    succeeded stage), this lets the operator force a re-run starting at a chosen
    stage -- e.g. pick ``world_generation`` to re-author the world and rebuild
    everything downstream.
    """
    lock = _project_lock(project_id)
    if not lock.acquire(blocking=False):
        ctx = _make_ctx(project_id, settings)
        ctx.emit(
            stage,
            "skipped",
            "another run is already in progress for this project",
        )
        return
    try:
        ctx = _make_ctx(project_id, settings)
        if stage not in [s for s, _ in _stages_for_project(ctx)]:
            # Not a pipeline-runnable stage (e.g. cmake_configure is folded into
            # cmake_build; created/ready_to_launch/failed are markers).
            ctx.emit(
                stage,
                "failed",
                f"stage '{stage.value}' cannot be run directly",
            )
            return
        state = ctx.store.load_state(project_id)
        state.status = ProjectStatus.running
        ctx.store.save_state(state)
        try:
            _run_stages_from(ctx, stage)
        except Exception as e:
            state = ctx.store.load_state(project_id)
            ctx.store.record_stage(
                state,
                state.current_stage,
                ProjectStatus.failed,
                message=str(e)[:1000],
            )
            ctx.emit(state.current_stage, "failed", str(e)[:300])
            raise
    finally:
        lock.release()


def run_stage(project_id: str, stage: ProjectStage, settings: Settings | None = None) -> None:
    lock = _project_lock(project_id)
    if not lock.acquire(blocking=False):
        ctx = _make_ctx(project_id, settings)
        ctx.emit(stage, "skipped", "another run is already in progress")
        return
    try:
        ctx = _make_ctx(project_id, settings)
        for s, fn in _stages_for_project(ctx):
            if s == stage:
                fn(ctx)
                return
        raise ValueError(f"Unknown stage: {stage}")
    finally:
        lock.release()
