"""World-generation stage: a single ChatGPT "world architect" agent authors all
the C++ for the VR environment, iterating under a compile-and-repair loop.

Flow
----
1. Build the architect (OpenAI Agents SDK, gpt-5.5 @ reasoning xhigh,
   WebSearchTool, persistent Session) and hand it the engine guide + game brief.
2. Write its bounded C++ into ``engine/generated/world/<project_id>/`` (world
   include whitelist enforced; violations are fed back to the agent).
3. Compile the offline ``ptv_worldgen`` host (engine/worldgen, standalone).
4. On compile failure, feed the compiler log back to the SAME session and let
   the agent fix it; retry up to ``world_max_repair_rounds``.
5. Run the host to emit ``world.scene.json`` and validate it (ground + spawn +
   sane entity count); refine on invalid.
6. If the budget is exhausted, emit a deterministic fallback world so the
   project still boots.

The engine core is never touched: generated code compiles into a host that
links no Vulkan/OpenXR/Jolt/renderer/ECS, and only ``ptv/api/IWorldBuilder.h``
(+ std + glm) may be included.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
from pathlib import Path
from typing import Any, Optional

from ptv_schemas import ProjectStage, ProjectStatus

from ..build.cmake_runner import configure_and_build
from .bounds import (
    GeneratedBoundsError,
    assert_world_path_allowed,
    filter_world_cpp_only,
    validate_world_header_content,
    validate_world_includes,
)
from .context import PipelineContext, StageResult
from .prompts import WORLD_BUILDER_SYSTEM
from .world_agent import (
    WorldAgentError,
    WorldAgentInput,
    WorldAgentResult,
    WorldArchitect,
    build_initial_message,
    build_repair_message,
    build_validation_repair_message,
)

_MAX_ENTITIES = 20_000


def run(ctx: PipelineContext) -> StageResult:
    """Standard stage entry point."""
    return run_with_architect(ctx, architect=None)


def run_with_architect(
    ctx: PipelineContext, architect: Optional[WorldArchitect] = None
) -> StageResult:
    """Run the world-generation stage with an optional injected architect
    (tests pass a stub so the OpenAI Agents SDK is never touched)."""

    ctx.emit(ProjectStage.world_generation, "running", "Generating VR environment C++")
    state = ctx.store.load_state(ctx.project_id)
    ctx.store.record_stage(state, ProjectStage.world_generation, ProjectStatus.running)

    repo_root: Path = ctx.settings.repo_root
    project_id = ctx.project_id
    builder_id = f"generated.world.{project_id}"
    out_dir = repo_root / "engine" / "generated" / "world" / project_id
    gen_root = repo_root / "engine" / "generated"
    world_scene_path = ctx.project_dir / "world.scene.json"

    # Persist a detailed world-generation log so the UI Logs tab (and any
    # post-mortem debugging) shows the FULL agent iteration history and
    # compiler/host output -- not just the truncated SSE excerpts. Truncated
    # once per run, like cmake.log.
    world_log = ctx.project_dir / "logs" / "world.log"
    world_log.parent.mkdir(parents=True, exist_ok=True)
    world_log.write_text("", encoding="utf-8")

    def _wlog(text: str) -> None:
        ctx.store.append_log(project_id, "world", text)

    # Bridge OPENAI_API_KEY into os.environ for the Agents SDK (BaseSettings
    # does not export it automatically).
    if ctx.settings.openai_api_key and not os.environ.get("OPENAI_API_KEY"):
        os.environ["OPENAI_API_KEY"] = ctx.settings.openai_api_key

    # Fresh output dir so stale files from a previous run never compile.
    if out_dir.exists():
        shutil.rmtree(out_dir, ignore_errors=True)
    out_dir.mkdir(parents=True, exist_ok=True)

    # Asset map (asset_id -> glb_path) for optional place_object().
    asset_map, object_assets = _load_object_assets(ctx)
    assets_json_path = ctx.project_dir / "builds" / "worldgen_assets.json"
    assets_json_path.parent.mkdir(parents=True, exist_ok=True)
    assets_json_path.write_text(json.dumps(asset_map, indent=2), encoding="utf-8")

    guide = _read_engine_guide(ctx)
    build_excerpt = _build_spec_excerpt(ctx)

    if architect is None:
        architect = WorldArchitect(
            instructions=WORLD_BUILDER_SYSTEM,
            model=ctx.settings.world_model,
            reasoning_effort=ctx.settings.world_reasoning_effort,
            max_turns=ctx.settings.world_agent_max_turns,
            session_id=f"ptv-world-{project_id}",
        )

    inp = WorldAgentInput(
        project_id=project_id,
        output_subdir=f"engine/generated/world/{project_id}",
        builder_id=builder_id,
        engine_guide=guide,
        build_spec_excerpt=build_excerpt,
        object_assets_json=json.dumps(object_assets, indent=2),
        extra_instructions=[
            f"Register with PTV_REGISTER_WORLD_BUILDER(\"{builder_id}\", ...).",
            "Only place_object() with asset ids from the available list.",
        ],
    )

    max_rounds = int(ctx.settings.world_max_repair_rounds)
    message = build_initial_message(inp)
    seed = _seed_for(project_id)

    last_error = ""
    for attempt in range(1, max_rounds + 1):
        ctx.emit(
            ProjectStage.world_generation,
            "agent_turn",
            f"architect attempt {attempt}/{max_rounds}",
        )
        _wlog(f"\n===== attempt {attempt}/{max_rounds} =====")
        try:
            result = architect.run(message)
        except WorldAgentError as exc:
            last_error = f"agent error: {exc}"
            _wlog("AGENT ERROR: " + str(exc))
            ctx.emit(ProjectStage.world_generation, "warn", last_error[:240])
            break

        if result.plan:
            _wlog("PLAN:\n" + result.plan)
        _wlog("FILES: " + ", ".join(Path(f.path).name for f in result.files))
        if result.notes:
            _wlog("NOTES:\n" + result.notes)

        # 1) Write + bounds-check the files.
        violations = _write_world_files(result, out_dir, gen_root, project_id, ctx)
        if violations:
            last_error = "bounds violations"
            _wlog("BOUNDS VIOLATIONS:\n" + "\n".join(violations))
            ctx.emit(
                ProjectStage.world_generation,
                "bounds_reject",
                f"attempt {attempt}: {len(violations)} violation(s)",
            )
            message = (
                "Your files violated the world include boundary. Fix them and "
                "return the COMPLETE corrected files[] again (same schema, same "
                "builder_id). Violations:\n" + "\n".join(violations)
            )
            continue

        # 2) Compile the offline host.
        ok, log_text = _build_worldgen(ctx)
        if not ok:
            last_error = "compile failed"
            excerpt = _error_excerpt(log_text)
            _wlog("COMPILE FAILED:\n" + log_text)
            ctx.emit(
                ProjectStage.world_generation,
                "compile_error",
                f"attempt {attempt}: {excerpt[:200]}",
            )
            message = build_repair_message(_tail(log_text, 12_000))
            continue
        _wlog("compile ok")

        # 3) Run the host and validate the emitted world.
        ran, run_log = _run_worldgen(ctx, builder_id, seed, assets_json_path,
                                     world_scene_path)
        if not ran:
            last_error = "host run failed"
            _wlog("HOST RUN FAILED:\n" + run_log)
            ctx.emit(ProjectStage.world_generation, "run_error", run_log[:200])
            message = build_repair_message(
                "The host failed to run/emit the scene:\n" + _tail(run_log, 4_000)
            )
            continue
        if run_log.strip():
            _wlog("HOST OUTPUT:\n" + run_log)

        valid, reason = _validate_world_scene(world_scene_path)
        if not valid:
            last_error = f"invalid scene: {reason}"
            _wlog("INVALID SCENE: " + reason)
            ctx.emit(ProjectStage.world_generation, "invalid_scene", reason[:200])
            message = build_validation_repair_message(reason)
            continue

        # Success.
        _wlog(f"SUCCESS in {attempt} attempt(s): {reason}")
        rel = _rel(world_scene_path, repo_root)
        ctx.store.record_stage(
            state,
            ProjectStage.world_generation,
            ProjectStatus.succeeded,
            message=f"world built by agent in {attempt} attempt(s); {reason}",
            artifacts=[rel],
        )
        ctx.emit(ProjectStage.world_generation, "succeeded", f"world.scene.json ({reason})")
        return StageResult(
            stage=ProjectStage.world_generation,
            artifacts=[rel],
            message="agent world",
        )

    # ------------------------------------------------------------------
    # Exhausted the repair budget (or agent errored): deterministic fallback
    # so the project still boots into something walkable.
    # ------------------------------------------------------------------
    _wlog(f"\n===== fallback (agent unavailable: {last_error}) =====")
    ctx.emit(
        ProjectStage.world_generation,
        "fallback",
        f"agent world unavailable ({last_error}); emitting fallback world",
    )
    fb_ok, fb_log = _emit_fallback_world(ctx, seed, world_scene_path)
    _wlog(("FALLBACK OK\n" if fb_ok else "FALLBACK FAILED\n") + fb_log)
    if not fb_ok:
        ctx.store.record_stage(
            state,
            ProjectStage.world_generation,
            ProjectStatus.failed,
            message=f"agent world failed ({last_error}) and fallback failed: "
            + _error_excerpt(fb_log)[:300],
        )
        ctx.emit(ProjectStage.world_generation, "failed", "no world produced")
        raise RuntimeError(
            f"world_generation failed: agent ({last_error}) and fallback both failed"
        )

    rel = _rel(world_scene_path, repo_root)
    ctx.store.record_stage(
        state,
        ProjectStage.world_generation,
        ProjectStatus.succeeded,
        message=f"fallback world (agent unavailable: {last_error})",
        artifacts=[rel],
    )
    ctx.emit(ProjectStage.world_generation, "succeeded", "fallback world.scene.json")
    return StageResult(
        stage=ProjectStage.world_generation,
        artifacts=[rel],
        message="fallback world",
    )


# ---------------------------------------------------------------------------
# File writing + bounds
# ---------------------------------------------------------------------------


def _write_world_files(
    result: WorldAgentResult,
    out_dir: Path,
    gen_root: Path,
    project_id: str,
    ctx: PipelineContext,
) -> list[str]:
    """Validate + write the agent's files. Returns a list of violation strings
    (empty = wrote cleanly). Does not write anything if there are violations."""
    if not result.files:
        return ["agent returned 0 files"]
    if len(result.files) > 24:
        return [f"agent returned {len(result.files)} files; max is 24"]

    prefix = f"engine/generated/world/{project_id}"
    local_headers = frozenset(
        Path(f.path).name
        for f in result.files
        if Path(f.path).suffix.lower() in (".h", ".hpp")
    )

    normalised: list[tuple[Path, str]] = []
    violations: list[str] = []
    seen: set[str] = set()

    for f in result.files:
        name = Path(f.path.replace("\\", "/")).name
        if not name or ".." in name:
            violations.append(f"illegal path {f.path!r}")
            continue
        ext = Path(name).suffix.lower()
        if ext not in (".cpp", ".h", ".hpp"):
            violations.append(f"{name}: only .cpp/.h/.hpp allowed")
            continue
        target = out_dir / name
        if str(target) in seen:
            violations.append(f"duplicate file {name}")
            continue
        seen.add(str(target))

        try:
            assert_world_path_allowed(target, gen_root)
        except GeneratedBoundsError as exc:
            violations.append(str(exc))
            continue

        code = _strip_fences(f.contents)
        violations.extend(
            f"{name}: {v}" for v in validate_world_includes(
                code, name, local_headers=local_headers
            )
        )
        if ext in (".h", ".hpp"):
            violations.extend(
                f"{name}: {v}" for v in validate_world_header_content(code, name)
            )
        code = filter_world_cpp_only(code, local_headers=local_headers)
        normalised.append((target, code))

    if violations:
        return violations

    for target, code in normalised:
        target.write_text(code.rstrip() + "\n", encoding="utf-8")
        ctx.emit(
            ProjectStage.world_generation,
            "info",
            f"wrote {target.name}",
        )
    return []


# ---------------------------------------------------------------------------
# Building + running the offline host
# ---------------------------------------------------------------------------


def _worldgen_source_dir(ctx: PipelineContext) -> Path:
    return ctx.settings.engine_dir / "worldgen"


def _build_worldgen(ctx: PipelineContext) -> tuple[bool, str]:
    """Configure + build ptv_worldgen (standalone) with this project's world
    sources. Returns (ok, combined_log)."""
    build_dir = ctx.project_dir / "builds" / "worldgen"
    logs: list[str] = []

    def _sink(line: str) -> None:
        logs.append(line)
        ctx.emit(ProjectStage.world_generation, "log", line[:400])

    result = configure_and_build(
        source_dir=_worldgen_source_dir(ctx),
        build_dir=build_dir,
        generator=ctx.settings.cmake_generator,
        platform=ctx.settings.cmake_generator_platform,
        config=ctx.settings.cmake_build_config,
        vcpkg_root=ctx.settings.vcpkg_root,
        extra_cache=(("PTV_WORLD_PROJECT_ID", ctx.project_id),),
        on_line=_sink,
    )
    return result.exit_code == 0, "\n".join(logs)


def _worldgen_exe(ctx: PipelineContext, build_dir: Path) -> Optional[Path]:
    config = ctx.settings.cmake_build_config
    other = "Debug" if config.lower() == "release" else "Release"
    for cfg in (config, other):
        cand = build_dir / cfg / "ptv_worldgen.exe"
        if cand.exists():
            return cand
    # Single-config generators put it directly in build_dir.
    cand = build_dir / "ptv_worldgen.exe"
    return cand if cand.exists() else None


def _run_worldgen(
    ctx: PipelineContext,
    builder_id: str,
    seed: int,
    assets_json_path: Path,
    out_path: Path,
    *,
    force_fallback: bool = False,
) -> tuple[bool, str]:
    """Run the host to emit world.scene.json. Returns (ok, log)."""
    build_dir = ctx.project_dir / "builds" / "worldgen"
    exe = _worldgen_exe(ctx, build_dir)
    if exe is None:
        return False, "ptv_worldgen.exe not found after build"
    args = [
        str(exe),
        "--out",
        str(out_path),
        "--seed",
        str(seed),
        "--assets",
        str(assets_json_path),
    ]
    if not force_fallback:
        args += ["--builder", builder_id]
    try:
        proc = subprocess.run(
            args, capture_output=True, text=True, timeout=120
        )
    except Exception as exc:  # pragma: no cover
        return False, f"host launch failed: {exc}"
    log = (proc.stdout or "") + (proc.stderr or "")
    return proc.returncode == 0 and out_path.exists(), log


def _emit_fallback_world(
    ctx: PipelineContext, seed: int, out_path: Path
) -> tuple[bool, str]:
    """Build a clean host WITHOUT the (possibly broken) project world sources
    and run the built-in fallback builder to emit a bootable world."""
    build_dir = ctx.project_dir / "builds" / "worldgen_fallback"
    logs: list[str] = []

    def _sink(line: str) -> None:
        logs.append(line)
        ctx.emit(ProjectStage.world_generation, "log", line[:400])

    result = configure_and_build(
        source_dir=_worldgen_source_dir(ctx),
        build_dir=build_dir,
        generator=ctx.settings.cmake_generator,
        platform=ctx.settings.cmake_generator_platform,
        config=ctx.settings.cmake_build_config,
        vcpkg_root=ctx.settings.vcpkg_root,
        extra_cache=(("PTV_WORLD_PROJECT_ID", ""),),  # no project sources
        on_line=_sink,
    )
    if result.exit_code != 0:
        return False, "\n".join(logs)

    exe = _worldgen_exe(ctx, build_dir)
    if exe is None:
        return False, "fallback ptv_worldgen.exe not found"
    try:
        proc = subprocess.run(
            [str(exe), "--out", str(out_path), "--seed", str(seed),
             "--builder", "builtin.world.fallback"],
            capture_output=True, text=True, timeout=120,
        )
    except Exception as exc:  # pragma: no cover
        return False, f"fallback host launch failed: {exc}"
    log = "\n".join(logs) + "\n" + (proc.stdout or "") + (proc.stderr or "")
    return proc.returncode == 0 and out_path.exists(), log


# ---------------------------------------------------------------------------
# Validation + context helpers
# ---------------------------------------------------------------------------


def _validate_world_scene(path: Path) -> tuple[bool, str]:
    if not path.exists():
        return False, "world.scene.json not written"
    try:
        doc = json.loads(path.read_text(encoding="utf-8"))
    except Exception as exc:
        return False, f"unparseable json: {exc}"
    entities = doc.get("entities")
    if not isinstance(entities, list) or not entities:
        return False, "no entities"
    if len(entities) > _MAX_ENTITIES:
        return False, f"too many entities ({len(entities)} > {_MAX_ENTITIES})"
    has_ground = any(
        isinstance(e, dict) and "ground" in (e.get("tags") or [])
        for e in entities
    )
    if not has_ground:
        return False, "no ground entity (tag 'ground')"
    player = doc.get("player") or {}
    spawn = player.get("spawn") if isinstance(player, dict) else None
    if not isinstance(spawn, dict) or "position" not in spawn:
        return False, "no player spawn"
    return True, f"{len(entities)} entities"


def _load_object_assets(ctx: PipelineContext) -> tuple[dict[str, str], list[dict]]:
    """Return (asset_id->glb_path map, [ {id,name,category,notes} ]) for props
    the agent MAY place. Environment assets are no longer generated as GLBs."""
    asset_map: dict[str, str] = {}
    listing: list[dict] = []
    # glb paths from the normalization report (if present).
    norm_path = ctx.project_dir / "asset_normalization.json"
    norm: dict[str, Any] = {}
    if norm_path.exists():
        try:
            norm = json.loads(norm_path.read_text(encoding="utf-8"))
        except Exception:
            norm = {}
    try:
        manifest = ctx.store.load_asset_manifest(ctx.project_id)
    except Exception:
        return asset_map, listing
    for item in manifest.items:
        cat = getattr(item.category, "value", str(item.category))
        if cat == "environment":
            continue  # environment is agent C++ now, not a GLB
        glb = ""
        info = norm.get(item.id) if isinstance(norm, dict) else None
        if isinstance(info, dict):
            glb = info.get("glb_path", "") or ""
        if glb:
            asset_map[item.id] = glb
        listing.append(
            {
                "id": item.id,
                "name": item.name,
                "category": cat,
                "notes": (item.style_notes or "")[:300],
            }
        )
    return asset_map, listing


def _read_engine_guide(ctx: PipelineContext) -> str:
    guide_path = ctx.settings.engine_dir / "world_author_guide.md"
    try:
        return guide_path.read_text(encoding="utf-8")
    except Exception:
        return "(world_author_guide.md missing; see ptv/api/IWorldBuilder.h)"


def _build_spec_excerpt(ctx: PipelineContext) -> str:
    try:
        bs = ctx.store.load_build_spec(ctx.project_id)
    except Exception:
        return "(no BuildSpec available)"
    lines = [
        f"title: {bs.title}",
        f"pitch: {bs.pitch[:800]}",
        f"game_type: {bs.game_type.value}",
        f"locomotion: {bs.locomotion.value}",
        f"player_height_meters: {bs.player_height_meters}",
    ]
    if bs.constraints:
        lines.append("constraints: " + "; ".join(bs.constraints[:20]))
    # Scene summaries carry the environment/setting brief.
    for s in bs.scenes[:6]:
        lines.append(f"scene[{s.id}]: {s.name} -- {(s.summary or '')[:600]}")
    return "\n".join(lines)


def _seed_for(project_id: str) -> int:
    h = 1469598103934665603
    for ch in project_id:
        h ^= ord(ch)
        h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h % 2_000_000_000


def _strip_fences(text: str) -> str:
    t = text.strip()
    if t.startswith("```"):
        nl = t.find("\n")
        if nl != -1:
            t = t[nl + 1 :]
        if t.endswith("```"):
            t = t[:-3]
    return t


def _tail(text: str, n: int) -> str:
    return text[-n:] if len(text) > n else text


def _error_excerpt(log_text: str) -> str:
    for line in log_text.splitlines():
        low = line.lower()
        if ": error" in low or "error c" in low or "fatal error" in low:
            return line.strip()
    return (log_text.strip().splitlines() or [""])[-1]


def _rel(path: Path, repo_root: Path) -> str:
    try:
        return str(path.relative_to(repo_root)).replace("\\", "/")
    except ValueError:
        return str(path).replace("\\", "/")


__all__ = ["run", "run_with_architect"]
