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

import hashlib
import re
import shutil
from pathlib import Path

from ptv_schemas import ProjectStage, ProjectStatus

from ..build import assemble_debug, find_debug_apk
from ..quest_ids import quest_package_name
from .context import PipelineContext, StageResult
from .stage_code_generation import generated_sources_cache
from .proc import proc_hooks

_SHADER_EXTS = (".vert", ".frag", ".comp")
# Files in the template that carry @PTV_*@ tokens.
_TOKENISED = (
    "settings.gradle",
    "app/build.gradle",
    "app/src/main/AndroidManifest.xml",
    "app/src/main/res/values/strings.xml",
)


def _native_staging_dir(ctx: PipelineContext) -> Path:
    """A short, per-project home for the CMake/.cxx tree.

    Windows' 260-character MAX_PATH is still enforced by ninja, and the
    default ``<project>/builds/quest/app/.cxx`` sits ~104 characters deep
    before CMake appends a FetchContent stamp path of its own (another ~145).
    Timestamped project ids leave no headroom: switching the native config
    name from ``Debug`` to ``RelWithDebInfo`` was enough to break configure.

    Keyed on a digest of the project id rather than the id itself, because the
    id is 35 characters and the whole point is to be short. Lives beside the
    engine build tree so ``git clean`` treats it like any other build output.
    """
    digest = hashlib.sha256(ctx.project_id.encode("utf-8")).hexdigest()[:8]
    return ctx.settings.repo_root / ".cxx" / digest


def _app_label(ctx: PipelineContext) -> str:
    try:
        title = ctx.store.load_state(ctx.project_id).title
    except Exception:
        title = ""
    return (title or ctx.project_id).strip() or ctx.project_id


_PROGRESS_RE = re.compile(r"^\s*\[\d+/\d+\]")


def is_compile_error_line(line: str) -> bool:
    """A compiler diagnostic that is an error (clang ``file:1:2: error:``)."""
    return ": error:" in line or ": fatal error:" in line


def is_notable_gradle_line(line: str) -> bool:
    """Lines worth streaming live: build progress, task boundaries,
    diagnostics and the final verdict."""
    s = line.strip()
    if not s:
        return False
    if _PROGRESS_RE.match(s) or s.startswith(("> Task", "BUILD ", "FAILURE", "FAILED",
                                              "* What went wrong", "Execution failed")):
        return True
    low = s.lower()
    return ": error:" in low or ": fatal error:" in low or ": warning:" in low


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


def _package_engine_assets(engine_dir: Path, assets_dir: Path) -> list[str]:
    """Build the default rigged hands into ``assets/engine/``.

    Engine-owned rather than project-owned: produced here from
    ``Default_Models/default_rigged_hand_model.glb`` by the committed
    ``prepare_default_hands.py`` (renamed to BodySlots joints, mirrored for
    the left hand), exactly as the desktop CMake build does. Matches
    ``PTV_ENGINE_ASSET_DIR="engine"`` in the Android CMakeLists. Failure is
    non-fatal: the runtime falls back to the procedural block hands.
    """
    import subprocess
    import sys

    import shutil

    notes: list[str] = []
    # The UI font (end screens, menus, HUD numerals), beside its OFL license.
    # Without it the runtime still runs, but every label is invisible.
    font_dir = engine_dir / "runtime" / "assets" / "fonts"
    out_fonts = assets_dir / "engine" / "fonts"
    if font_dir.is_dir():
        out_fonts.mkdir(parents=True, exist_ok=True)
        for f in font_dir.iterdir():
            if f.suffix.lower() in (".ttf", ".txt"):
                shutil.copy2(f, out_fonts / f.name)
    else:
        notes.append(f"{font_dir} missing; UI text will not render")

    script = engine_dir / "tools" / "prepare_default_hands.py"
    source = engine_dir.parent / "Default_Models" / "default_rigged_hand_model.glb"
    if not script.is_file():
        notes.append("prepare_default_hands.py missing; hands NOT packaged")
        return notes
    if not source.is_file():
        notes.append(f"{source} missing; default hands NOT packaged")
        return notes

    out_dir = assets_dir / "engine"
    out_dir.mkdir(parents=True, exist_ok=True)
    try:
        subprocess.run(
            [sys.executable, str(script), "--input", str(source),
             "--out-dir", str(out_dir)],
            check=True,
            capture_output=True,
            text=True,
        )
    except subprocess.CalledProcessError as exc:
        notes.append(
            f"default hand preparation failed: "
            f"{(exc.stderr or exc.stdout)[:200]}"
        )
    return notes


