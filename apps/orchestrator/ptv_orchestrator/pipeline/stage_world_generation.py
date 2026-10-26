"""World-generation stage: a single ChatGPT "world architect" agent authors all
the C++ for the VR environment, iterating under a compile-and-repair loop.

Flow
----
1. Build the architect (OpenAI Agents SDK, gpt-5.6-sol @ reasoning high,
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
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
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
from .proc import proc_hooks, run_tracked
from .prompts import world_builder_system
from .world_agent import (
    WorldAgentError,
    WorldAgentInput,
    WorldAgentOutputError,
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


@dataclass(frozen=True)
class EnvironmentJob:
    """One environment to generate.

    Derived from ``BuildSpec.scenes``: the scene list IS the environment list,
    minus any scene that borrows a sibling's environment rather than getting
    its own.
    """

    scene_id: str
    name: str
    summary: str
    kind: str
    nested: bool
    host_asset_id: str
    host_anchor: str
    fit_extent_m: float
    authoring_scale_m: float
    links: tuple[tuple[str, str, str], ...]  # (to_scene, kind, spawn_point_id)
    entry_point: bool


@dataclass
class EnvironmentResult:
    job: EnvironmentJob
    ok: bool
    used_fallback: bool
    attempts: int
    detail: str
    artifact: str = ""


def _environment_jobs(ctx: PipelineContext) -> list[EnvironmentJob]:
    """Which environments this project needs, in scene order.

    A scene naming ``reuse_environment_from`` is skipped: it shares a
    sibling's environment, and generating a duplicate would pay for a second
    xhigh-reasoning agent run to produce the same place twice.
    """

    try:
        build_spec = ctx.store.load_build_spec(ctx.project_id)
    except Exception:
        return []

    cap = int(getattr(ctx.settings, "world_max_environments", 12))
    jobs: list[EnvironmentJob] = []
    for scene in build_spec.scenes:
        if getattr(scene, "reuse_environment_from", None):
            continue
        if not getattr(scene, "needs_unique_environment", True):
            continue
        host = getattr(scene, "host", None)
        mode = getattr(getattr(scene, "environment_mode", None), "value", "")
        jobs.append(
            EnvironmentJob(
                scene_id=scene.id,
                name=scene.name,
                summary=(scene.summary or "")[:2000],
                kind=getattr(getattr(scene, "kind", None), "value", "standalone"),
                nested=(mode == "nested"),
                host_asset_id=getattr(host, "asset_id", "") if host else "",
                host_anchor=getattr(host, "anchor_slot", "head") if host else "",
                fit_extent_m=(
                    getattr(host, "fit_to_host_extent_m", 0.22) if host else 0.0
                ),
                authoring_scale_m=float(
                    getattr(scene, "authoring_scale_m", 0.0) or 0.0
                ),
                links=tuple(
                    (
                        link.to_scene,
                        getattr(getattr(link, "kind", None), "value", "portal"),
                        link.spawn_point_id or "",
                    )
                    for link in (getattr(scene, "links", None) or [])
                ),
                entry_point=bool(getattr(scene, "entry_point", False)),
            )
        )

    if len(jobs) > cap:
        ctx.emit(
            ProjectStage.world_generation,
            "warning",
            f"{len(jobs)} environments requested; capping at {cap}. Each one "
            "is a full agent run with its own compile loop.",
        )
        # Keep the entry point whatever happens -- a project that drops its
        # first scene has nowhere to start.
        entry = [j for j in jobs if j.entry_point]
        rest = [j for j in jobs if not j.entry_point]
        jobs = (entry + rest)[:cap]
    return jobs


def _env_brief(job: EnvironmentJob, total: int) -> list[str]:
    """The per-environment half of the architect's brief.

    Every environment gets its whole contract UP FRONT, because the agents do
    not talk to each other. That is deliberate: a shared, deterministic brief
    cannot disagree with itself, whereas negotiation between parallel agents
    can. Cross-environment consistency is then checked deterministically
    afterwards rather than coordinated during.
    """

    lines = [
        f"You are authoring ONE environment of {total} in this game: "
        f"'{job.scene_id}' ({job.name}).",
        f"Its role in the game is: {job.kind}.",
        "Author ONLY this environment. Do NOT reference another "
        "environment's nav regions, ids, or geometry -- you cannot see them "
        "and they cannot see you.",
    ]
    if job.summary:
        lines.append(f"What this place is: {job.summary}")
    if job.authoring_scale_m > 0.0:
        lines.append(
            f"Author it at roughly {job.authoring_scale_m:.0f} m across."
        )
    if job.nested:
        lines.append(
            "This environment is NESTED inside another character's "
            f"'{job.host_anchor}' (asset '{job.host_asset_id}'). Author it at "
            "HUMAN scale as a real walkable level -- the engine scales the "
            f"whole thing down to fit {job.fit_extent_m:.2f} m afterwards. Do "
            "not try to model it at that size yourself; a level authored "
            "inside a 0.22 m box produces unusable geometry and nav regions. "
            "Include the openings the design calls for (eye sockets, viewing "
            "ports) as real holes in the shell, so looking through them shows "
            "the outside world."
        )
    if job.links:
        lines.append(
            "This environment has exits. Declare a portal for each one the "
            "player physically walks through, and a named spawn_point for "
            "each arrival:"
        )
        for to_scene, link_kind, spawn in job.links:
            spawn_note = f", arriving at spawn '{spawn}'" if spawn else ""
            lines.append(f"  - to '{to_scene}' via {link_kind}{spawn_note}")
        lines.append(
            "A 'menu_select' exit needs NO portal geometry -- it is chosen "
            "from an interface -- but the TARGET still needs the named spawn "
            "point to arrive at."
        )
    return lines


def _npc_brief(ctx: PipelineContext) -> list[str]:
    """Tell the architect that NPCs arrive during play, so it declares the
    entrances the encounter director spawns them through."""

    try:
        plan = ctx.npc_plan or ctx.store.load_npc_plan(ctx.project_id)
    except Exception:  # noqa: BLE001 - an older project may have no plan
        plan = None
    if plan is None or not plan.npcs:
        return []
    encounters = getattr(plan, "encounters", None)
    spawned = set(plan.spawned_asset_ids())
    arriving = [
        n.asset_id for n in plan.npcs
        if n.asset_id in spawned or (
            encounters is None and n.role.value in ("enemy", "boss", "critter")
            and not getattr(n, "persistent_count", 0))
    ]
    lines: list[str] = []
    if arriving:
        lines.append(
            "NPCs ARRIVE DURING PLAY in this game "
            f"({', '.join(arriving)}). Declare at least 3 npc_spawn_zone "
            "entrances spread around the level, each 12 m or more from the "
            "player spawn, on walkable floor inside a nav_region.")
    placed = [n for n in plan.npcs if getattr(n, "persistent_count", 0)]
    companions = [n.asset_id for n in placed
                  if n.role.value == "ally" and n.movement_style.value != "stationary"]
    posted = [n for n in placed if n.asset_id not in companions]
    if companions:
        lines.append(
            "Companions start beside the player "
            f"({', '.join(companions)}): keep 3 m around the player spawn "
            "clear and walkable.")
    if posted:
        lines.append(
            "These NPCs are PLACED AT LOAD and stay where they start: "
            + "; ".join(
                f"{n.asset_id} ({n.role.value}, {n.movement_style.value}, "
                f"x{n.persistent_count})" for n in posted)
            + ". Give each copy a post with w.npc_anchor(ptv::world::"
            "NpcAnchorDesc{id, position, facing, asset_id, tags}) -- set "
            "asset_id to that NPC's id -- somewhere it belongs (a guard at "
            "the gate, a vendor behind the counter), ON walkable floor inside "
            "a nav_region and facing where it should look. Without an anchor "
            "it starts beside the player.")
    return lines


def _objective_brief(ctx: PipelineContext) -> list[str]:
    """Places the game design's objectives need the world to declare.

    A ``reach_location`` objective is checked by builtin.game_flow against a
    NAMED spawn point, and scene_compile fails the build if the world never
    declared it -- so the architect is told the exact ids up front."""
    try:
        design = ctx.game_design or ctx.store.load_game_design(ctx.project_id)
    except Exception:  # noqa: BLE001 - an unreadable design is not the world's problem
        return []
    if design is None:
        return []
    wanted = [(o.id, t, o.description) for o in design.objectives
              if o.kind.value == "reach_location" for t in o.target_ids]
    if not wanted:
        return []
    return [
        "OBJECTIVE DESTINATIONS (required): declare w.spawn_point with EXACTLY "
        "these ids, on walkable floor inside a nav_region, at the place each "
        "objective sends the player -- the game is won by reaching them: "
        + "; ".join(f"'{t}' for objective '{oid}' ({desc})" for oid, t, desc in wanted)
    ]


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
    gen_root = repo_root / "engine" / "generated"
    project_world_root = gen_root / "world" / project_id

    # Which environments this game needs. One per scene that owns its own
    # place; the scene list IS the environment list.
    jobs = _environment_jobs(ctx)
    multi_env = len(jobs) > 1

    # Single-environment projects keep the historical layout exactly, so
    # nothing about an existing project changes: one directory, one builder
    # id, one world.scene.json.
    def _env_paths(job: Optional[EnvironmentJob]) -> tuple[str, Path, Path, str]:
        """(builder_id, out_dir, scene_path, env_subdir) for one environment."""
        if job is None or not multi_env:
            return (
                f"generated.world.{project_id}",
                project_world_root,
                ctx.project_dir / "world.scene.json",
                "",
            )
        return (
            f"generated.world.{project_id}.{job.scene_id}",
            project_world_root / job.scene_id,
            ctx.project_dir / f"world.{job.scene_id}.scene.json",
            job.scene_id,
        )

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

    # Clear the whole project's world tree once, up front. Per-environment
    # cleanup alone would leave an environment that a re-planned BuildSpec no
    # longer has, and its stale .cpp would still be swept up by the glob.
    if project_world_root.exists():
        shutil.rmtree(project_world_root, ignore_errors=True)
    project_world_root.mkdir(parents=True, exist_ok=True)

    # Asset map (asset_id -> glb_path) for optional place_object().
    asset_map, object_assets = _load_object_assets(ctx)
    assets_json_path = ctx.project_dir / "builds" / "worldgen_assets.json"
    assets_json_path.parent.mkdir(parents=True, exist_ok=True)
    assets_json_path.write_text(json.dumps(asset_map, indent=2), encoding="utf-8")

    guide = _read_engine_guide(ctx)
    build_excerpt = _build_spec_excerpt(ctx)

    # Cross-scene structure is checked BEFORE any agent runs, because these
    # problems are the planner's, not the architect's: a link to a scene that
    # does not exist or an unreachable mission cannot be fixed by writing
    # better C++. Reporting them up front also avoids paying for an
    # environment that was never going to be reachable.
    try:
        graph_problems = validate_environment_graph(
            ctx.store.load_build_spec(ctx.project_id)
        )
    except Exception:
        graph_problems = []
    for problem in graph_problems:
        ctx.emit(ProjectStage.world_generation, "warning", problem[:200])
        ctx.store.append_log(ctx.project_id, "world", f"graph: {problem}")

    max_rounds = int(ctx.settings.world_max_repair_rounds)

    def _generate_one(job: Optional[EnvironmentJob]) -> EnvironmentResult:
        """Author, compile, run and validate ONE environment.

        Everything an environment touches is its own: directory, builder id,
        namespace, build tree, agent session and repair budget. That isolation
        is what makes running several at once safe -- a syntax error in the
        head interior must not fail the main world's compile and spend its
        repair rounds.
        """

        env_builder_id, env_out_dir, env_scene_path, env_subdir = _env_paths(job)
        label = job.scene_id if job is not None else "world"

        def _log(text: str) -> None:
            ctx.store.append_log(project_id, "world", f"[{label}] {text}")

        # Fresh output dir so stale files from a previous run never compile.
        if env_out_dir.exists():
            shutil.rmtree(env_out_dir, ignore_errors=True)
        env_out_dir.mkdir(parents=True, exist_ok=True)

        env_architect = architect
        if env_architect is None:
            env_architect = WorldArchitect(
                instructions=world_builder_system(state.target_device),
                model=ctx.settings.world_model,
                reasoning_effort=ctx.settings.world_reasoning_effort,
                verbosity=ctx.settings.world_verbosity,
                max_turns=ctx.settings.world_agent_max_turns,
                # Per-environment session: the agents share no history, which
                # is the point. They coordinate through a deterministic brief,
                # not by talking.
                session_id=f"ptv-world-{project_id}-{label}",
                log=_log,
            )

        extra = [
            f'Register with PTV_REGISTER_WORLD_BUILDER("{env_builder_id}", ...).',
            "Only place_object() with asset ids from the available list.",
        ]
        if env_subdir:
            extra.append(
                "Wrap ALL of your code in "
                f"`namespace ptv_world_{_ns_token(env_subdir)} {{ ... }}` "
                "inside namespace ptv::world. Several environments are built "
                "for this game and identical helper names at namespace scope "
                "would collide."
            )
        if job is not None:
            extra.extend(_env_brief(job, len(jobs)))
        extra.extend(_npc_brief(ctx))
        extra.extend(_objective_brief(ctx))

        inp = WorldAgentInput(
            project_id=project_id,
            output_subdir=(
                f"engine/generated/world/{project_id}"
                + (f"/{env_subdir}" if env_subdir else "")
            ),
            builder_id=env_builder_id,
            engine_guide=guide,
            build_spec_excerpt=build_excerpt,
            object_assets_json=json.dumps(object_assets, indent=2),
            extra_instructions=extra,
        )

        message = build_initial_message(inp)
        # Seeded per environment, so two places in one game do not come out
        # with identical procedural variation.
        seed = _seed_for(f"{project_id}/{label}")
        last_error = ""

        for attempt in range(1, max_rounds + 1):
            ctx.raise_if_cancelled()
            ctx.emit(
                ProjectStage.world_generation,
                "agent_turn",
                f"{label}: architect attempt {attempt}/{max_rounds}",
            )
            _log(f"\n===== attempt {attempt}/{max_rounds} =====")
            try:
                result = env_architect.run(message)
            except WorldAgentOutputError as exc:
                # The model answered, it just did not match the schema. The
                # session still remembers the files it wrote, so spending
                # another round naming the bad field is worth far more than
                # dropping this environment to the fallback world.
                last_error = f"output error: {exc}"
                _log("OUTPUT ERROR: " + str(exc))
                ctx.emit(
                    ProjectStage.world_generation,
                    "warn",
                    f"{label}: {last_error}"[:240],
                )
                bullets = "\n".join(f"- {i}" for i in exc.issues) or "- (unspecified)"
                message = (
                    "Your answer did not match the WorldAgentResult schema. "
                    "Fix ALL of these and return the COMPLETE result again "
                    "(same builder_id, full files[]):\n"
                    f"{bullets}"
                    if not exc.truncated
                    else (
                        "Your answer was CUT OFF before the JSON was "
                        "complete: it exceeded the output token limit. "
                        "Return the same world with fewer, shorter files."
                    )
                )
                continue
            except WorldAgentError as exc:
                last_error = f"agent error: {exc}"
                _log("AGENT ERROR: " + str(exc))
                ctx.emit(
                    ProjectStage.world_generation,
                    "warn",
                    f"{label}: {last_error}"[:240],
                )
                break

            if result.plan:
                _log("PLAN:\n" + result.plan)
            _log("FILES: " + ", ".join(Path(f.path).name for f in result.files))
            if result.notes:
                _log("NOTES:\n" + result.notes)

            # 1) Write + bounds-check the files.
            violations = _write_world_files(
                result, env_out_dir, gen_root, project_id, ctx
            )
            if violations:
                last_error = "bounds violations"
                _log("BOUNDS VIOLATIONS:\n" + "\n".join(violations))
                ctx.emit(
                    ProjectStage.world_generation,
                    "bounds_reject",
                    f"{label} attempt {attempt}: {len(violations)} violation(s)",
                )
                message = (
                    "Your files violated the world include boundary. Fix them "
                    "and return the COMPLETE corrected files[] again (same "
                    "schema, same builder_id). Violations:\n"
                    + "\n".join(violations)
                )
                continue

            # 2) Compile this environment's own host.
            ok, log_text = _build_worldgen(ctx, env_subdir)
            if not ok:
                last_error = "compile failed"
                _log("COMPILE FAILED:\n" + log_text)
                ctx.emit(
                    ProjectStage.world_generation,
                    "compile_error",
                    f"{label} attempt {attempt}: "
                    f"{_error_excerpt(log_text)[:200]}",
                )
                message = build_repair_message(_tail(log_text, 12_000))
                continue
            _log("compile ok")

            # 3) Run the host and validate the emitted world.
            ran, run_log = _run_worldgen(
                ctx, env_builder_id, seed, assets_json_path, env_scene_path,
                env_subdir,
            )
            if not ran:
                last_error = "host run failed"
                _log("HOST RUN FAILED:\n" + run_log)
                ctx.emit(
                    ProjectStage.world_generation,
                    "run_error",
                    f"{label}: {run_log[:200]}",
                )
                message = build_repair_message(
                    "The host failed to run/emit the scene:\n"
                    + _tail(run_log, 4_000)
                )
                continue
            if run_log.strip():
                _log("HOST OUTPUT:\n" + run_log)

            valid, reason = _validate_world_scene(env_scene_path)
            if not valid:
                last_error = f"invalid scene: {reason}"
                _log("INVALID SCENE: " + reason)
                ctx.emit(
                    ProjectStage.world_generation,
                    "invalid_scene",
                    f"{label}: {reason}"[:200],
                )
                message = build_validation_repair_message(reason)
                continue

            _log(f"SUCCESS in {attempt} attempt(s): {reason}")
            return EnvironmentResult(
                job=job or _placeholder_job(),
                ok=True,
                used_fallback=False,
                attempts=attempt,
                detail=reason,
                artifact=_rel(env_scene_path, repo_root),
            )

        # Exhausted this environment's budget. Fall back for THIS environment
        # only: one bad place must not take a whole game down.
        _log(f"\n===== fallback (agent unavailable: {last_error}) =====")
        ctx.emit(
            ProjectStage.world_generation,
            "fallback",
            f"{label}: agent world unavailable ({last_error}); "
            "emitting fallback world",
        )
        fb_ok, fb_log = _emit_fallback_world(ctx, seed, env_scene_path)
        _log(("FALLBACK OK\n" if fb_ok else "FALLBACK FAILED\n") + fb_log)
        return EnvironmentResult(
            job=job or _placeholder_job(),
            ok=fb_ok,
            used_fallback=True,
            attempts=max_rounds,
            detail=last_error if fb_ok else _error_excerpt(fb_log)[:300],
            artifact=_rel(env_scene_path, repo_root) if fb_ok else "",
        )

    # ------------------------------------------------------------------
    # Drive it. One environment keeps the historical serial path exactly; a
    # multi-environment game fans out, mirroring how scene_spec already
    # parallelizes its per-scene agents.
    # ------------------------------------------------------------------
    results: list[EnvironmentResult] = []
    if not multi_env:
        results.append(_generate_one(jobs[0] if jobs else None))
    else:
        workers = max(
            1,
            min(int(ctx.settings.world_max_parallel_workers), len(jobs)),
        )
        ctx.emit(
            ProjectStage.world_generation,
            "info",
            f"generating {len(jobs)} environments with {workers} "
            "parallel architect(s)",
        )
        ordered: list[Optional[EnvironmentResult]] = [None] * len(jobs)
        with ThreadPoolExecutor(max_workers=workers) as pool:
            futures = {
                pool.submit(_generate_one, job): i
                for i, job in enumerate(jobs)
            }
            for future in futures:
                ordered[futures[future]] = future.result()
        results = [r for r in ordered if r is not None]

    artifacts = [r.artifact for r in results if r.artifact]
    failed = [r for r in results if not r.ok]
    fell_back = [r for r in results if r.used_fallback and r.ok]

    if not artifacts:
        detail = "; ".join(r.detail for r in failed) or "no world produced"
        ctx.store.record_stage(
            state,
            ProjectStage.world_generation,
            ProjectStatus.failed,
            message=f"no environment could be produced: {detail}"[:400],
        )
        ctx.emit(ProjectStage.world_generation, "failed", "no world produced")
        raise RuntimeError(f"world_generation failed: {detail}")

    # A single environment falling back is survivable; the project still
    # boots, and the message says which one so it can be regenerated alone.
    summary_parts = [f"{len(artifacts)} environment(s)"]
    if fell_back:
        summary_parts.append(
            "fallback for " + ", ".join(r.job.scene_id for r in fell_back)
        )
    if failed:
        summary_parts.append(
            "FAILED for " + ", ".join(r.job.scene_id for r in failed)
        )
    summary = "; ".join(summary_parts)

    _wlog(f"\n===== stage summary: {summary} =====")
    ctx.store.record_stage(
        state,
        ProjectStage.world_generation,
        ProjectStatus.succeeded,
        message=summary[:400],
        artifacts=artifacts,
    )
    ctx.emit(ProjectStage.world_generation, "succeeded", summary)
    return StageResult(
        stage=ProjectStage.world_generation,
        artifacts=artifacts,
        message=summary,
    )


def _placeholder_job() -> EnvironmentJob:
    """Stand-in for the single-environment path, which has no scene entry."""
    return EnvironmentJob(
        scene_id="world",
        name="World",
        summary="",
        kind="standalone",
        nested=False,
        host_asset_id="",
        host_anchor="",
        fit_extent_m=0.0,
        authoring_scale_m=0.0,
        links=(),
        entry_point=True,
    )


def _ns_token(env_id: str) -> str:
    """A C++-safe namespace fragment from a scene id."""
    return "".join(c if c.isalnum() else "_" for c in env_id).strip("_") or "env"


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


def _build_worldgen(
    ctx: PipelineContext, env_subdir: str = ""
) -> tuple[bool, str]:
    """Configure + build ptv_worldgen (standalone) with ONE environment's
    world sources. Returns (ok, combined_log).

    Each environment gets its own build tree and its own narrowed source glob.
    That is affordable precisely because this host links almost nothing --
    nlohmann_json plus optional glm, with no Vulkan, OpenXR, Jolt, renderer or
    ECS -- so N configure-and-builds in parallel are cheap, and it is what
    keeps one environment's compile error out of another's repair budget.
    """
    build_dir = _worldgen_build_dir(ctx, env_subdir)
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
        extra_cache=(
            ("PTV_WORLD_PROJECT_ID", ctx.project_id),
            # Empty narrows to nothing, which is the single-environment
            # layout; a scene id narrows the glob to that one subdirectory.
            ("PTV_WORLD_ENV_ID", env_subdir),
        ),
        on_line=_sink,
        **proc_hooks(ctx),
    )
    return result.exit_code == 0, "\n".join(logs)


def _worldgen_build_dir(ctx: PipelineContext, env_subdir: str = "") -> Path:
    """Build tree for one environment's host.

    Separate per environment so two architects compiling at the same time do
    not overwrite each other's object files and binaries.
    """
    base = ctx.project_dir / "builds" / "worldgen"
    return base / env_subdir if env_subdir else base


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
    env_subdir: str = "",
    *,
    force_fallback: bool = False,
) -> tuple[bool, str]:
    """Run the host to emit one environment's scene json. Returns (ok, log)."""
    build_dir = _worldgen_build_dir(ctx, env_subdir)
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
        proc = run_tracked(ctx, args, timeout=120)
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
        **proc_hooks(ctx),
    )
    if result.exit_code != 0:
        return False, "\n".join(logs)

    exe = _worldgen_exe(ctx, build_dir)
    if exe is None:
        return False, "fallback ptv_worldgen.exe not found"
    try:
        proc = run_tracked(
            ctx,
            [str(exe), "--out", str(out_path), "--seed", str(seed),
             "--builder", "builtin.world.fallback"],
            timeout=120,
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

    id_problems = _validate_entity_ids(doc)
    if id_problems:
        return False, "ids: " + "; ".join(id_problems[:4])

    nav_problems = _validate_navigation(doc)
    if nav_problems:
        return False, "navigation: " + "; ".join(nav_problems[:4])

    interactive_problems = _validate_interactives(doc)
    if interactive_problems:
        return False, "interactives: " + "; ".join(interactive_problems[:4])

    portal_problems = _validate_portals(doc)
    if portal_problems:
        return False, "portals: " + "; ".join(portal_problems[:4])

    barrier_problems = _validate_barriers(doc)
    if barrier_problems:
        return False, "barriers: " + "; ".join(barrier_problems[:4])

    nav = doc.get("navigation") or {}
    regions = nav.get("regions") or []
    nav_note = f", {len(regions)} nav regions" if regions else ""
    n_interactive = len(doc.get("interactives") or [])
    int_note = f", {n_interactive} interactives" if n_interactive else ""
    return True, f"{len(entities)} entities{nav_note}{int_note}"


# Every affordance kind the engine can bind. Kept as a literal rather than
# imported from the scene-compile stage so world generation does not depend on
# a later stage; the shared test asserts the two agree.
_AFFORDANCE_KINDS = frozenset(
    {
        "door",
        "switchable",
        "container",
        "mover",
        "breakable",
        "harvestable",
        "movable",
    }
)

# Ceiling on declared interactives in one environment. Mirrors
# Settings.world_max_interactives_per_env; duplicated here so the pure
# validator stays callable without a settings object.
_MAX_INTERACTIVES = 512


_BARRIER_WORDS = ("wall", "barrier", "fence", "partition", "boundary")
# Names that contain a barrier word but are dressing ON a wall, not the wall.
_BARRIER_DRESSING_WORDS = (
    "trim", "sign", "art", "poster", "stripe", "decal", "light", "lamp",
    "panel", "shelf", "hook", "clock", "screen", "window", "vent",
)
_MIN_BARRIER_THICKNESS_M = 0.2
_BARRIER_MIN_HEIGHT_M = 1.5
_BARRIER_MIN_LENGTH_M = 1.5


def _validate_barriers(doc: dict) -> list[str]:
    """Walls must stop the player from every side.

    A barrier (a floor-standing box named or tagged wall / barrier / fence /
    partition) needs collision and enough thickness that a head or a fast
    slide cannot tunnel through it. ``walkthrough`` in its tags is the
    prompt-requested exception (a holographic wall, a curtain).
    """
    problems: list[str] = []
    for e in doc.get("entities") or []:
        if not isinstance(e, dict):
            continue
        prim = e.get("primitive") or {}
        if prim.get("kind") != "box":
            continue
        tags = [str(t).lower() for t in (e.get("tags") or [])]
        label = f"{e.get('id', '')} {e.get('name', '')}".lower()
        if "walkthrough" in tags:
            continue
        named = any(w in label for w in _BARRIER_WORDS) or any(
            t in _BARRIER_WORDS for t in tags
        )
        if not named or any(w in label for w in _BARRIER_DRESSING_WORDS):
            continue
        size = prim.get("size") or []
        if len(size) < 3:
            continue
        try:
            scale = float((e.get("transform") or {}).get("scale") or 1.0)
            sx, sy, sz = (float(v) * scale for v in size[:3])
            y = float(((e.get("transform") or {}).get("position") or [0, 0, 0])[1])
        except (TypeError, ValueError):
            continue
        if sy < _BARRIER_MIN_HEIGHT_M or max(sx, sz) < _BARRIER_MIN_LENGTH_M:
            continue
        if y - sy * 0.5 > 0.5:
            continue  # not standing on the floor: a header, a beam
        physics = e.get("physics")
        if not isinstance(physics, dict) or physics.get("collision_shape") in (
            None, "", "none"
        ):
            problems.append(
                f"'{e.get('id')}' is a wall with no collision; give it"
                " Collision::Box (tag it 'walkthrough' only if the prompt"
                " asks for a wall the player can pass through)"
            )
            continue
        thickness = min(sx, sz)
        if thickness < _MIN_BARRIER_THICKNESS_M:
            problems.append(
                f"'{e.get('id')}' is {thickness:.2f} m thick; walls must be at"
                f" least 0.3 m (the player can push through anything under"
                f" {_MIN_BARRIER_THICKNESS_M} m)"
            )
    return problems


def _validate_entity_ids(doc: dict) -> list[str]:
    """Entity ids must exist and be unique.

    These ids are the keys saved game state is written under, so a duplicate
    does not merely look untidy -- two objects share one save slot and each
    overwrites the other, restoring a state that never existed. A blank id is
    worse: every unnamed object collides.
    """

    problems: list[str] = []
    seen: dict[str, int] = {}
    blank = 0
    for entity in doc.get("entities") or []:
        if not isinstance(entity, dict):
            continue
        ident = str(entity.get("id") or "")
        if not ident:
            blank += 1
            continue
        seen[ident] = seen.get(ident, 0) + 1
    if blank:
        problems.append(f"{blank} entity(ies) have no id")
    duplicates = sorted(k for k, n in seen.items() if n > 1)
    if duplicates:
        problems.append(
            "duplicate entity id(s): " + ", ".join(duplicates[:5])
        )
    return problems


def _validate_interactives(doc: dict) -> list[str]:
    """Check declared state against the geometry it claims to describe.

    The failure this exists to prevent is silent: an interactive whose id
    matches no entity produces a scene where the affordance binder has
    nothing to bind, so the door simply is not a door -- and the world looks
    completely fine. Same for a kind the engine has never heard of.
    """

    interactives = doc.get("interactives")
    if not isinstance(interactives, list) or not interactives:
        return []

    problems: list[str] = []
    if len(interactives) > _MAX_INTERACTIVES:
        problems.append(
            f"{len(interactives)} interactives declared, over the "
            f"{_MAX_INTERACTIVES} cap; state belongs on things that can "
            "change, not on static scenery"
        )

    entity_ids = {
        str(e.get("id"))
        for e in (doc.get("entities") or [])
        if isinstance(e, dict) and e.get("id")
    }

    seen: set[str] = set()
    for spec in interactives:
        if not isinstance(spec, dict):
            problems.append("an interactive entry is not an object")
            continue
        ident = str(spec.get("id") or "")
        if not ident:
            problems.append("an interactive has no id")
            continue
        if ident in seen:
            problems.append(f"duplicate interactive id '{ident}'")
            continue
        seen.add(ident)

        kind = str(spec.get("kind") or "")
        if kind not in _AFFORDANCE_KINDS:
            problems.append(
                f"interactive '{ident}' has unknown kind '{kind}'; expected "
                "one of " + ", ".join(sorted(_AFFORDANCE_KINDS))
            )
        if ident not in entity_ids:
            problems.append(
                f"interactive '{ident}' has no entity with that id, so "
                "nothing can bind to it"
            )

        # A door with no range cannot move, which reads as a door that does
        # not work rather than as a configuration mistake.
        if kind in ("door", "container"):
            has_hinge = abs(
                float(spec.get("max_angle_deg", 0.0) or 0.0)
                - float(spec.get("min_angle_deg", 0.0) or 0.0)
            ) > 1e-3
            has_slide = float(spec.get("slide_distance_m", 0.0) or 0.0) > 1e-3
            if not has_hinge and not has_slide:
                problems.append(
                    f"interactive '{ident}' is a {kind} with neither a hinge "
                    "range nor a slide distance, so it cannot open"
                )
        if kind == "mover" and len(spec.get("positions") or []) < 2:
            problems.append(
                f"interactive '{ident}' is a mover with fewer than two named "
                "positions, so it has nowhere to travel"
            )
        if kind == "harvestable" and not spec.get("yields_asset_id"):
            problems.append(
                f"interactive '{ident}' is harvestable but names no "
                "yields_asset_id, so picking it would produce nothing"
            )

    # Nav gates have to name something real, or the route is either
    # permanently open or permanently shut depending on how the runtime reads
    # a missing id -- both of which are wrong and neither of which is visible.
    nav = doc.get("navigation") or {}
    for link in nav.get("links") or []:
        if not isinstance(link, dict):
            continue
        gate = str(link.get("gated_by") or "")
        if not gate:
            continue
        if gate not in seen:
            problems.append(
                f"nav link {link.get('from')} -> {link.get('to')} is gated by "
                f"'{gate}', which is not a declared interactive"
            )
    return problems


def _validate_portals(doc: dict) -> list[str]:
    """Portals must name a target and an arrival point.

    A portal with no target scene is a door to nowhere; one with no spawn id
    drops the player at the target's world origin, which is usually inside
    geometry.
    """

    portals = doc.get("portals")
    spawn_points = doc.get("spawn_points")
    have_portals = isinstance(portals, list) and portals
    have_spawns = isinstance(spawn_points, list) and spawn_points
    # Checked independently: a world whose transitions are all menu-driven
    # declares named spawn points and NO portals, and its spawn ids still
    # have to be unique.
    if not have_portals and not have_spawns:
        return []

    problems: list[str] = []
    seen: set[str] = set()
    for portal in portals if have_portals else []:
        if not isinstance(portal, dict):
            continue
        ident = str(portal.get("id") or "")
        if not ident:
            problems.append("a portal has no id")
            continue
        if ident in seen:
            problems.append(f"duplicate portal id '{ident}'")
        seen.add(ident)
        if not portal.get("to_scene"):
            problems.append(f"portal '{ident}' names no target scene")

    # Duplicate spawn point ids would make "arrive at the north door"
    # ambiguous, and the ambiguity resolves differently per run.
    spawn_seen: set[str] = set()
    for spawn in doc.get("spawn_points") or []:
        if not isinstance(spawn, dict):
            continue
        ident = str(spawn.get("id") or "")
        if not ident:
            problems.append("a spawn point has no id")
            continue
        if ident in spawn_seen:
            problems.append(f"duplicate spawn point id '{ident}'")
        spawn_seen.add(ident)
    # Whether a portal's target_spawn_id exists is deliberately NOT checked
    # here: that spawn lives in the TARGET environment, which this validator
    # cannot see. It belongs to the cross-environment pass.
    return problems


def validate_environment_graph(build_spec: Any) -> list[str]:
    """Cross-scene checks that no single environment can see.

    Each world architect authors one environment and is told not to reference
    another, so these are the mistakes only the whole picture reveals: a link
    to a scene that does not exist, a borrowed environment that was never
    generated, or a scene nothing can reach.
    """

    scenes = list(getattr(build_spec, "scenes", None) or [])
    if not scenes:
        return ["build spec declares no scenes"]

    ids = {s.id for s in scenes}
    problems: list[str] = []

    for scene in scenes:
        for link in getattr(scene, "links", None) or []:
            if link.to_scene not in ids:
                problems.append(
                    f"scene '{scene.id}' links to '{link.to_scene}', which is "
                    "not a scene in this build"
                )
        borrowed = getattr(scene, "reuse_environment_from", None)
        if borrowed:
            if borrowed not in ids:
                problems.append(
                    f"scene '{scene.id}' reuses the environment of "
                    f"'{borrowed}', which is not a scene in this build"
                )
            elif borrowed == scene.id:
                problems.append(
                    f"scene '{scene.id}' reuses its own environment"
                )
        elif not getattr(scene, "needs_unique_environment", True):
            problems.append(
                f"scene '{scene.id}' says it does not need a unique "
                "environment but names no reuse_environment_from, so it is "
                "ambiguous which environment it borrows"
            )
        host = getattr(scene, "host", None)
        mode = getattr(getattr(scene, "environment_mode", None), "value", "")
        if mode == "nested" and host is None:
            problems.append(
                f"scene '{scene.id}' is nested but names no host to attach to"
            )

    # Reachability from the entry point. An unreachable mission is content
    # that was generated, paid for, and can never be played.
    entry = next((s.id for s in scenes if s.entry_point), scenes[0].id)
    reachable = {entry}
    frontier = [entry]
    while frontier:
        current = frontier.pop()
        scene = next((s for s in scenes if s.id == current), None)
        if scene is None:
            continue
        for link in getattr(scene, "links", None) or []:
            if link.to_scene in ids and link.to_scene not in reachable:
                reachable.add(link.to_scene)
                frontier.append(link.to_scene)
    # A nested environment is entered by its host, not by a link.
    for scene in scenes:
        if getattr(getattr(scene, "environment_mode", None), "value", "") == (
            "nested"
        ):
            reachable.add(scene.id)

    unreachable = sorted(ids - reachable)
    if unreachable and len(scenes) > 1:
        problems.append(
            "no route from the entry point to scene(s): "
            + ", ".join(unreachable)
        )
    return problems


# How far a doorway point may sit outside the rooms it joins before it counts
# as a mistake rather than a rounding difference.
_NAV_LINK_TOLERANCE_M = 1.5


def _validate_navigation(doc: dict) -> list[str]:
    """Check declared walkable topology against itself.

    The world architect declares its own navigation graph, which is far
    cheaper and more exact than reconstructing one from finished geometry --
    but it is also an LLM, so the declaration can be wrong in ways that only
    show up as NPCs standing still in a shipped game. These checks catch the
    mistakes that actually happen: an id typo in a link, a doorway coordinate
    that touches neither room, a room nobody can reach, a spawn point outside
    the walkable set.

    Declaring no navigation at all is legitimate -- an open arena needs none
    and NPCs fall back to local steering -- so an absent block is not an
    error. A malformed one is.
    """
    nav = doc.get("navigation")
    if not isinstance(nav, dict):
        return []
    regions = nav.get("regions") or []
    if not regions:
        return []

    problems: list[str] = []
    by_id: dict[str, dict] = {}
    for r in regions:
        if not isinstance(r, dict):
            continue
        rid = str(r.get("id") or "")
        mn = r.get("min") or []
        mx = r.get("max") or []
        if not rid or len(mn) < 3 or len(mx) < 3:
            problems.append(f"region {rid or '<unnamed>'} is malformed")
            continue
        if rid in by_id:
            problems.append(f"duplicate region id '{rid}'")
            continue
        width = abs(float(mx[0]) - float(mn[0]))
        depth = abs(float(mx[2]) - float(mn[2]))
        if width < 0.5 or depth < 0.5:
            problems.append(
                f"region '{rid}' is {width:.2f}x{depth:.2f} m, too small to "
                "stand in"
            )
        by_id[rid] = r

    def _near(region: dict, point: list) -> bool:
        mn, mx = region["min"], region["max"]
        dx = max(float(mn[0]) - point[0], 0.0, point[0] - float(mx[0]))
        dz = max(float(mn[2]) - point[2], 0.0, point[2] - float(mx[2]))
        return (dx * dx + dz * dz) <= _NAV_LINK_TOLERANCE_M ** 2

    adjacency: dict[str, set[str]] = {rid: set() for rid in by_id}
    for link in nav.get("links") or []:
        if not isinstance(link, dict):
            continue
        a, b = str(link.get("from") or ""), str(link.get("to") or "")
        if a not in by_id or b not in by_id:
            problems.append(f"link '{a}' -> '{b}' names an unknown region")
            continue
        point = link.get("point") or []
        if len(point) < 3:
            problems.append(f"link '{a}' -> '{b}' has no crossing point")
            continue
        # A crossing point that touches neither room is the failure mode that
        # looks fine in the JSON and sends every NPC into a wall.
        if not _near(by_id[a], point) or not _near(by_id[b], point):
            problems.append(
                f"link '{a}' -> '{b}' crosses at a point outside both regions"
            )
            continue
        adjacency[a].add(b)
        if link.get("bidirectional", True):
            adjacency[b].add(a)

    # Everything must be reachable from where the player starts, or part of
    # the level is walkable in theory and inert in practice.
    spawn_pos = ((doc.get("player") or {}).get("spawn") or {}).get("position")
    start: str | None = None
    if isinstance(spawn_pos, list) and len(spawn_pos) >= 3:
        for rid, r in by_id.items():
            mn, mx = r["min"], r["max"]
            if (float(mn[0]) <= spawn_pos[0] <= float(mx[0])
                    and float(mn[2]) <= spawn_pos[2] <= float(mx[2])):
                start = rid
                break
        if start is None:
            problems.append(
                "the player spawn is not inside any declared nav region"
            )

    if start is not None and len(by_id) > 1:
        seen = {start}
        queue = [start]
        while queue:
            cur = queue.pop()
            for nxt in adjacency.get(cur, ()):
                if nxt in seen:
                    continue
                seen.add(nxt)
                queue.append(nxt)
        stranded = sorted(set(by_id) - seen)
        if stranded:
            problems.append(
                "no route from the spawn to region(s): "
                + ", ".join(stranded[:5])
            )

    # NPC entrances and posts are only useful on floor an NPC can walk.
    def _inside(point: list) -> bool:
        return any(
            float(r["min"][0]) <= float(point[0]) <= float(r["max"][0])
            and float(r["min"][2]) <= float(point[2]) <= float(r["max"][2])
            for r in by_id.values())

    for key, label in (("npc_spawn_zones", "npc_spawn_zone"),
                       ("npc_anchors", "npc_anchor")):
        for item in doc.get(key) or []:
            if not isinstance(item, dict):
                continue
            pos = item.get("position") or []
            if len(pos) < 3:
                problems.append(f"{label} '{item.get('id')}' has no position")
            elif not _inside(pos):
                problems.append(
                    f"{label} '{item.get('id')}' at ({float(pos[0]):.1f}, "
                    f"{float(pos[2]):.1f}) is not inside any declared nav region"
                )

    return problems


def _gameplay_owned_assets(ctx: PipelineContext) -> set[str]:
    """Asset ids the scene composer already places as INTERACTIVE entities.

    A world architect that also places those (a rack of the same rifles the
    scene hands the player) produces look-alike props that cannot be picked
    up, at whatever scale it guessed -- the player cannot tell which rifle is
    real. They are listed as off-limits instead.
    """
    owned: set[str] = set()
    for path in sorted(ctx.project_dir.glob("scene_spec*.json")):
        try:
            doc = json.loads(path.read_text(encoding="utf-8"))
        except Exception:
            continue
        for ent in doc.get("entities", []) or []:
            if not isinstance(ent, dict):
                continue
            mesh = ent.get("mesh") or {}
            aid = mesh.get("asset_id") if isinstance(mesh, dict) else None
            if not aid:
                continue
            tags = set(ent.get("tags") or [])
            interactive = bool(ent.get("mechanic_bindings")) or bool(
                tags & {"grabbable", "weapon", "interactable", "wearable"}
            )
            if interactive:
                owned.add(aid)
    return owned


def _load_object_assets(ctx: PipelineContext) -> tuple[dict[str, dict], list[dict]]:
    """Return (asset_id -> {glb_path, target_scale_meters}, listing) for props
    the agent MAY place. Environment assets are no longer generated as GLBs.

    The scale travels with the path: ``ptv_worldgen`` used to hard-code 1.0
    for every placed object, so a world-placed pistol came out a metre long.
    """
    asset_map: dict[str, dict] = {}
    listing: list[dict] = []
    owned = _gameplay_owned_assets(ctx)
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
            asset_map[item.id] = {
                "glb_path": glb,
                "target_scale_meters": float(item.target_scale_meters),
            }
        listing.append(
            {
                "id": item.id,
                "name": item.name,
                "category": cat,
                "target_scale_meters": float(item.target_scale_meters),
                # The scene already places this as something the player
                # interacts with; do NOT place decorative copies of it.
                "gameplay_owned": item.id in owned,
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
