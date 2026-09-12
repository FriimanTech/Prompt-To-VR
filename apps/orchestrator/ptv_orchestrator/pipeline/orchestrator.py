"""Top-level pipeline orchestrator.

Runs are claimed through the :mod:`..runs` registry, which serves two
purposes. It enforces one run per project -- two concurrent cmake processes
on the same build dir race on the vcpkg install lock and one inevitably dies
with ``STATUS_CONTROL_C_EXIT`` -- and it carries the cancel flag and child
process handles that make Abort possible.

Every entry point here guarantees the project reaches a TERMINAL status.
Leaving ``running`` behind is not a cosmetic problem: the UI disables every
button while a project claims to be running, so a stage that died without
stamping a result locked the user out of their own project.
"""

from __future__ import annotations

from contextlib import contextmanager
from typing import Callable, Iterator, Optional

from ptv_schemas import ProjectStage, ProjectStatus, TargetDevice

from ..events import get_bus
from ..runs import ABORT_MESSAGE, RunCancelled, RunHandle, get_registry
from ..settings import Settings, get_settings
from ..storage import ProjectStore
from . import (
    stage_asset_contract,
    stage_asset_manifest,
    stage_asset_animation,
    stage_asset_normalize,
    stage_asset_optimize,
    stage_asset_rigging,
    stage_npc_plan,
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
from .target_device import sync_build_spec_device


StageFn = Callable[[PipelineContext], StageResult]


# Stages 1-9 are identical for both targets: prompt -> scene.json + generated
# C++ + GLBs. Only the build stage differs by target_device.
_SHARED_STAGES: list[tuple[ProjectStage, StageFn]] = [
    (ProjectStage.build_spec, stage_build_spec.run),
    (ProjectStage.asset_manifest, stage_asset_manifest.run),
    # Before rigging (it decides what needs animating and what does not) and
    # before mechanic/scene planning (both need to know who the combatants
    # are).
    (ProjectStage.npc_plan, stage_npc_plan.run),
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
    # Runs BEFORE code_generation, which is the reverse of the original order.
    #
    # The worlds are what gameplay code acts on. A mechanic that opens a door,
    # routes an NPC through a building, or reacts to a lever needs the ids of
    # the things the world architect actually declared -- and until this stage
    # has run, nobody knows them. Generating C++ first meant the codegen agent
    # had to invent plausible names for objects that did not exist yet, and
    # every one of those guesses resolved to nothing at runtime.
    #
    # There is no dependency the other way: a world builder links no ECS and
    # never calls into a mechanic, so it has nothing to learn from the
    # generated gameplay code.
    (ProjectStage.world_generation, stage_world_generation.run),
    (ProjectStage.code_generation, stage_code_generation.run),
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


def _make_ctx(
    project_id: str,
    settings: Settings | None = None,
    run: Optional[RunHandle] = None,
) -> PipelineContext:
    settings = settings or get_settings()
    return PipelineContext(
        project_id=project_id,
        settings=settings,
        store=ProjectStore(settings.projects_dir),
        bus=get_bus(),
        run=run,
    )


@contextmanager
def _claimed_run(
    project_id: str,
    settings: Settings | None,
    busy_stage: ProjectStage,
) -> Iterator[Optional[PipelineContext]]:
    """Claim the project, yield a context, and always leave a terminal status.

    Yields ``None`` when another run already holds the project; the caller
    then does nothing. Emitting a "skipped" event rather than raising keeps
    FastAPI's BackgroundTasks from surfacing an unhelpful traceback for what
    is really just a double-click.
    """
    registry = get_registry()
    handle = registry.start(project_id)
    if handle is None:
        ctx = _make_ctx(project_id, settings)
        ctx.emit(
            busy_stage,
            "skipped",
            "another run is already in progress for this project",
        )
        yield None
        return

    ctx = _make_ctx(project_id, settings, run=handle)
    try:
        yield ctx
    except RunCancelled:
        _stamp(ctx, ProjectStatus.aborted, ABORT_MESSAGE)
        # Swallowed deliberately: the user asked for this, and a traceback
        # in the server log would imply something went wrong.
    except Exception as exc:
        _stamp(ctx, ProjectStatus.failed, _describe(exc))
        raise
    except BaseException as exc:
        # KeyboardInterrupt, SystemExit and asyncio.CancelledError derive from
        # BaseException, not Exception, so a process teardown used to escape
        # the arm above and leave a stage stamped with whatever the generic
        # handler managed -- in practice an EMPTY message, which is
        # indistinguishable from a real failure with a blank error.
        #
        # ``uvicorn --reload`` does exactly this on every .py edit: it kills
        # the worker mid-stage while the build's own child process (a Gradle
        # daemon, say) carries on and finishes. Saying so is the difference
        # between "my pipeline is broken" and "re-run that stage".
        _stamp(
            ctx,
            ProjectStatus.aborted,
            f"interrupted: the server process was terminated mid-stage "
            f"({_describe(exc)}). A dev-server reload does this on any code "
            f"edit. Nothing is wrong with the project -- re-run the stage.",
        )
        raise
    else:
        _settle(ctx)
    finally:
        handle.kill_processes()
        registry.finish(project_id)


def _describe(exc: BaseException) -> str:
    """A message that is never empty, however the exception was raised.

    ``str(exc)`` is blank for anything constructed without arguments --
    ``SystemExit()``, ``CancelledError()``, a bare ``RuntimeError()`` -- and a
    stage stamped ``failed`` with an empty message tells a reader nothing at
    all. The type name alone is enough to separate a build error from a
    process teardown.
    """
    text = str(exc).strip()
    if text:
        return f"{type(exc).__name__}: {text}"[:1000]
    return type(exc).__name__


def _stamp(ctx: PipelineContext, status: ProjectStatus, message: str) -> None:
    """Record a terminal status against whatever stage was current.

    Never raises: this runs while handling another exception, and losing the
    original error to a bookkeeping failure would be the worse outcome.
    """
    text = (message or "").strip() or f"{status.value} (no detail reported)"
    try:
        state = ctx.store.load_state(ctx.project_id)
        ctx.store.record_stage(
            state, state.current_stage, status, message=text[:1000]
        )
        ctx.emit(state.current_stage, status.value, text[:300])
    except Exception:  # pragma: no cover - defensive
        pass


def _settle(ctx: PipelineContext) -> None:
    """After a clean return, make sure the project is not still 'running'.

    A stage that returns without stamping a result (or a stage list that
    ended early) would otherwise leave the project looking busy forever.
    """
    state = ctx.store.load_state(ctx.project_id)
    if state.status != ProjectStatus.running:
        return
    ctx.store.record_stage(
        state,
        state.current_stage,
        ProjectStatus.succeeded,
        message="run finished",
    )


def _reconcile_build_spec_device(ctx: PipelineContext) -> None:
    """Re-stamp ``build_spec.json`` if it disagrees with the project.

    Projects created before the build_spec stage learned about the device
    have the schema default (``pcvr``) on disk regardless of what the user
    picked. Every stage that matters already reads
    ``ProjectState.target_device``, so this only repairs the mirrored copy --
    no need to pay for a BuildSpec regeneration to fix one enum.
    """
    state = ctx.store.load_state(ctx.project_id)
    try:
        spec = ctx.store.load_build_spec(ctx.project_id)
    except (FileNotFoundError, ValueError):
        return  # not planned yet, or unreadable: the stage will rewrite it
    spec, changed = sync_build_spec_device(spec, state.target_device)
    if not changed:
        return
    ctx.build_spec = spec
    ctx.store.save_build_spec(ctx.project_id, spec)
    ctx.emit(
        ProjectStage.build_spec,
        "info",
        f"build_spec.target_device re-stamped to the project's "
        f"{state.target_device.value!r}",
    )


def _mark_running(ctx: PipelineContext) -> None:
    state = ctx.store.load_state(ctx.project_id)
    state.status = ProjectStatus.running
    ctx.store.save_state(state)


def run_pipeline(project_id: str, settings: Settings | None = None) -> None:
    with _claimed_run(project_id, settings, ProjectStage.build_spec) as ctx:
        if ctx is None:
            return
        _mark_running(ctx)
        device = ctx.store.load_state(project_id).target_device
        for stage, fn in _stages_for(device):
            ctx.raise_if_cancelled()
            fn(ctx)


def resume_pipeline(project_id: str, settings: Settings | None = None) -> None:
    with _claimed_run(project_id, settings, ProjectStage.build_spec) as ctx:
        if ctx is None:
            return
        _mark_running(ctx)
        state = ctx.store.load_state(project_id)

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


def _run_stages_from(ctx: PipelineContext, resume_from: ProjectStage | None) -> None:
    """Run ``resume_from`` and every stage after it; skip the ones before.

    ``resume_from is None`` means everything upstream already succeeded, so all
    stages are skipped (nothing to do). Downstream artefacts depend on upstream
    ones, so once we start we run every remaining stage in order.
    """
    _reconcile_build_spec_device(ctx)
    resuming = False
    for stage, fn in _stages_for_project(ctx):
        if not resuming:
            if resume_from is None or stage != resume_from:
                ctx.emit(stage, "skipped", "already done")
                continue
            resuming = True
        ctx.raise_if_cancelled()
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
    with _claimed_run(project_id, settings, stage) as ctx:
        if ctx is None:
            return
        if stage not in [s for s, _ in _stages_for_project(ctx)]:
            # Not a pipeline-runnable stage (e.g. cmake_configure is folded into
            # cmake_build; created/ready_to_launch/failed are markers).
            ctx.emit(
                stage,
                "failed",
                f"stage '{stage.value}' cannot be run directly",
            )
            return
        _mark_running(ctx)
        _run_stages_from(ctx, stage)


def run_stage(project_id: str, stage: ProjectStage, settings: Settings | None = None) -> None:
    with _claimed_run(project_id, settings, stage) as ctx:
        if ctx is None:
            return
        _mark_running(ctx)
        _reconcile_build_spec_device(ctx)
        for s, fn in _stages_for_project(ctx):
            if s == stage:
                fn(ctx)
                return
        raise ValueError(f"Unknown stage: {stage}")