def _is_superseded_by_lods(glb: Path) -> bool:
    """True for a raw source GLB whose directory already holds an LOD chain.

    ``asset_optimization`` writes ``<asset_id>_lod0.glb`` .. ``_lodN.glb`` next
    to the Tripo source and repoints ``scene.json`` at LOD0, so the original is
    dead weight in the APK.
    """
    if "_lod" in glb.stem:
        return False  # this IS an LOD file
    return any(glb.parent.glob("*_lod0.glb"))


def _package_assets(ctx: PipelineContext, build_dir: Path) -> list[str]:
    """Copy scene.json + GLBs + shaders into the APK assets dir."""
    notes: list[str] = []
    assets_dir = build_dir / "app" / "src" / "main" / "assets"
    # Wipe the staged assets first so the APK is always a faithful mirror of
    # the project's CURRENT files. Without this, anything packaged by an
    # earlier run lingers forever -- e.g. the raw Tripo GLBs from before
    # asset_optimization existed would keep shipping alongside their much
    # smaller LOD replacements (141 MB of dead weight in a 123 MB APK).
    if assets_dir.exists():
        shutil.rmtree(assets_dir, ignore_errors=True)
    assets_dir.mkdir(parents=True, exist_ok=True)

    scene = ctx.project_dir / "scene.json"
    if scene.exists():
        shutil.copy2(scene, assets_dir / "scene.json")
    else:
        notes.append("scene.json missing -- run scene_compile first")

    # Every OTHER scene too. A multi-scene game resolves a portal through the
    # scene index, whose paths are relative to scene.json, so the siblings have
    # to sit beside it inside the APK. Packaging only the entry point would
    # leave a build that runs and looks right until the player walks into the
    # first doorway.
    siblings = sorted(ctx.project_dir.glob("scene.*.json"))
    for path in siblings:
        shutil.copy2(path, assets_dir / path.name)
    if siblings:
        notes.append(f"packaged {len(siblings)} additional scene document(s)")

    # GLBs live under the project assets dir. Once the asset_optimization
    # stage has produced an LOD chain for an asset we ship ONLY the LODs --
    # shipping the raw Tripo source too would multiply the APK size (a single
    # untouched Tripo GLB is ~14 MB vs ~250 KB for its LOD0) while the runtime
    # would never load it.
    glb_count = 0
    skipped_sources = 0
    proj_assets = ctx.project_dir / "assets"
    if proj_assets.exists():
        for glb in proj_assets.rglob("*.glb"):
            if _is_superseded_by_lods(glb):
                skipped_sources += 1
                continue
            rel = glb.relative_to(proj_assets)
            dest = assets_dir / "assets" / rel
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(glb, dest)
            glb_count += 1
    notes.append(f"packaged {glb_count} GLB(s)")
    if skipped_sources:
        notes.append(
            f"skipped {skipped_sources} un-decimated source GLB(s) superseded by LODs"
        )

    notes.extend(_compile_shaders(ctx.settings.engine_dir, assets_dir))
    notes.extend(_package_engine_assets(ctx.settings.engine_dir, assets_dir))
    return notes


_NPC_TAGS = {"npc", "enemy", "ally", "neutral", "critter", "boss", "template"}


