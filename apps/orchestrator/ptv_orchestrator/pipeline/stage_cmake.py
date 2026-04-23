"""Stages 10/11: CMake configure + build."""

from __future__ import annotations

from datetime import datetime, timezone
from pathlib import Path

from ptv_schemas import ProjectStage, ProjectStatus

from ..build import configure_and_build
from .context import PipelineContext, StageResult


def run(ctx: PipelineContext) -> StageResult:
    ctx.emit(ProjectStage.cmake_configure, "running", "Configuring CMake")
    state = ctx.store.load_state(ctx.project_id)
    ctx.store.record_stage(state, ProjectStage.cmake_configure, ProjectStatus.running)

    source_dir = ctx.settings.engine_dir
    stamp = datetime.now(tz=timezone.utc).strftime("%Y%m%d-%H%M%S")
    build_dir = ctx.project_dir / "builds" / f"cmake-{stamp}"
    log_path = ctx.project_dir / "logs" / "cmake.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)

    def _sink(line: str) -> None:
        with log_path.open("a", encoding="utf-8") as f:
            f.write(line + "\n")
        ctx.emit(ProjectStage.cmake_build, "log", line[:512])

    result = configure_and_build(
        source_dir=source_dir,
        build_dir=build_dir,
        generator=ctx.settings.cmake_generator,
        platform=ctx.settings.cmake_generator_platform,
        config=ctx.settings.cmake_build_config,
        vcpkg_root=ctx.settings.vcpkg_root,
        extra_cache=(
            ("PTV_GENERATED_PROJECT_ID", ctx.project_id),
            ("PTV_SCENE_JSON", str(ctx.project_dir / "scene.json").replace("\\", "/")),
            ("PTV_PROJECT_DIR", str(ctx.project_dir).replace("\\", "/")),
        ),
        on_line=_sink,
    )

    status = ProjectStatus.succeeded if result.exit_code == 0 else ProjectStatus.failed
    ctx.store.record_stage(
        state, ProjectStage.cmake_configure, ProjectStatus.succeeded,
        message=f"Generator: {result.generator}",
    )
    ctx.store.record_stage(
        state,
        ProjectStage.cmake_build,
        status,
        message=f"CMake exit={result.exit_code}",
        artifacts=[
            str(log_path.relative_to(ctx.settings.repo_root)).replace("\\", "/"),
        ],
    )
    ctx.emit(
        ProjectStage.cmake_build,
        "succeeded" if status == ProjectStatus.succeeded else "failed",
        f"exit={result.exit_code}",
    )

    if status == ProjectStatus.succeeded:
        exe = _find_exe(build_dir, ctx.settings.cmake_build_config)
        if exe is not None:
            state.latest_build_exe = str(exe)
            state.latest_build_dir = str(build_dir)
            state.current_stage = ProjectStage.ready_to_launch
            state.status = ProjectStatus.succeeded
            ctx.store.save_state(state)
            ctx.emit(ProjectStage.ready_to_launch, "ready", exe.name)

    return StageResult(
        stage=ProjectStage.cmake_build,
        artifacts=[
            str(log_path.relative_to(ctx.settings.repo_root)).replace("\\", "/"),
        ],
        data=result,
    )


def _find_exe(build_dir: Path, config: str) -> Path | None:
    for candidate in (
        build_dir / "samples" / "starter-room" / config / "ptv_starter_room.exe",
        build_dir / "apps" / "ptv_game" / config / "ptv_game.exe",
    ):
        if candidate.exists():
            return candidate
    # Fallback: first .exe found.
    for exe in build_dir.rglob("*.exe"):
        return exe
    return None
