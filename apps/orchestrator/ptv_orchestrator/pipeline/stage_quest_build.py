"""Stage 10 (Quest target): build a standalone Android APK.

This is the ``target_device == quest`` counterpart to ``stage_cmake`` (which
builds the Windows ``.exe``). It:

1. instantiates the ``engine/android`` Gradle template into
   ``projects/<id>/builds/quest/`` and substitutes the ``@PTV_*@`` tokens,
2. packages the game's assets (``scene.json``, GLBs, compiled ``.spv``
   shaders) into ``app/src/main/assets/`` so they ship inside the APK,
3. runs ``gradlew assembleDebug`` (NDK + Gradle) to produce a debug APK,
4. records the APK path on ``ProjectState.latest_build_apk``.

The generated C++ mechanics and ``scene.json`` are identical to the PC VR
path; only the packaging differs. Debug APKs are auto-signed, so the launch
stage can ``adb install -r`` them directly.

NOTE: the arm64-android dependency build and native XR bring-up are expected
to need on-device iteration; see engine/android/README.md.
"""

from __future__ import annotations

import shutil
from pathlib import Path

from ptv_schemas import ProjectStage, ProjectStatus

from ..build import assemble_debug, find_debug_apk
from ..quest_ids import quest_package_name
from .context import PipelineContext, StageResult

_SHADER_EXTS = (".vert", ".frag", ".comp")
# Files in the template that carry @PTV_*@ tokens.
_TOKENISED = (
    "settings.gradle",
    "app/build.gradle",
    "app/src/main/AndroidManifest.xml",
    "app/src/main/res/values/strings.xml",
)


def _app_label(ctx: PipelineContext) -> str:
    try:
        title = ctx.store.load_state(ctx.project_id).title
    except Exception:
        title = ""
    return (title or ctx.project_id).strip() or ctx.project_id


def _substitute_tokens(build_dir: Path, tokens: dict[str, str]) -> None:
    for rel in _TOKENISED:
        p = build_dir / rel
        if not p.exists():
            continue
        text = p.read_text(encoding="utf-8")
        for key, value in tokens.items():
            text = text.replace(key, value)
        p.write_text(text, encoding="utf-8")


def _compile_shaders(engine_dir: Path, assets_dir: Path) -> list[str]:
    """Compile engine GLSL to SPIR-V into ``assets/shaders/`` via glslc.

    Returns a list of human-readable notes. If glslc is not found the shaders
    are simply not packaged (the app will fail to load them on-device -- an
    explicit on-device iteration signal rather than a silent success)."""
    import os
    import subprocess

    notes: list[str] = []
    shader_src = engine_dir / "runtime" / "shaders"
    out_dir = assets_dir / "shaders"
    out_dir.mkdir(parents=True, exist_ok=True)

    glslc = shutil.which("glslc")
    if not glslc:
        vk = os.environ.get("VULKAN_SDK")
        if vk:
            cand = Path(vk) / "Bin" / "glslc.exe"
            if cand.exists():
                glslc = str(cand)
    if not glslc:
        notes.append("glslc not found; shaders NOT packaged (set VULKAN_SDK)")
        return notes

    for ext in _SHADER_EXTS:
        for shader in shader_src.glob(f"*{ext}"):
            out = out_dir / (shader.name + ".spv")
            try:
                subprocess.run(
                    [glslc, "-O", str(shader), "-o", str(out)],
                    check=True,
                    capture_output=True,
                    text=True,
                )
            except subprocess.CalledProcessError as exc:
                notes.append(f"glslc failed for {shader.name}: {exc.stderr[:200]}")
    return notes