def missing_packaged_meshes(
    assets_dir: Path, project_id: str
) -> tuple[list[str], list[str]]:
    """GLBs the packaged scenes reference that are not in the APK assets.

    Returns ``(npc_missing, other_missing)`` as ``"<scene>/<entity>: <path>"``.
    Inside the APK a ``projects/<id>/assets/<rest>`` path is read from
    ``assets/<rest>`` (AndroidAssetSource strips the project prefix), so that
    is where each one has to be. An NPC is anything with an NPC tag or a
    skinned mesh -- the characters whose absence is the bug this stops."""
    import json

    prefix = f"projects/{project_id}/"
    npc_missing: list[str] = []
    other_missing: list[str] = []
    for scene in sorted(assets_dir.glob("scene*.json")):
        try:
            doc = json.loads(scene.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            continue
        for e in doc.get("entities") or []:
            mesh = e.get("mesh") if isinstance(e, dict) else None
            if not isinstance(mesh, dict):
                continue
            paths = [mesh.get("glb_path")] + list(mesh.get("lod_paths") or [])
            is_npc = bool(mesh.get("skinned")) or bool(
                _NPC_TAGS & set(e.get("tags") or []))
            for p in dict.fromkeys(x for x in paths if isinstance(x, str) and x):
                rel = p.replace("\\", "/")
                if rel.startswith(prefix):
                    rel = rel[len(prefix):]
                if (assets_dir / rel).is_file():
                    continue
                entry = f"{scene.name}/{e.get('id', '?')}: {p}"
                (npc_missing if is_npc else other_missing).append(entry)
    return npc_missing, other_missing


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
    cxx_dir = _native_staging_dir(ctx)
    cxx_dir.mkdir(parents=True, exist_ok=True)
    tokens = {
        "@PTV_CXX_DIR@": str(cxx_dir).replace("\\", "/"),
        "@PTV_PACKAGE@": package,
        "@PTV_APP_LABEL@": _app_label(ctx),
        "@PTV_ENGINE_DIR@": str(s.engine_dir).replace("\\", "/"),
        "@PTV_PROJECT_ID@": ctx.project_id,
        "@PTV_GENERATED_EXTRA_SOURCES@": dict(generated_sources_cache(ctx))[
            "PTV_GENERATED_EXTRA_SOURCES"],
        "@PTV_MIN_SDK@": str(s.android_min_sdk),
        "@PTV_TARGET_SDK@": str(s.android_target_sdk),
        "@PTV_COMPILE_SDK@": str(s.android_compile_sdk),
        "@PTV_LOADER_VERSION@": s.android_openxr_loader_version,
        "@PTV_NDK_VERSION@": s.android_ndk_version,
    }
    _substitute_tokens(build_dir, tokens)

    asset_notes = _package_assets(ctx, build_dir)
    for note in asset_notes:
        ctx.emit(ProjectStage.quest_build, "info", note)

    # Before spending minutes in Gradle: is every model the scenes draw
    # actually in the APK? A missing character GLB loads as nothing at all.
    npc_missing, other_missing = missing_packaged_meshes(
        build_dir / "app" / "src" / "main" / "assets", ctx.project_id)
    for entry in other_missing:
        ctx.emit(ProjectStage.quest_build, "warning", f"not packaged: {entry}")
    if npc_missing:
        msg = (
            f"{len(npc_missing)} NPC model file(s) the scene uses are not in "
            "the APK, so those characters would be invisible: "
            + "; ".join(npc_missing[:8])
            + ". Re-run asset_optimization and scene_compile."
        )
        ctx.store.record_stage(
            state, ProjectStage.quest_build, ProjectStatus.failed, message=msg
        )
        ctx.emit(ProjectStage.quest_build, "failed", msg)
        raise RuntimeError(msg)

    log_path = ctx.project_dir / "logs" / "gradle.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_path.write_text("", encoding="utf-8")

    first_errors: list[str] = []

    def _sink(line: str) -> None:
        # Everything goes to gradle.log; the live view gets what a person
        # watching wants (progress, warnings, errors). Streaming every line of
        # a native build is thousands of events for no reader.
        with log_path.open("a", encoding="utf-8") as f:
            f.write(line + "\n")
        if is_compile_error_line(line) and len(first_errors) < 6:
            text = line.strip().removeprefix("C/C++:").strip()
            if text not in first_errors:
                first_errors.append(text)
        if is_notable_gradle_line(line):
            ctx.emit(ProjectStage.quest_build, "log", line[:512])

    result = assemble_debug(
        project_dir=build_dir, on_line=_sink, **proc_hooks(ctx)
    )

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

    if first_errors:
        msg = (
            f"Gradle build failed (exit={result.exit_code}): the Quest compiler "
            f"rejected the C++. First errors: {' | '.join(first_errors[:3])}. "
            "See logs/gradle.log."
        )
    else:
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
