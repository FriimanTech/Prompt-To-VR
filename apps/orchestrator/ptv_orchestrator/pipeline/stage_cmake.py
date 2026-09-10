"""Stages 10/11: CMake configure + build.

Design notes
------------
* The build dir is **stable** across retries (``builds/cmake-current``) so
  resumes re-use the existing vcpkg install tree and don't redownload 22
  packages every attempt. This cuts the "time window in which we can get
  killed" from minutes to seconds on a warm cache.
* The ``cmake.log`` file is truncated at the start of every fresh configure
  so users see the current attempt, not a concatenation of every historical
  run (which previously made real errors unfindable).
* If the child cmake exits with ``STATUS_CONTROL_C_EXIT`` we retry once
  automatically because that specific exit code means the process was killed
  by an outside signal (uvicorn reload, Ctrl+C in the parent, etc.) rather
  than a real CMake error.
"""

from __future__ import annotations

import json
import re
from pathlib import Path

from ptv_schemas import ProjectStage, ProjectStatus

from ..build import configure_and_build
from ..build.cmake_runner import (
    STATUS_CONTROL_C_EXIT,
    STATUS_CONTROL_C_EXIT_SIGNED,
)
from .context import PipelineContext, StageResult


_CTRL_C_EXITS = {STATUS_CONTROL_C_EXIT, STATUS_CONTROL_C_EXIT_SIGNED}

# Characters Windows forbids in a file name, plus control chars.
_ILLEGAL_EXE_CHARS = re.compile(r'[<>:"/\\|?*\x00-\x1f]')


def _project_app_name(ctx: PipelineContext) -> str:
    """Filesystem-safe executable base name for this project.

    Uses the project's own title (the ``title`` field baked into
    ``scene.json`` by the scene-compile stage -- e.g. ``GREEN LANTERN:
    WILLPOWER VR``) so each generated game builds as ``<Title>.exe`` and is
    easy to spot in Windows / Quest Link. Falls back to the project id when no
    title is available. The returned string is stripped of characters Windows
    forbids in file names; the human-readable label shown in the headset
    "recents" list comes separately from the OpenXR application name.
    """
    title = ""
    scene = ctx.project_dir / "scene.json"
    try:
        if scene.exists():
            raw = json.loads(scene.read_text(encoding="utf-8"))
            title = str(raw.get("title") or "").strip()
    except (OSError, ValueError):
        title = ""
    if not title:
        title = ctx.project_id
    # Strip illegal characters, collapse whitespace, and trim trailing dots /
    # spaces (both invalid at the end of a Windows file name).
    name = _ILLEGAL_EXE_CHARS.sub(" ", title)
    name = re.sub(r"\s+", " ", name).strip().strip(".").strip()
    return name or ctx.project_id


