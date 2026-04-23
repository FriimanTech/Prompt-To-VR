"""Top-level pipeline orchestrator."""

from __future__ import annotations

from typing import Callable

from ptv_schemas import ProjectStage, ProjectStatus

from ..events import get_bus
from ..settings import Settings, get_settings
from ..storage import ProjectStore
from . import (
    stage_asset_manifest,
    stage_asset_normalize,
    stage_build_spec,
    stage_cmake,
    stage_code_generation,
    stage_mechanic_plan,
    stage_reference_images,
    stage_scene_compile,
    stage_scene_spec,
    stage_tripo_models,
)
from .context import PipelineContext, StageResult


StageFn = Callable[[PipelineContext], StageResult]


# Stage sequence in the order defined by Instructions.md.
_STAGES: list[tuple[ProjectStage, StageFn]] = [
    (ProjectStage.build_spec, stage_build_spec.run),
    (ProjectStage.asset_manifest, stage_asset_manifest.run),
    (ProjectStage.mechanic_plan, stage_mechanic_plan.run),
    (ProjectStage.scene_spec, stage_scene_spec.run),
    (ProjectStage.reference_images, stage_reference_images.run),
    (ProjectStage.tripo_models, stage_tripo_models.run),
    (ProjectStage.asset_normalization, stage_asset_normalize.run),
    (ProjectStage.code_generation, stage_code_generation.run),
    (ProjectStage.scene_compile, stage_scene_compile.run),
    (ProjectStage.cmake_build, stage_cmake.run),
]


def _make_ctx(project_id: str, settings: Settings | None = None) -> PipelineContext:
    settings = settings or get_settings()
    return PipelineContext(
        project_id=project_id,
        settings=settings,
        store=ProjectStore(settings.projects_dir),
        bus=get_bus(),
    )


def run_pipeline(project_id: str, settings: Settings | None = None) -> None:
    ctx = _make_ctx(project_id, settings)
    state = ctx.store.load_state(project_id)
    state.status = ProjectStatus.running
    ctx.store.save_state(state)
    try:
        for stage, fn in _STAGES:
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


def resume_pipeline(project_id: str, settings: Settings | None = None) -> None:
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
    for stage, _fn in _STAGES:
        if latest_status.get(stage) != ProjectStatus.succeeded:
            resume_from = stage
            break

    resuming = False
    for stage, fn in _STAGES:
        if not resuming:
            if resume_from is None:
                ctx.emit(stage, "skipped", "already done")
                continue
            if stage != resume_from:
                ctx.emit(stage, "skipped", "already done")
                continue
            resuming = True
        fn(ctx)


def run_stage(project_id: str, stage: ProjectStage, settings: Settings | None = None) -> None:
    ctx = _make_ctx(project_id, settings)
    for s, fn in _STAGES:
        if s == stage:
            fn(ctx)
            return
    raise ValueError(f"Unknown stage: {stage}")