def _package_assets(ctx: PipelineContext, build_dir: Path) -> list[str]:
    """Copy scene.json + GLBs + shaders into the APK assets dir."""
    notes: list[str] = []
    assets_dir = build_dir / "app" / "src" / "main" / "assets"
    assets_dir.mkdir(parents=True, exist_ok=True)

    scene = ctx.project_dir / "scene.json"
    if scene.exists():
        shutil.copy2(scene, assets_dir / "scene.json")
    else:
        notes.append("scene.json missing -- run scene_compile first")

    # GLBs live under the project assets dir (normalized Tripo output).
    glb_count = 0
    proj_assets = ctx.project_dir / "assets"
    if proj_assets.exists():
        for glb in proj_assets.rglob("*.glb"):
            rel = glb.relative_to(proj_assets)
            dest = assets_dir / "assets" / rel
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(glb, dest)
            glb_count += 1
    notes.append(f"packaged {glb_count} GLB(s)")

    notes.extend(_compile_shaders(ctx.settings.engine_dir, assets_dir))
    return notes


def run(ctx: PipelineContext) -> StageResult:
    ctx.emit(ProjectStage.quest_build, "running", "Building Quest APK")
    state = ctx.store.load_state(ctx.project_id)
    ctx.store.record_stage(state, ProjectStage.quest_build, ProjectStatus.running)

    template_dir = ctx.settings.engine_dir / "android"
    if not template_dir.exists():
        msg = f"Quest template not found at {template_dir}"
        ctx.store.record_stage(
            state, ProjectStage.quest_build, ProjectStatus.failed, message=msg
        )
        ctx.emit(ProjectStage.quest_build, "failed", msg)
        raise RuntimeError(msg)

    build_dir = ctx.project_dir / "builds" / "quest"
    # Copy the template (idempotent; preserves the Gradle cache on re-runs).
    shutil.copytree(template_dir, build_dir, dirs_exist_ok=True)

    s = ctx.settings
    package = quest_package_name(s.android_package_prefix, ctx.project_id)
    tokens = {
        "@PTV_PACKAGE@": package,
        "@PTV_APP_LABEL@": _app_label(ctx),
        "@PTV_ENGINE_DIR@": str(s.engine_dir).replace("\\", "/"),
        "@PTV_PROJECT_ID@": ctx.project_id,
        "@PTV_MIN_SDK@": str(s.android_min_sdk),
        "@PTV_TARGET_SDK@": str(s.android_target_sdk),
        "@PTV_COMPILE_SDK@": str(s.android_compile_sdk),
        "@PTV_LOADER_VERSION@": s.android_openxr_loader_version,
    }
    _substitute_tokens(build_dir, tokens)

    asset_notes = _package_assets(ctx, build_dir)
    for note in asset_notes:
        ctx.emit(ProjectStage.quest_build, "info", note)

    log_path = ctx.project_dir / "logs" / "gradle.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_path.write_text("", encoding="utf-8")

    def _sink(line: str) -> None:
        with log_path.open("a", encoding="utf-8") as f:
            f.write(line + "\n")
        ctx.emit(ProjectStage.quest_build, "log", line[:512])

    result = assemble_debug(project_dir=build_dir, on_line=_sink)

    apk = find_debug_apk(build_dir)
    ok = result.exit_code == 0 and apk is not None

    state = ctx.store.load_state(ctx.project_id)
    if ok:
        state.latest_build_apk = str(apk)
        state.latest_build_dir = str(build_dir)
        ctx.store.save_state(state)
        rel = str(apk.relative_to(s.repo_root)).replace("\\", "/") if apk else ""
        ctx.store.record_stage(
            state,
            ProjectStage.quest_build,
            ProjectStatus.succeeded,
            message=f"APK: {apk.name if apk else '?'}",
            artifacts=[rel] if rel else [],
        )
        ctx.emit(ProjectStage.quest_build, "succeeded", f"APK built: {apk}")
        return StageResult(stage=ProjectStage.quest_build, artifacts=[rel] if rel else [])

    msg = (
        f"Gradle build failed (exit={result.exit_code}). See logs/gradle.log. "
        "First-time arm64-android dependency builds and native bring-up often "
        "need iteration; see engine/android/README.md."
    )
    ctx.store.record_stage(
        state, ProjectStage.quest_build, ProjectStatus.failed, message=msg
    )
    ctx.emit(ProjectStage.quest_build, "failed", msg)
    raise RuntimeError(msg)