def run(ctx: PipelineContext) -> StageResult:
    ctx.emit(ProjectStage.cmake_configure, "running", "Configuring CMake")
    state = ctx.store.load_state(ctx.project_id)
    ctx.store.record_stage(state, ProjectStage.cmake_configure, ProjectStatus.running)

    source_dir = ctx.settings.engine_dir
    # Stable build dir so repeated resumes reuse vcpkg + CMakeCache. This
    # avoids re-installing 22 vcpkg packages on every attempt.
    build_dir = ctx.project_dir / "builds" / "cmake-current"
    # Per-project exe name (``<Title>.exe``) so builds are identifiable.
    app_name = _project_app_name(ctx)
    log_path = ctx.project_dir / "logs" / "cmake.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)
    # Truncate the cmake log so the user is only ever looking at THIS run's
    # output (previous behaviour appended and made multi-run logs useless).
    log_path.write_text("", encoding="utf-8")

    # We track two distinct cmake.log events: the configure phase (which ends
    # at "Build files have been written to") and the build phase. A failure in
    # either must be reported on the correct stage so the user can tell whether
    # it was vcpkg / find_package / toolchain vs. a real C++ compile error.
    configure_done = {"seen": False}
    error_excerpts: list[str] = []

    def _sink(line: str) -> None:
        with log_path.open("a", encoding="utf-8") as f:
            f.write(line + "\n")
        if "Build files have been written to" in line:
            configure_done["seen"] = True
        low = line.lower()
        if (
            ": error " in low
            or low.startswith("error:")
            or low.startswith("cmake error")
            or "error c" in low
            or "fatal error" in low
        ):
            error_excerpts.append(line.strip())
        # Route log lines to the correct stage so the UI live-events column
        # doesn't falsely label everything "cmake_build".
        stage = ProjectStage.cmake_build if configure_done["seen"] else ProjectStage.cmake_configure
        ctx.emit(stage, "log", line[:512])

    def _run_once() -> object:
        return configure_and_build(
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
                ("PTV_APP_NAME", app_name),
            ),
            on_line=_sink,
            **proc_hooks(ctx),
        )

    result = _run_once()

    # STATUS_CONTROL_C_EXIT means cmake.exe was killed by an outside signal
    # (uvicorn --reload, Ctrl+C in the parent terminal, a concurrent pipeline
    # run acquiring the vcpkg lock, etc.) rather than CMake producing a real
    # error. Auto-retry once with a fresh log so the next attempt doesn't
    # carry forward misleading partial output.
    if result.exit_code in _CTRL_C_EXITS and not configure_done["seen"]:
        ctx.emit(
            ProjectStage.cmake_configure,
            "log",
            "[stage_cmake] cmake was interrupted (CTRL_C_EXIT); retrying once",
        )
        log_path.write_text("", encoding="utf-8")
        error_excerpts.clear()
        configure_done["seen"] = False
        result = _run_once()

    # A nonzero exit code before configure finished means CMake configure failed.
    # Only mark configure as succeeded if we actually observed the "Build files
    # have been written to" line.
    configure_ok = configure_done["seen"]
    build_ok = configure_ok and result.exit_code == 0
    configure_status = ProjectStatus.succeeded if configure_ok else ProjectStatus.failed
    build_status = ProjectStatus.succeeded if build_ok else ProjectStatus.failed

    first_error_line = error_excerpts[0][:200] if error_excerpts else ""
    configure_msg = f"Generator: {result.generator}"
    if not configure_ok:
        if result.exit_code in _CTRL_C_EXITS:
            configure_msg = (
                "CMake configure was interrupted (exit=STATUS_CONTROL_C_EXIT). "
                "Another pipeline run, a uvicorn --reload, or Ctrl+C in the "
                "orchestrator terminal killed cmake.exe mid-install. "
                "Click Resume to try again -- the fix in this version isolates "
                "cmake in its own process group so the next run should survive."
            )
        elif first_error_line:
            configure_msg = (
                f"CMake configure failed (exit={result.exit_code}). "
                f"First error: {first_error_line}"
            )
        else:
            configure_msg = (
                f"CMake configure failed (exit={result.exit_code}) -- no "
                "recognisable error line in log; see logs/cmake.log for raw output."
            )

    ctx.store.record_stage(
        state, ProjectStage.cmake_configure, configure_status, message=configure_msg,
    )

    build_msg = f"CMake exit={result.exit_code}"
    if configure_ok and not build_ok:
        if result.exit_code in _CTRL_C_EXITS:
            build_msg = (
                "CMake build was interrupted (exit=STATUS_CONTROL_C_EXIT). "
                "Click Resume to retry; the build dir is preserved."
            )
        elif first_error_line:
            build_msg = (
                f"CMake build failed (exit={result.exit_code}). "
                f"First error: {first_error_line}"
            )
    ctx.store.record_stage(
        state,
        ProjectStage.cmake_build,
        build_status,
        message=build_msg,
        artifacts=[
            str(log_path.relative_to(ctx.settings.repo_root)).replace("\\", "/"),
        ],
    )
    ctx.emit(
        ProjectStage.cmake_build,
        "succeeded" if build_ok else "failed",
        f"exit={result.exit_code}" + (f" :: {first_error_line}" if first_error_line else ""),
    )

    status = build_status

    if status == ProjectStatus.succeeded:
        exe = _find_exe(build_dir, ctx.settings.cmake_build_config, app_name)
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


def _find_exe(build_dir: Path, config: str, app_name: str | None = None) -> Path | None:
    """Locate the runnable VR binary inside a project's build dir.

    The starter-room CMake target is the shared runtime host for every
    project: it boots Vulkan + OpenXR and then loads whichever ``scene.json``
    is passed via argv[1] / PTV_SCENE_JSON. The project's scene path is stored
    separately in ``state.latest_scene_json`` and handed to ``launch_exe``
    at launch time, so the same exe correctly launches the right game.

    Each project overrides the exe's OUTPUT_NAME to its own title (via
    ``-DPTV_APP_NAME``), so we look for ``<app_name>.exe`` first and only fall
    back to the generic ``ptv_starter_room.exe`` for older builds.

    We prefer the ``config`` requested (Release/Debug) but fall back to the
    other config if only one was built -- Visual Studio's multi-config
    generator often ends up with just one of them populated depending on
    what the user built last.
    """
    other = "Debug" if config.lower() == "release" else "Release"
    starter = build_dir / "samples" / "starter-room"
    exe_names = []
    if app_name:
        exe_names.append(f"{app_name}.exe")
    exe_names.append("ptv_starter_room.exe")
    roots: list[Path] = []
    for name in exe_names:
        roots.append(starter / config / name)
        roots.append(starter / other / name)
    roots += [
        build_dir / "apps" / "ptv_game" / config / "ptv_game.exe",
        build_dir / "apps" / "ptv_game" / other / "ptv_game.exe",
    ]
    for candidate in roots:
        if candidate.exists():
            return candidate
    # Last resort: first .exe under the build dir that isn't a CMake
    # helper (compiler probe / tests / tools).
    skip = {"CompilerIdCXX.exe", "ptv_smoke.exe", "ptv_glb_validate.exe"}
    for exe in build_dir.rglob("*.exe"):
        if exe.name in skip:
            continue
        return exe
    return None
