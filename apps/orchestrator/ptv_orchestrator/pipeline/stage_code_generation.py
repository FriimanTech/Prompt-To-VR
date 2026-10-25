"""Stage 8: Multi-agent code generation.

Architecture (see ``docs/codegen-agents.md`` for the full design):

* The MechanicPlan's ``generate`` items each enroll on a per-project
  ledger at ``projects/<project_id>/codegen_board.json``.
* Up to ``settings.codegen_max_parallel_agents`` worker threads run
  concurrently (default 3). Each thread runs the worker loop below
  until no task is eligible.
* The board enforces FCFS (= MechanicPlan order) and the dependency
  DAG: a CodeTask with any unresolved ``depends_on`` stays ``blocked``
  and only becomes ``pending`` once every dep is ``done``.
* For each claimed CodeTask, ONE OpenAI Agents SDK ``Runner.run_sync``
  session executes the four-step process (read metadata -> read deps
  / ledger -> optional web search with NO cap -> implement multi-file
  C++). The structured result carries ``files[]`` (1..16 files) and
  ``exports`` for the ledger.
* Each generated file (``.cpp``, ``.h``, ``.hpp``) passes
  ``validate_includes`` -> ``filter_cpp_only`` ->
  ``repair_generated_cpp`` -> ``assert_path_allowed`` before being
  written. Header files are additionally checked by
  ``validate_header_content`` (no registration macros in headers).
  The board records ``files_written`` and ``exports`` so downstream
  tasks can plug into them.
* Once every task is ``done``, an optional **compile-verification +
  repair loop** builds the ``ptv_starter_room`` executable (which links
  ``ptv_runtime`` with every generated mechanic force-linked). Include
  validation only checks *includes*; this loop additionally catches
  compile AND link errors (e.g. a task calling a dependency's function
  with a mismatched signature -- an ``unresolved external symbol`` that a
  static-lib-only build would miss). On failure the errors are mapped
  back to the owning task and fed to that task's agent as a
  ``repair_context`` so it regenerates the file; the stage then rebuilds,
  up to ``codegen_max_repair_rounds`` times. Controlled by
  ``settings.codegen_verify_compile``.

The CodeTask ``test_files[]`` field is intentionally unused: no disk
writes, no CMake test targets, no planner prompts for tests.
"""

from __future__ import annotations

import dataclasses
import json
import os
import re
import subprocess
import threading
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable, Optional

from ptv_schemas import (
    BuildSpec,
    CodeTask,
    CodeTaskFile,
    CodeTaskKind,
    MechanicClassification,
    ProjectStage,
    ProjectStatus,
    SceneSpec,
    TargetDevice,
)

from ..build.cmake_runner import configure_and_build
from .bounds import (
    assert_path_allowed,
    filter_cpp_only,
    repair_generated_cpp,
    validate_header_content,
    validate_includes,
)
from .builtin_source import format_builtin_sources_block, read_builtin_sources
from .codegen_agent import (
    CodegenAgentError,
    CodegenAgentInput,
    CodegenAgentResult,
    RunnerFn,
    dep_exports_json,
    run_codegen_agent,
)
from .codegen_board import CodegenBoard
from .codegen_contracts import (
    API_CONTRACT_VERSION,
    find_dead_contracts,
    lint_task_sources,
)
from .context import PipelineContext, StageResult
from .npc_population import reconcile_npc_ai_items
from .proc import proc_hooks
from .prompts import code_task_system


# Maps ``CodeTaskKind`` -> directory under ``engine/generated/``.
_KIND_DIR: dict[CodeTaskKind, str] = {
    CodeTaskKind.mechanic: "mechanics",
    CodeTaskKind.ai: "ai",
    CodeTaskKind.dialogue: "dialogue",
}


class CodegenContractError(CodegenAgentError):
    """Generated code that compiles but breaks an engine contract
    (see ``codegen_contracts``). Repaired once with the violations as
    feedback before the task is failed."""


# A compile verifier builds the generated C++ and reports ``(ok, log)``.
# Production uses ``_default_verify_compile``; tests inject a stub.
CompileVerifier = Callable[[], "tuple[bool, str]"]


# Substrings that mark an agent failure as a TRANSPORT problem rather than a
# disagreement about the task. Matched on the message because the Agents SDK
# flattens provider exceptions into ``CodegenAgentError``, so the type is the
# same either way. Kept deliberately narrow: retrying a schema violation just
# pays twice for the same refusal.
_TRANSIENT_AGENT_ERRORS = (
    "request timed out",
    "timeout",
    "connection error",
    "connection reset",
    "temporarily unavailable",
    "server error",
    "internal server error",
    "bad gateway",
    "service unavailable",
    "rate limit",
    "overloaded",
    " 429",
    " 500",
    " 502",
    " 503",
    " 504",
)


def _is_transient(exc: Exception) -> bool:
    text = str(exc).lower()
    return any(marker in text for marker in _TRANSIENT_AGENT_ERRORS)


# ---------------------------------------------------------------------------
# Public entry point (matches the StageFn signature in orchestrator.py)
# ---------------------------------------------------------------------------


def run(ctx: PipelineContext) -> StageResult:
    """Standard stage entry point used by the orchestrator."""
    return run_with_runner(ctx, agent_runner=None)


def run_with_runner(
    ctx: PipelineContext,
    agent_runner: Optional[RunnerFn] = None,
    compile_verifier: Optional[CompileVerifier] = None,
) -> StageResult:
    """Run the code-generation stage with an optional injected agent
    runner. Tests use this hook to bypass the OpenAI Agents SDK.

    Parameters
    ----------
    ctx:
        The pipeline context.
    agent_runner:
        Optional ``(CodegenAgentInput, model) -> CodegenAgentResult``
        callable. When ``None`` (production), the default OpenAI Agents
        SDK runner is used.
    compile_verifier:
        Optional ``() -> (ok, log)`` callable used by the compile-verify
        loop. When ``None`` (production) and
        ``settings.codegen_verify_compile`` is set, the default cmake
        verifier builds the ``ptv_runtime`` target. Tests inject a stub
        so no real toolchain is required.
    """

    ctx.emit(ProjectStage.code_generation, "running", "Generating C++ modules")
    state = ctx.store.load_state(ctx.project_id)
    ctx.store.record_stage(
        state, ProjectStage.code_generation, ProjectStatus.running
    )

    plan = ctx.mechanic_plan or ctx.store.load_mechanic_plan(ctx.project_id)

    # A plan written before the behaviour-coverage rule may still ask for a
    # generated copy of a built-in NPC behaviour. Converting it here, with the
    # SceneSpecs to say which NPC each item drives, is what lets a resume
    # from code_generation drop that task (the board deletes its files).
    try:
        npc_plan = ctx.npc_plan or ctx.store.load_npc_plan(ctx.project_id)
    except Exception:  # noqa: BLE001 - projects from before the NPC plan
        npc_plan = None
    plan, covered_notes = reconcile_npc_ai_items(
        plan, npc_plan, _load_scene_specs(ctx.store.dir_for(ctx.project_id)))
    if covered_notes:
        ctx.mechanic_plan = plan
        ctx.store.save_mechanic_plan(ctx.project_id, plan)
        for note in covered_notes:
            ctx.emit(ProjectStage.code_generation, "info", f"NPCs: {note}"[:300])
            ctx.store.append_log(ctx.project_id, "code_generation", f"NPCs: {note}")

    codegen_items = [it for it in plan.items if it.code_task is not None]

    if not codegen_items:
        # Nothing to do; record a clean success.
        ctx.store.record_stage(
            state,
            ProjectStage.code_generation,
            ProjectStatus.succeeded,
            message="No generate-class mechanics in plan",
        )
        ctx.emit(ProjectStage.code_generation, "succeeded", "0 files")
        return StageResult(stage=ProjectStage.code_generation, artifacts=[])

    project_dir = ctx.store.dir_for(ctx.project_id)
    repo_root: Path = ctx.settings.repo_root
    gen_root = repo_root / "engine" / "generated"

    # ------------------------------------------------------------------
    # Build the codegen board (Part A: work queue + ledger + DAG)
    # ------------------------------------------------------------------
    max_agents = max(1, int(getattr(ctx.settings, "codegen_max_parallel_agents", 3)))
    board = CodegenBoard.load_or_init(
        project_dir, plan, max_agents=max_agents, repo_root=repo_root
    )
    if board.dropped_tasks:
        ctx.emit(
            ProjectStage.code_generation,
            "info",
            "dropped tasks the current MechanicPlan no longer has: "
            + ",".join(board.dropped_tasks)
            + (f" (deleted {len(board.deleted_files)} of their files)"
               if board.deleted_files else ""),
        )
    if board.changed_tasks:
        ctx.emit(
            ProjectStage.code_generation,
            "info",
            "regenerating tasks whose CodeTask changed since they were written: "
            + ",".join(board.changed_tasks),
        )
    # Resume policy: reset anything left stuck in ``in_progress`` from a
    # crashed previous run, so a new worker can claim it again.
    reset = board.reset_stuck_in_progress()
    if reset:
        ctx.emit(
            ProjectStage.code_generation,
            "info",
            f"reset stuck in_progress tasks from previous run: {','.join(reset)}",
        )

    # A changed socket contract invalidates the C++ that read it. Without
    # this, "fix the asset_contract stage, then re-run code_generation" is a
    # no-op: every task is already ``done``, no worker claims anything, and
    # the socket-blind code ships.
    socket_resets = board.invalidate_on_socket_change(
        _sockets_by_asset(project_dir), repo_root
    )
    if socket_resets:
        ctx.emit(
            ProjectStage.code_generation,
            "info",
            "asset socket contract changed since these tasks were generated; "
            f"regenerating: {','.join(socket_resets)}",
        )

    # Same idea for the engine contract (codegen_contracts): code written
    # before a rule existed -- an AI with no MechanicContext, a private
    # damage inbox -- compiled, shipped and silently did nothing.
    api_resets = board.invalidate_on_api_contract_change(
        API_CONTRACT_VERSION, repo_root,
        lambda files: lint_task_sources("", files),
    )
    for tid, why in api_resets:
        ctx.emit(
            ProjectStage.code_generation,
            "info",
            f"regenerating {tid}: "
            + ("; ".join(why)[:400] if why else "depends on a regenerated task"),
        )

    # Also retry previously failed tasks. A failure may have been caused by
    # a transient issue (e.g. SDK strict schema incompatibility, network
    # timeout) that is now fixed. Resetting them to pending lets the workers
    # claim and retry them.
    reset_failed = board.reset_failed()
    if reset_failed:
        ctx.emit(
            ProjectStage.code_generation,
            "info",
            f"reset failed tasks for retry: {','.join(reset_failed)}",
        )

    initial_counts = board.status_counts()
    ctx.emit(
        ProjectStage.code_generation,
        "board_init",
        (
            f"tasks={sum(initial_counts.values())} "
            f"pending={initial_counts['pending']} "
            f"blocked={initial_counts['blocked']} "
            f"done={initial_counts['done']} "
            f"failed={initial_counts['failed']} "
            f"max_agents={max_agents}"
        ),
    )

    # ------------------------------------------------------------------
    # Build static agent context (BuildSpec + every scene_spec*.json)
    # ------------------------------------------------------------------
    static_ctx = _load_static_context(ctx)

    # Pull the actual CodeTask payloads off the plan so workers can
    # claim by id and still see the full task.
    tasks_by_id: dict[str, CodeTask] = {
        it.code_task.id: it.code_task  # type: ignore[union-attr]
        for it in codegen_items
    }

    # Map code-edit task ids -> base built-in id whose source the agent
    # should copy-and-edit. Only ``builtin+config_code_edit`` items appear
    # here; ``generate`` items author from scratch.
    code_edit_base_by_task_id: dict[str, str] = {
        it.code_task.id: (it.base_builtin_id or it.builtin_id or "")  # type: ignore[union-attr]
        for it in codegen_items
        if it.classification == MechanicClassification.builtin_config_code_edit
        and (it.base_builtin_id or it.builtin_id)
    }

    # ------------------------------------------------------------------
    # Ensure OPENAI_API_KEY is in os.environ for the Agents SDK.
    # Pydantic BaseSettings reads .env into settings.openai_api_key but
    # does NOT inject it into os.environ. The OpenAI Agents SDK creates
    # its own internal client and reads OPENAI_API_KEY from os.environ
    # directly, so we bridge the gap here.
    # ------------------------------------------------------------------
    if ctx.settings.openai_api_key and not os.environ.get("OPENAI_API_KEY"):
        os.environ["OPENAI_API_KEY"] = ctx.settings.openai_api_key

    # ------------------------------------------------------------------
    # Spawn the thread pool and drain the board
    # ------------------------------------------------------------------
    # Codegen has its own model knobs rather than borrowing
    # ``openai_hard_model`` (which means "what we use when Claude is down").
    # The effort in particular must be explicit: the Agents SDK infers it
    # from the model name otherwise, and infers ``none`` for gpt-5.6-sol.
    runtime = _StageRuntime(
        ctx=ctx,
        board=board,
        tasks_by_id=tasks_by_id,
        code_edit_base_by_task_id=code_edit_base_by_task_id,
        static=static_ctx,
        gen_root=gen_root,
        repo_root=repo_root,
        model_id=ctx.settings.codegen_model,
        reasoning_effort=ctx.settings.codegen_reasoning_effort,
        verbosity=ctx.settings.codegen_verbosity,
        agent_runner=agent_runner,
    )

    with ThreadPoolExecutor(max_workers=max_agents) as pool:
        futures = [
            pool.submit(runtime.worker_loop, f"worker-{i}")
            for i in range(max_agents)
        ]
        for fut in futures:
            # ``worker_loop`` swallows per-task errors and stamps them
            # on the board; an exception escaping here means a hard
            # bug in the driver itself.
            fut.result()

    # Cross-task wiring: an entry point one task offers that no other task
    # calls is a producer that was never written. Reported, not fatal --
    # the per-task rules already reject the damage-inbox case outright.
    task_sources: dict[str, dict[str, str]] = {}
    for tid in board.task_order():
        row = board.task(tid) or {}
        files: dict[str, str] = {}
        for rel in row.get("files_written") or []:
            try:
                files[Path(rel).name] = (repo_root / rel).read_text(
                    encoding="utf-8", errors="ignore")
            except OSError:
                continue
        if files:
            task_sources[tid] = files
    for warning in find_dead_contracts(task_sources):
        ctx.emit(ProjectStage.code_generation, "warning", f"dead contract: {warning}")
        ctx.store.append_log(ctx.project_id, "code_generation", f"dead contract: {warning}")

    # ------------------------------------------------------------------
    # Compile-verification + repair loop
    #
    # Every task wrote its files and passed include validation, but that
    # does not prove the C++ compiles. If nothing failed/blocked, build the
    # ptv_runtime target (which globs in the generated mechanics) and feed
    # any compiler errors back to the owning task's agent to regenerate.
    # ------------------------------------------------------------------
    pre_failed = [
        tid
        for tid in board.task_order()
        if (board.task(tid) or {}).get("status") == "failed"
    ]
    pre_blocked = [
        tid
        for tid in board.task_order()
        if (board.task(tid) or {}).get("status") == "blocked"
    ]
    # Generated files may exist on disk from a previous run even when THIS
    # run produced nothing (an idempotent resume where every task is already
    # ``done``). We must still compile-verify in that case -- otherwise a
    # resume after a partial/interrupted repair can ship un-verified C++ to
    # the cmake_build stage (the shooting_hub false-green regression).
    done_with_files = any(
        (board.task(tid) or {}).get("status") == "done"
        and (board.task(tid) or {}).get("files_written")
        for tid in board.task_order()
    )
    verify_excerpt = ""
    if (
        not pre_failed
        and not pre_blocked
        and getattr(ctx.settings, "codegen_verify_compile", True)
        and (runtime.produced or done_with_files)
    ):
        verifier = compile_verifier or (lambda: _verify_all_targets(ctx))
        max_rounds = max(0, int(getattr(ctx.settings, "codegen_max_repair_rounds", 2)))
        verify_ok, verify_log, failing_after = runtime.verify_and_repair(
            verifier=verifier, max_rounds=max_rounds
        )
        if not verify_ok:
            verify_excerpt = _error_excerpt(verify_log)
            # Mark the still-failing tasks so the standard failure path
            # reports + raises. If we could not map the errors to a
            # specific task, mark every codegen task so the build stage
            # never runs against C++ that does not compile.
            targets = failing_after or list(tasks_by_id.keys())
            for tid in targets:
                row = board.task(tid) or {}
                if row.get("status") == "done":
                    try:
                        board.mark_failed(
                            tid,
                            error=f"compile verification failed: {verify_excerpt}"[:2000],
                        )
                    except KeyError:
                        pass

    # ------------------------------------------------------------------
    # Wrap up
    # ------------------------------------------------------------------
    if not board.flush():
        ctx.emit(
            ProjectStage.code_generation,
            "warning",
            f"codegen_board.json could not be written ({board.last_save_error}); "
            "this run's results are intact, but a Resume may redo finished tasks",
        )

    final_counts = board.status_counts()
    produced: list[str] = []
    with runtime._produced_lock:
        produced.extend(runtime.produced)

    failed_ids = [
        tid
        for tid in board.task_order()
        if (board.task(tid) or {}).get("status") == "failed"
    ]
    blocked_ids = [
        tid
        for tid in board.task_order()
        if (board.task(tid) or {}).get("status") == "blocked"
    ]

    msg = (
        f"done={final_counts['done']} failed={final_counts['failed']} "
        f"blocked={final_counts['blocked']} files={len(produced)}"
    )

    overall_status = (
        ProjectStatus.failed
        if (failed_ids or blocked_ids)
        else ProjectStatus.succeeded
    )

    detail = msg
    if failed_ids:
        detail += f" failed_ids={','.join(failed_ids)}"
    if blocked_ids:
        detail += f" blocked_ids={','.join(blocked_ids)}"
    if verify_excerpt:
        detail += f" compile_error={verify_excerpt[:200]}"

    ctx.store.record_stage(
        state,
        ProjectStage.code_generation,
        overall_status,
        message=detail,
        artifacts=produced,
    )

    ctx.emit(
        ProjectStage.code_generation,
        "succeeded" if overall_status == ProjectStatus.succeeded else "failed",
        msg,
    )

    if overall_status == ProjectStatus.failed:
        # Surface a clear error to the top-level pipeline runner so the
        # build stage doesn't try to compile broken code.
        raise RuntimeError(
            f"Code-generation stage finished with {final_counts['failed']} "
            f"failed task(s) and {final_counts['blocked']} blocked task(s). "
            f"See codegen_board.json."
        )

    return StageResult(
        stage=ProjectStage.code_generation,
        artifacts=produced,
        message=msg,
    )


# ---------------------------------------------------------------------------
# Static agent context (BuildSpec + scene specs)
# ---------------------------------------------------------------------------


@dataclass
class _StaticContext:
    build_spec: Optional[BuildSpec]
    scene_specs: list[SceneSpec]
    """Each parsed SceneSpec is paired with its filename for debug
    purposes via ``scene_files`` below."""

    scene_files: list[str]

    # asset_id -> sorted socket names derived by the asset_contract stage.
    # This is the whole reason that stage runs before codegen: the agent needs
    # to see which named attachment points actually exist so it writes
    # ctx.sockets->socket_world(self, "muzzle") against a real name instead of
    # inventing a hard-coded offset it cannot possibly know.
    sockets_by_asset: dict[str, list[str]]

    # Which VR target this project builds for, read once from ProjectState
    # (the source of truth) so every task's system prompt names the build
    # that is actually happening. A Quest APK runs on a mobile GPU; code
    # written for a desktop budget misses frame rate there.
    target_device: TargetDevice = TargetDevice.pcvr

    # What every NPC is supposed to do, as decided once by the npc_plan stage.
    # Handed to every task rather than only AI ones: a wave spawner needs the
    # spawn caps, a HUD needs the health values, and a weapon needs to know
    # which faction it damages.
    npc_plan_json: str = ""

    # The animation keys every character really has, from asset_animation.json
    # and character_routing.json (provider, keys, gait variants, missing
    # actions). Without it the agent guesses clip names from the NPC plan.
    animation_inventory_json: str = ""

    # What the world architects actually declared, per environment: the
    # interactive ids, their kinds and state keys, the spawn points and the
    # portals. This is why world_generation now runs BEFORE this stage.
    #
    # A mechanic that opens a door, waits on a lever, or teleports the player
    # has to name the thing it acts on, and those names are invented by the
    # world architect at generation time. Without this the agent could only
    # guess -- and a guessed id resolves to nothing, silently, at runtime.
    world_context_json: str = ""


def _load_world_context(ctx: PipelineContext) -> str:
    """Summarize every generated environment's declared vocabulary.

    Deliberately a summary, not the whole document: a world is tens of
    thousands of primitives, of which only the declared interactives, spawn
    points and portals are addressable by name from gameplay code.
    """

    project_dir = ctx.store.dir_for(ctx.project_id)
    envs: list[dict[str, Any]] = []

    for path in sorted(project_dir.glob("world*.scene.json")):
        try:
            doc = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            continue
        if not isinstance(doc, dict):
            continue

        # ``world.<scene_id>.scene.json`` -> scene_id; ``world.scene.json``
        # is the single-environment layout and has no scene in its name.
        stem = path.name[len("world") : -len(".scene.json")].strip(".")

        interactives = []
        for item in doc.get("interactives") or []:
            if not isinstance(item, dict):
                continue
            interactives.append(
                {
                    "id": item.get("id", ""),
                    "kind": item.get("kind", ""),
                    # State KEYS matter more than their values here: the agent
                    # needs to know a door is read with "open", not "is_open".
                    "state_keys": sorted((item.get("initial_state") or {}).keys()),
                    "locked": bool(item.get("locked", False)),
                }
            )

        envs.append(
            {
                "scene_id": stem or "(single environment)",
                "builder_id": doc.get("environment_builder_id", ""),
                "interactives": interactives,
                "spawn_points": sorted(
                    sp.get("id", "")
                    for sp in (doc.get("spawn_points") or [])
                    if isinstance(sp, dict)
                ),
                "portals": [
                    {
                        "id": p.get("id", ""),
                        "to_scene": p.get("to_scene", ""),
                        "spawn_point_id": p.get("spawn_point_id", ""),
                    }
                    for p in (doc.get("portals") or [])
                    if isinstance(p, dict)
                ],
                "nav_regions": sorted(
                    r.get("id", "")
                    for r in (doc.get("nav_regions") or [])
                    if isinstance(r, dict)
                ),
            }
        )

    if not envs:
        return ""
    return json.dumps(envs, indent=2, sort_keys=True)


def _sockets_by_asset(project_dir: Path) -> dict[str, list[str]]:
    """``asset_id -> sorted socket names`` from ``asset_contract.json``.

    The agent's whole view of the socket contract, and the thing the codegen
    board fingerprints, so both read it through here and cannot disagree
    about what "the contract changed" means.
    """
    out: dict[str, list[str]] = {}
    contract_path = project_dir / "asset_contract.json"
    if not contract_path.exists():
        return out
    try:
        contract = json.loads(contract_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        # No contract is a valid state (projects predating the stage); the
        # agent then simply sees no sockets and uses no sockets.
        return out
    for asset_id, entry in (contract or {}).items():
        names = sorted((entry or {}).get("sockets") or {})
        if names:
            out[asset_id] = names
    return out


def _load_scene_specs(project_dir: Path) -> list[SceneSpec]:
    """Every per-scene SceneSpec on disk (the canonical copy is skipped: it
    duplicates the entry scene)."""
    out: list[SceneSpec] = []
    for p in sorted(project_dir.glob("scene_spec.*.json")):
        try:
            out.append(SceneSpec.model_validate(
                json.loads(p.read_text(encoding="utf-8"))))
        except Exception:  # noqa: BLE001 - a broken spec just maps nothing
            continue
    return out


def _load_static_context(ctx: PipelineContext) -> _StaticContext:
    """Load BuildSpec + every scene_spec*.json on disk. Missing files
    are tolerated -- the agent simply sees the available context."""

    try:
        build_spec: Optional[BuildSpec] = ctx.store.load_build_spec(ctx.project_id)
    except Exception:
        build_spec = None

    project_dir = ctx.store.dir_for(ctx.project_id)
    scene_specs: list[SceneSpec] = []
    scene_files: list[str] = []
    for p in sorted(project_dir.glob("scene_spec*.json")):
        # ``scene_spec.json`` (the canonical name) and
        # ``scene_spec.<scene_id>.json`` (per-scene files) both match.
        try:
            scene = ctx.store.load_scene_spec(ctx.project_id, filename=p.name)
            scene_specs.append(scene)
            scene_files.append(p.name)
        except Exception:
            # A malformed scene file shouldn't sink the whole stage.
            ctx.emit(
                ProjectStage.code_generation,
                "warn",
                f"failed to parse {p.name}; skipping in agent context",
            )

    npc_plan_json = ""
    try:
        npc_plan = ctx.store.load_npc_plan(ctx.project_id)
        if npc_plan.npcs:
            npc_plan_json = npc_plan.model_dump_json(indent=2)
    except Exception:
        # Projects predating the stage simply have no plan; the agent then
        # sees no NPCs, which is the pre-existing behaviour.
        pass

    return _StaticContext(
        build_spec=build_spec,
        scene_specs=scene_specs,
        scene_files=scene_files,
        sockets_by_asset=_sockets_by_asset(project_dir),
        target_device=ctx.store.load_state(ctx.project_id).target_device,
        npc_plan_json=npc_plan_json,
        animation_inventory_json=animation_inventory(project_dir),
        world_context_json=_load_world_context(ctx),
    )


def animation_inventory(project_dir: Path) -> str:
    """Per character: provider, available animation keys, variants, gaps."""
    from .character_artifacts import ASSET_ANIMATION_NAME, load_json, load_routing

    anims = load_json(project_dir / ASSET_ANIMATION_NAME)
    routing = load_routing(project_dir)
    out: dict[str, Any] = {}
    asset_ids = set(anims) | {r.asset_id for r in (routing.routes if routing else [])}
    for asset_id in sorted(asset_ids):
        entry = anims.get(asset_id) if isinstance(anims.get(asset_id), dict) else {}
        route = routing.route_for(asset_id) if routing else None
        keys = sorted((entry.get("animations") or {}).keys()) if entry.get("status") == "animated" else []
        out[asset_id] = {
            "provider": entry.get("provider")
            or (route.effective_animation_provider().value if route else "none"),
            "keys": keys,
            "gait_variants": sorted(k.split(".", 1)[1] for k in keys if "." in k),
            "missing": [
                {"key": u.key, "fallback": u.fallback.value}
                for u in (route.unmet if route else [])
            ],
        }
    if not out:
        return ""
    return json.dumps(out, indent=2, sort_keys=True)


# ---------------------------------------------------------------------------
# Worker / runtime
# ---------------------------------------------------------------------------


class _StageRuntime:
    """Encapsulates per-stage state shared across all worker threads.

    Threads share the ``CodegenBoard`` (its own lock guards mutations)
    and the ``produced`` list (guarded by ``_produced_lock``).
    """

    def __init__(
        self,
        *,
        ctx: PipelineContext,
        board: CodegenBoard,
        tasks_by_id: dict[str, CodeTask],
        static: _StaticContext,
        gen_root: Path,
        repo_root: Path,
        model_id: str,
        agent_runner: Optional[RunnerFn],
        reasoning_effort: str = "high",
        verbosity: str = "medium",
        code_edit_base_by_task_id: Optional[dict[str, str]] = None,
    ) -> None:
        self.ctx = ctx
        self.board = board
        self.tasks_by_id = tasks_by_id
        self.code_edit_base_by_task_id = code_edit_base_by_task_id or {}
        self.static = static
        self.gen_root = gen_root
        self.repo_root = repo_root
        self.model_id = model_id
        self.reasoning_effort = reasoning_effort
        self.verbosity = verbosity
        self.agent_runner = agent_runner

        self.produced: list[str] = []
        self._produced_lock = threading.Lock()

    # ------------------------------------------------------------------
    # Agent call
    # ------------------------------------------------------------------
    def _run_agent_with_retry(
        self, task_id: str, inp: CodegenAgentInput
    ) -> CodegenAgentResult:
        """One agent call, retried on a transport-level failure.

        A timeout or a 5xx here does not just lose the task: every task
        that declares it in ``depends_on`` stays blocked, and the stage
        fails after all the OTHER tasks have already been paid for. One
        in-process retry is what a human would do anyway, and it costs a
        re-run of a single task rather than the stage.

        A malformed RESULT is not retried -- that is the agent
        disagreeing with the schema, and it would disagree again.
        """
        attempts = max(1, int(self.ctx.settings.codegen_max_task_attempts))
        last: Exception | None = None
        for attempt in range(1, attempts + 1):
            try:
                return run_codegen_agent(
                    inp,
                    model=self.model_id,
                    reasoning_effort=self.reasoning_effort,
                    verbosity=self.verbosity,
                    request_timeout_seconds=(
                        self.ctx.settings.codegen_request_timeout_seconds
                    ),
                    runner=self.agent_runner,
                )
            except CodegenAgentError as exc:
                last = exc
                if attempt >= attempts or not _is_transient(exc):
                    raise
                self.ctx.emit(
                    ProjectStage.code_generation,
                    "info",
                    f"{task_id}: agent call failed ({exc}); "
                    f"retrying (attempt {attempt + 1}/{attempts})",
                )
        raise last  # type: ignore[misc]  # unreachable: loop returns or raises

    # ------------------------------------------------------------------
    # Worker loop
    # ------------------------------------------------------------------
    def worker_loop(self, agent_id: str) -> None:
        """Pull-based work loop. Each iteration refreshes blocked rows,
        atomically claims the next pending task, runs the agent, writes
        files, and marks done/failed. Exits when no task is claimable.
        """

        while True:
            # An abort must not have to wait for the whole board to drain:
            # each worker stops before claiming its next task. The one in
            # flight is left in_progress, which the board's own resume logic
            # already knows how to reset.
            self.ctx.raise_if_cancelled()

            # Belt-and-suspenders: even though mark_done refreshes
            # blocked, calling refresh_blocked at the top of every loop
            # iteration is the documented contract and ensures
            # promotions happen as soon as any worker is next able to
            # claim.
            promoted = self.board.refresh_blocked()
            for pid in promoted:
                self.ctx.emit(
                    ProjectStage.code_generation,
                    "task_unblocked",
                    f"{pid} (blocked -> pending)",
                )

            claimed = self.board.atomic_claim_next_pending(agent_id)
            if claimed is None:
                # No eligible task. Either everything is done/in-progress/failed,
                # or all remaining work is still blocked on in-flight deps.
                # Either way this worker exits cleanly.
                return

            task_id: str = claimed["id"]
            self.ctx.emit(
                ProjectStage.code_generation,
                "task_claimed",
                f"{task_id} by {agent_id}",
            )

            try:
                files_written, exports = self._process_one_task(
                    agent_id=agent_id,
                    task_id=task_id,
                )
                promoted = self.board.mark_done(
                    task_id, files_written=files_written, exports=exports
                )
                self.ctx.emit(
                    ProjectStage.code_generation,
                    "task_completed",
                    f"{task_id} files={len(files_written)}"
                    + (f" promoted={','.join(promoted)}" if promoted else ""),
                )
            except Exception as exc:
                # Stamp the failure and continue with the next task.
                err = f"{type(exc).__name__}: {exc}"
                try:
                    self.board.mark_failed(task_id, error=err)
                except KeyError:
                    pass  # the row is gone; nothing left to stamp
                self.ctx.emit(
                    ProjectStage.code_generation,
                    "task_failed",
                    f"{task_id} ({err[:240]})",
                )

    # ------------------------------------------------------------------
    # One task -> agent -> file writes -> exports
    # ------------------------------------------------------------------
    def _process_one_task(
        self, *, agent_id: str, task_id: str, repair_context: str = ""
    ) -> tuple[list[str], dict[str, Any]]:
        """Run the agent for ``task_id``, validate + write its files,
        return ``(files_written_rel_paths, exports)``.

        ``repair_context`` is non-empty only on a compile-repair re-run; it
        carries the compiler errors + current file contents so the agent
        fixes the existing files instead of authoring from scratch."""

        task = self.tasks_by_id.get(task_id)
        if task is None:
            raise RuntimeError(
                f"task {task_id!r} claimed but not present in plan"
            )

        # Build the per-task agent context bundle. We snapshot the
        # board once at the start of this run -- the agent's run
        # itself doesn't need live ledger updates, only a coherent
        # view of dep exports at claim time.
        snapshot = self.board.snapshot()
        scene_ctx_json = _scene_context_json(
            task, self.static.scene_specs, self.static.sockets_by_asset
        )
        bs_excerpt = _buildspec_excerpt(task, self.static.build_spec)
        output_subdir = (
            f"engine/generated/{_KIND_DIR[task.kind]}/{self.ctx.project_id}"
        )

        extra = [
            f"This run is owned by agent_id={agent_id}.",
            f"Project id is {self.ctx.project_id!r} -- all generated paths must include it.",
        ]

        # Code-edit task: inject the base built-in's source so the agent
        # copies-and-edits it into a NEW generated mechanic.
        base_source_block = ""
        base_builtin_id = self.code_edit_base_by_task_id.get(task.id, "")
        if base_builtin_id:
            sources = read_builtin_sources(self.repo_root, base_builtin_id)
            base_source_block = format_builtin_sources_block(sources)
            extra.append(
                f"CODE-EDIT TASK: copy and adapt the source of "
                f"'{base_builtin_id}' below into a NEW mechanic registered "
                f"under 'generated.{task.id}'. Do NOT replace the built-in."
            )

        inp = CodegenAgentInput(
            project_id=self.ctx.project_id,
            task_id=task.id,
            output_subdir=output_subdir,
            task_json=task.model_dump_json(indent=2),
            ledger_snapshot_json=json.dumps(snapshot, indent=2, sort_keys=True),
            dep_exports_json=dep_exports_json(snapshot, list(task.depends_on)),
            scene_context_json=scene_ctx_json,
            build_spec_excerpt=bs_excerpt,
            npc_plan_json=self.static.npc_plan_json,
            animation_inventory_json=self.static.animation_inventory_json,
            world_context_json=self.static.world_context_json,
            instructions=code_task_system(self.static.target_device),
            base_source_block=base_source_block,
            repair_context=repair_context,
            extra_instructions=extra,
        )

        result = self._run_agent_with_retry(task.id, inp)

        # Headers produced by this task's declared dependency tasks. They live
        # in the same project directory and the board guarantees they are
        # ``done`` (written to disk) before this task runs, so a dependent
        # task MAY include them (e.g. a weapon profile including
        # ``shooting_hub_shared.h``).
        dep_headers = _dep_header_names(snapshot, list(task.depends_on))

        try:
            files_written_abs, files_written_rel = self._write_files(
                task, result, dep_headers=dep_headers
            )
        except CodegenContractError as exc:
            # One repair round with the rules it broke. Code that compiles
            # but cannot work at runtime is exactly what the agent can fix
            # when told precisely why.
            self.ctx.emit(
                ProjectStage.code_generation,
                "info",
                f"{task.id}: engine contract violations; asking the agent to "
                f"fix them ({str(exc)[:240]})",
            )
            produced = "\n\n".join(
                f"// FILE: {e.path}\n{_strip_code_fences(e.contents)}"
                for e in result.files
            )
            inp = dataclasses.replace(
                inp,
                repair_context=(
                    f"{exc}\n\nRewrite the task so none of these apply. The "
                    f"files you produced:\n\n{produced}"
                ),
            )
            result = self._run_agent_with_retry(task.id, inp)
            files_written_abs, files_written_rel = self._write_files(
                task, result, dep_headers=dep_headers
            )

        # Update the .codetask.json sidecar with the final files[]
        # content so downstream tools (promotion stage) see exactly
        # what shipped. Place sidecar next to the first generated file.
        sidecar_path = self._sidecar_path(task, files_written_abs)
        sidecar_task = task.model_copy(
            update={
                "files": [
                    CodeTaskFile(path=rel, contents=_read_text(abs_path))
                    for rel, abs_path in zip(files_written_rel, files_written_abs)
                ],
            }
        )
        sidecar_path.parent.mkdir(parents=True, exist_ok=True)
        sidecar_path.write_text(
            sidecar_task.model_dump_json(indent=2), encoding="utf-8"
        )
        with self._produced_lock:
            self.produced.extend(files_written_rel)
            self.produced.append(
                str(sidecar_path.relative_to(self.repo_root)).replace("\\", "/")
            )

        return files_written_rel, dict(result.exports)

    # ------------------------------------------------------------------
    # Per-file write pipeline
    # ------------------------------------------------------------------
    def _write_files(
        self,
        task: CodeTask,
        result: CodegenAgentResult,
        *,
        dep_headers: frozenset[str] = frozenset(),
    ) -> tuple[list[Path], list[str]]:
        """Validate + write every file in ``result.files``. Returns
        ``(absolute_paths_written, repo-relative posix paths)``.

        Raises if no files were returned, if any path is out of bounds,
        or if any contents fail validation.

        Generated ``.h``/``.hpp`` files are first-class outputs subject
        to the same path and include rules as ``.cpp`` files.  Same-task
        headers are whitelisted so that ``#include "foo_shared.h"`` is
        preserved when the task also emits ``foo_shared.h``. ``dep_headers``
        additionally whitelists headers produced by this task's declared
        ``depends_on`` tasks (same directory, built first), so a dependent
        task may include them.
        """

        if not result.files:
            raise CodegenAgentError(
                f"agent returned 0 files for {task.id!r}; expected at least 1"
            )

        if len(result.files) > 16:
            raise CodegenAgentError(
                f"agent returned {len(result.files)} files for {task.id!r}; max is 16"
            )

        rel_dir = (
            Path("engine/generated") / _KIND_DIR[task.kind] / self.ctx.project_id
        )
        out_dir = self.repo_root / rel_dir
        out_dir.mkdir(parents=True, exist_ok=True)

        # -- Collect the set of header filenames this task produces so
        #    we can whitelist same-task local includes.
        local_headers: frozenset[str] = frozenset(
            Path(e.path).name
            for e in result.files
            if Path(e.path).suffix.lower() in (".h", ".hpp")
        )

        seen_paths: set[str] = set()
        abs_paths: list[Path] = []
        rel_paths: list[str] = []

        # -- First pass: normalise paths and validate includes/content
        #    before writing anything to disk (fail-loudly).
        normalised: list[tuple[str, str]] = []  # (target_rel, cleaned_code)
        all_violations: list[str] = []

        for entry in result.files:
            target_rel = _normalise_target_path(entry.path, task, self.ctx.project_id)
            if target_rel in seen_paths:
                raise CodegenAgentError(
                    f"agent returned duplicate file path {target_rel!r} for {task.id!r}"
                )
            seen_paths.add(target_rel)

            abs_path = (self.repo_root / target_rel).resolve()
            assert_path_allowed(abs_path, self.gen_root)

            code = _strip_code_fences(entry.contents)
            basename = Path(target_rel).name

            # Validate includes (fail-loudly: collect all violations).
            inc_violations = validate_includes(
                code, basename,
                local_headers=local_headers,
                dep_headers=dep_headers,
            )
            all_violations.extend(
                f"CodeTask {task.id!r}: {v}" for v in inc_violations
            )

            # Header-specific content rules.
            is_header = Path(target_rel).suffix.lower() in (".h", ".hpp")
            if is_header:
                hdr_violations = validate_header_content(code, basename)
                all_violations.extend(
                    f"CodeTask {task.id!r}: {v}" for v in hdr_violations
                )

            # Secondary safety-net filter + auto-repair.
            code = filter_cpp_only(
                code, local_headers=local_headers, dep_headers=dep_headers
            )
            code, repair_notes = repair_generated_cpp(code)
            for note in repair_notes:
                self.ctx.emit(
                    ProjectStage.code_generation,
                    "info",
                    f"{task.id} ({target_rel}): auto-repair {note}",
                )

            normalised.append((target_rel, code))

        if all_violations:
            raise CodegenAgentError(
                f"Include / content violations in {task.id!r}:\n"
                + "\n".join(all_violations)
            )

        # Engine-contract rules: things that compile but cannot work.
        kind_value = getattr(task.kind, "value", str(task.kind))
        contract = lint_task_sources(
            kind_value, {Path(rel).name: code for rel, code in normalised}
        )
        if contract:
            raise CodegenContractError(
                f"Engine contract violations in {task.id!r}:\n"
                + "\n".join(f"- {c}" for c in contract)
            )

        # -- Second pass: write validated files to disk.
        for target_rel, code in normalised:
            abs_path = (self.repo_root / target_rel).resolve()
            abs_path.parent.mkdir(parents=True, exist_ok=True)
            abs_path.write_text(code.rstrip() + "\n", encoding="utf-8")

            abs_paths.append(abs_path)
            rel_paths.append(target_rel)
            self.ctx.emit(
                ProjectStage.code_generation,
                "info",
                f"{task.id} -> {target_rel}",
            )

        return abs_paths, rel_paths

    def _sidecar_path(
        self, task: CodeTask, files_written_abs: list[Path]
    ) -> Path:
        """Place the .codetask.json sidecar in the task's output
        directory under a deterministic name."""
        rel_dir = (
            Path("engine/generated") / _KIND_DIR[task.kind] / self.ctx.project_id
        )
        out_dir = self.repo_root / rel_dir
        return out_dir / f"{_safe_filename(task.id)}.codetask.json"

    # ------------------------------------------------------------------
    # Compile verification + repair
    # ------------------------------------------------------------------
    def verify_and_repair(
        self, *, verifier: CompileVerifier, max_rounds: int
    ) -> tuple[bool, str, list[str]]:
        """Build the generated C++ and repair tasks whose files fail to
        compile.

        Loops: build -> on failure map compiler errors to owning tasks ->
        re-run those tasks' agents with the errors as ``repair_context`` ->
        rebuild. Returns ``(ok, last_build_log, still_failing_task_ids)``.
        ``still_failing_task_ids`` is empty on success; on failure it lists
        the tasks the errors mapped to (empty if none could be mapped).
        """
        last_log = ""
        for rnd in range(0, max_rounds + 1):
            self.ctx.raise_if_cancelled()
            self.ctx.emit(
                ProjectStage.code_generation,
                "verify",
                f"compile check (round {rnd}/{max_rounds})",
            )
            ok, log = verifier()
            last_log = log
            if ok:
                self.ctx.emit(
                    ProjectStage.code_generation,
                    "verify_ok",
                    f"generated C++ compiled (round {rnd})",
                )
                return True, log, []

            failing = self._failing_tasks_from_log(log)
            excerpt = _error_excerpt(log)
            self.ctx.emit(
                ProjectStage.code_generation,
                "verify_error",
                f"round {rnd}: {excerpt[:200]}",
            )

            # Out of budget, or errors we cannot attribute to a task: stop.
            if rnd >= max_rounds:
                return False, log, list(failing.keys())
            if not failing:
                return False, log, []

            for tid, errs in failing.items():
                self._repair_one_task(tid, errs, log)

        return False, last_log, []

    def _failing_tasks_from_log(self, log: str) -> dict[str, list[str]]:
        """Map compiler AND linker error lines to the codegen task that owns
        the file.

        Builds ``file -> task_id`` (source paths + basenames) and
        ``stem -> task_id`` (basename without extension) indexes from every
        task's ``files_written``, then attributes each error line to a task
        by the source path it names (compiler errors, ``foo.cpp(42):``) or
        by the object file it names (MSVC linker errors, ``foo.obj : error
        LNK2019: ...``). Returns ``{task_id: [error lines]}``."""
        file_to_task: dict[str, str] = {}
        stem_to_task: dict[str, str] = {}
        for tid in self.tasks_by_id:
            row = self.board.task(tid) or {}
            for f in row.get("files_written", []):
                fp = str(f).replace("\\", "/").lower()
                base = fp.rsplit("/", 1)[-1]
                file_to_task[fp] = tid
                file_to_task[base] = tid
                stem_to_task[base.rsplit(".", 1)[0]] = tid

        per_task: dict[str, list[str]] = {}
        for line in log.splitlines():
            low = line.lower()
            is_compile_err = (
                ": error" in low or "error c" in low or "fatal error" in low
            )
            # MSVC linker errors (LNK2019/LNK2001 unresolved external, etc.)
            # name an .obj, not a source file: "foo.obj : error LNK2019: ...".
            is_link_err = "lnk" in low or "unresolved external" in low
            if not (is_compile_err or is_link_err):
                continue

            tid: Optional[str] = None

            # 1) Source-file attribution (compiler errors).
            # MSVC: C:\path\file.cpp(42): error C2065: ...
            # clang/gcc: /path/file.cpp:42:5: error: ...
            m = re.search(r"([A-Za-z]:[^()<>|]+?\.(?:cpp|h|hpp))\(\d+", line)
            if not m:
                m = re.search(r"([^\s:<>|]+\.(?:cpp|h|hpp)):\d+", line)
            if m:
                token = m.group(1).replace("\\", "/").lower()
                base = token.rsplit("/", 1)[-1]
                for fp, owner in file_to_task.items():
                    if "/" in fp and token.endswith(fp):
                        tid = owner
                        break
                if tid is None:
                    tid = file_to_task.get(base)

            # 2) Object-file attribution (linker errors).
            if tid is None:
                for om in re.finditer(r"([\w\-]+)\.obj\b", line, re.IGNORECASE):
                    cand = stem_to_task.get(om.group(1).lower())
                    if cand is not None:
                        tid = cand
                        break

            if tid:
                per_task.setdefault(tid, []).append(line.strip())
        return per_task

    def _dependency_contract_block(self, task_id: str) -> str:
        """Build a repair-context block with the shared headers + ledger
        exports of this task's declared dependencies, so the agent fixes
        cross-task symbol mismatches against the REAL declarations."""
        task = self.tasks_by_id.get(task_id)
        if task is None or not task.depends_on:
            return ""
        snapshot = self.board.snapshot()
        tasks = snapshot.get("tasks", {})
        blocks: list[str] = []
        for dep in task.depends_on:
            row = tasks.get(dep) or {}
            for rel in row.get("files_written", []):
                low = str(rel).lower()
                if not (low.endswith(".h") or low.endswith(".hpp")):
                    continue
                abs_path = (self.repo_root / rel).resolve()
                try:
                    text = abs_path.read_text(encoding="utf-8")
                except OSError:
                    continue
                if len(text) > 16_000:
                    text = text[:16_000] + "\n// ... [truncated] ...\n"
                blocks.append(f"----- {rel} (dependency '{dep}') -----\n{text}")
        if not blocks:
            return ""
        return (
            "\n\nDEPENDENCY CONTRACT -- use these EXACT type names and function "
            "signatures from your depends_on tasks (do NOT invent different "
            "names):\n" + "\n\n".join(blocks)
        )

    def _repair_one_task(
        self, task_id: str, errors: list[str], full_log: str
    ) -> None:
        """Re-run one task's agent with the compiler errors + current file
        contents so it fixes the existing files, then update the board."""
        row = self.board.task(task_id) or {}
        current_files: list[str] = []
        for rel in row.get("files_written", []):
            abs_path = (self.repo_root / rel).resolve()
            try:
                text = abs_path.read_text(encoding="utf-8")
            except OSError:
                continue
            if len(text) > 40_000:
                text = text[:40_000] + "\n// ... [truncated] ...\n"
            current_files.append(f"----- {rel} -----\n{text}")

        # Dependency contract: the shared headers + ledger exports of every
        # task this one depends on. Many compile failures are cross-task
        # symbol mismatches (calling a dependency's function/struct by an
        # invented name, e.g. ``ProjectileSpawnParams`` vs the real
        # ``ProjectileSpawnRequest``). Showing the dependency's ACTUAL header
        # lets the agent use the exact declared names/signatures.
        dep_contract = self._dependency_contract_block(task_id)

        repair_context = (
            "COMPILER ERRORS for this task:\n"
            + "\n".join(errors[:60])
            + "\n\nFULL BUILD LOG TAIL:\n"
            + _tail(full_log, 8_000)
            + dep_contract
            + "\n\nCURRENT FILES ON DISK (fix these -- re-emit ALL of them as "
            "a consistent set; if you change a name/signature in the shared "
            "header, update every .cpp that uses it):\n"
            + ("\n\n".join(current_files) or "(none found)")
        )

        self.ctx.emit(
            ProjectStage.code_generation,
            "task_repair",
            f"{task_id}: {len(errors)} compiler error(s)",
        )
        try:
            files_written, exports = self._process_one_task(
                agent_id="repair",
                task_id=task_id,
                repair_context=repair_context,
            )
            self.board.mark_done(
                task_id, files_written=files_written, exports=exports
            )
            self.ctx.emit(
                ProjectStage.code_generation,
                "task_repaired",
                f"{task_id} files={len(files_written)}",
            )
        except Exception as exc:  # noqa: BLE001 - stamp + let verify re-check
            self.board.mark_failed(task_id, error=f"repair failed: {exc}")
            self.ctx.emit(
                ProjectStage.code_generation,
                "task_failed",
                f"{task_id} repair ({type(exc).__name__}: {exc})"[:240],
            )


# ---------------------------------------------------------------------------
# Path / text helpers
# ---------------------------------------------------------------------------


def _safe_filename(raw: str) -> str:
    s = re.sub(r"[^a-zA-Z0-9_-]+", "_", raw)
    return s[:48] or "module"


def _strip_code_fences(text: str) -> str:
    """Strip the most common markdown fences the model emits even when
    structured output is requested. This is defensive: structured
    outputs SHOULD return raw text inside the JSON string, but real
    runs occasionally include ``` fences inside the contents field."""
    t = text.strip()
    if t.startswith("```"):
        # Remove the opening fence (optionally with a language tag) and
        # the trailing fence if present.
        t = re.sub(r"^```[a-zA-Z0-9_+\-]*\n", "", t)
        if t.endswith("```"):
            t = t[:-3]
    return t


def _normalise_target_path(raw_path: str, task: CodeTask, project_id: str) -> str:
    """Coerce an agent-supplied path into a canonical
    ``engine/generated/<kind>/<project_id>/<name>`` form.

    Accepted extensions: ``.cpp``, ``.h``, ``.hpp``.

    The agent may emit:
      * a bare filename (``shooting_hub_register.cpp``, ``shared.h``)
      * a project-relative path (``mechanics/proj/foo.cpp``)
      * the full path (``engine/generated/mechanics/<id>/foo.cpp``)

    We always rewrite the prefix to enforce the right kind directory
    and project id; ``assert_path_allowed`` does the final boundary
    check after this normalisation.
    """
    kind_dir = _KIND_DIR[task.kind]
    prefix = f"engine/generated/{kind_dir}/{project_id}"

    p = raw_path.replace("\\", "/").strip()
    # Strip any leading slashes the agent might emit.
    while p.startswith("/"):
        p = p[1:]

    if p.startswith(prefix + "/"):
        rel_inside = p[len(prefix) + 1 :]
    elif p.startswith("engine/generated/"):
        # Same engine/generated tree but possibly wrong kind or project.
        # Re-attach to the canonical prefix using just the filename.
        rel_inside = Path(p).name
    else:
        # Bare filename or unrelated prefix.
        rel_inside = Path(p).name

    if not rel_inside:
        rel_inside = f"{_safe_filename(task.id)}.cpp"

    # Light sanitisation: forbid backreferences.
    if ".." in Path(rel_inside).parts:
        raise CodegenAgentError(
            f"agent returned path with '..' segment for {task.id!r}: {raw_path!r}"
        )

    return f"{prefix}/{rel_inside}"


def _read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def _dep_header_names(
    board_snapshot: dict[str, Any], depends_on: list[str]
) -> frozenset[str]:
    """Bare header filenames (``*.h`` / ``*.hpp``) written by each task in
    ``depends_on``, read from a board snapshot's ``files_written``. These
    headers live in the same project directory and are guaranteed built
    before the dependent task, so the dependent task may ``#include`` them.
    """
    tasks = board_snapshot.get("tasks", {})
    out: set[str] = set()
    for dep in depends_on:
        row = tasks.get(dep)
        if not row:
            continue
        for f in row.get("files_written", []):
            name = Path(str(f).replace("\\", "/")).name
            if name.lower().endswith((".h", ".hpp")):
                out.add(name)
    return frozenset(out)


def _tail(text: str, n: int) -> str:
    return text[-n:] if len(text) > n else text


def _error_excerpt(log_text: str) -> str:
    """First recognisable compiler/linker error line, else the last line."""
    for line in log_text.splitlines():
        low = line.lower()
        if ": error" in low or "error c" in low or "fatal error" in low:
            return line.strip()
    lines = [ln for ln in log_text.strip().splitlines() if ln.strip()]
    return lines[-1] if lines else ""


# ---------------------------------------------------------------------------
# Default compile verifier: build the ptv_runtime target
# ---------------------------------------------------------------------------


def _verify_all_targets(ctx: PipelineContext) -> tuple[bool, str]:
    """MSVC build-and-link, then (Quest projects) a clang syntax pass.

    Clang only runs once MSVC is clean, so one repair round never mixes the
    two compilers' opinions of the same line."""
    ok, log = _default_verify_compile(ctx)
    if not ok:
        return ok, log
    try:
        device = ctx.store.load_state(ctx.project_id).target_device
    except Exception:  # noqa: BLE001
        device = None
    if device != TargetDevice.quest or not getattr(
            ctx.settings, "codegen_quest_syntax_check", True):
        return ok, log
    clang = quest_syntax_check(ctx)
    if clang is None:
        return ok, log
    clang_ok, clang_log = clang
    return clang_ok, log + "\n" + clang_log


#: Kept in step with PTV_COMPILE_DEFS / PTV_COMPILE_OPTS in
#: engine/android/app/src/main/cpp/CMakeLists.txt.
_ANDROID_DEFINES = (
    "GLM_FORCE_DEPTH_ZERO_TO_ONE", "GLM_FORCE_RADIANS", "GLM_ENABLE_EXPERIMENTAL",
    "VK_NO_PROTOTYPES", "VK_USE_PLATFORM_ANDROID_KHR", "XR_USE_GRAPHICS_API_VULKAN",
    "XR_USE_PLATFORM_ANDROID", 'PTV_SHADER_DIR="shaders"', 'PTV_ENGINE_ASSET_DIR="engine"',
)
_ANDROID_WARNING_FLAGS = (
    "-Wno-nullability-completeness", "-Wno-nullability-extension",
    "-Wno-deprecated-literal-operator",
)


def resolve_ndk(settings: Any) -> Optional[Path]:
    """The NDK the Quest build uses: ``android_ndk_home``, ``$ANDROID_NDK_HOME``,
    then ``<sdk>/ndk/<android_ndk_version>`` with the SDK from
    ``android_sdk_root``, ``$ANDROID_HOME`` or the Android Studio default."""
    candidates: list[Path] = []
    for value in (getattr(settings, "android_ndk_home", None),
                  os.environ.get("ANDROID_NDK_HOME")):
        if value:
            candidates.append(Path(value))
    version = str(getattr(settings, "android_ndk_version", "") or "")
    sdk_roots = [getattr(settings, "android_sdk_root", None), os.environ.get("ANDROID_HOME"),
                 os.environ.get("ANDROID_SDK_ROOT")]
    if os.environ.get("LOCALAPPDATA"):
        sdk_roots.append(str(Path(os.environ["LOCALAPPDATA"]) / "Android" / "Sdk"))
    for root in sdk_roots:
        if root and version:
            candidates.append(Path(root) / "ndk" / version)
    for c in candidates:
        if _ndk_clang(c) is not None:
            return c
    return None


def _ndk_clang(ndk: Path) -> Optional[Path]:
    prebuilt = ndk / "toolchains" / "llvm" / "prebuilt"
    for host in ("windows-x86_64", "linux-x86_64", "darwin-x86_64"):
        for name in ("clang++.exe", "clang++"):
            exe = prebuilt / host / "bin" / name
            if exe.is_file():
                return exe
    return None


def quest_syntax_check(ctx: PipelineContext) -> Optional[tuple[bool, str]]:
    """Compile every generated .cpp of this game with the NDK's clang,
    syntax only. Returns ``(ok, log)``, or None when it cannot run (no NDK).

    Third-party headers come from the project's desktop vcpkg tree (EnTT,
    GLM, nlohmann, VMA are header-only and identical); the compile-verify
    build has already installed it."""
    ndk = resolve_ndk(ctx.settings)
    clang = _ndk_clang(ndk) if ndk else None
    if clang is None:
        ctx.emit(ProjectStage.code_generation, "warning",
                 "Quest syntax check skipped: no Android NDK found "
                 "(set ANDROID_HOME or android_ndk_home)")
        return None
    repo_root: Path = ctx.settings.repo_root
    gen_root = repo_root / "engine" / "generated"
    sources: list[Path] = []
    for kind in ("mechanics", "ai", "dialogue"):
        d = gen_root / kind / ctx.project_id
        if d.is_dir():
            sources.extend(sorted(d.glob("*.cpp")))
    extra = dict(generated_sources_cache(ctx)).get("PTV_GENERATED_EXTRA_SOURCES", "")
    sources.extend(Path(p) for p in extra.split(";") if p)
    if not sources:
        return True, "quest syntax check: no generated sources"

    vcpkg_inc = (ctx.project_dir / "builds" / "cmake-current" / "vcpkg_installed"
                 / "x64-windows" / "include")
    includes = [ctx.settings.engine_dir / "runtime" / "include",
                ctx.settings.engine_dir / "runtime" / "src"]
    if vcpkg_inc.is_dir():
        includes.append(vcpkg_inc)
        if (vcpkg_inc / "vma").is_dir():
            includes.append(vcpkg_inc / "vma")
    base_cmd = [str(clang), "--target=aarch64-linux-android32", "-std=c++20",
                "-fsyntax-only", *_ANDROID_WARNING_FLAGS]
    base_cmd += [f"-D{d}" for d in _ANDROID_DEFINES]
    base_cmd += [f"-I{p}" for p in includes]

    ctx.emit(ProjectStage.code_generation, "verify",
             f"Quest compiler check: {len(sources)} file(s) with {clang.name}")

    def _one(src: Path) -> tuple[Path, int, str]:
        try:
            proc = subprocess.run(base_cmd + [str(src)], capture_output=True, text=True,
                                  encoding="utf-8", errors="replace", timeout=300)
        except subprocess.TimeoutExpired:
            return src, 1, f"{src}: error: quest syntax check timed out"
        return src, proc.returncode, (proc.stdout or "") + (proc.stderr or "")

    results: list[tuple[Path, int, str]] = []
    with ThreadPoolExecutor(max_workers=max(1, min(8, os.cpu_count() or 4))) as pool:
        for res in pool.map(_one, sources):
            results.append(res)
    failed = [(src, out) for src, code, out in results if code != 0]
    lines = [f"quest syntax check ({clang}): {len(sources) - len(failed)}/{len(sources)} ok"]
    seen: set[str] = set()  # a header's errors repeat once per includer
    for src, out in failed:
        lines.append(f"--- {src.name}")
        for l in out.splitlines():
            if not (" error:" in l or "fatal error:" in l or "note:" in l):
                continue
            if l in seen:
                continue
            seen.add(l)
            lines.append(l)
    log = "\n".join(lines)
    for line in lines[:40]:
        ctx.emit(ProjectStage.code_generation, "log", line[:400])
    return not failed, log


def generated_sources_cache(ctx: PipelineContext) -> tuple[tuple[str, str], ...]:
    """CMake cache entries selecting THIS game's generated C++ (plus any
    promoted mechanics it reuses). Without them the build would compile every
    game's code together, and two games' same-named tasks collide at link."""
    from ..engine_catalog import promoted_sources_for_plan

    try:
        plan = ctx.mechanic_plan or ctx.store.load_mechanic_plan(ctx.project_id)
    except Exception:  # noqa: BLE001 - no plan, no promoted reuse
        plan = None
    extra = promoted_sources_for_plan(ctx.settings.repo_root, plan, ctx.project_id)
    return (
        ("PTV_GENERATED_PROJECT_ID", ctx.project_id),
        ("PTV_GENERATED_EXTRA_SOURCES", ";".join(extra)),
    )


def _default_verify_compile(ctx: PipelineContext) -> tuple[bool, str]:
    """Configure the engine and build the ``ptv_starter_room`` executable,
    which links ``ptv_runtime`` (with every generated mechanic force-linked
    via /WHOLEARCHIVE). Returns ``(ok, combined_log)``.

    Building the EXE (not just the ``ptv_runtime`` static lib) is deliberate:
    a static-library build never links, so it would miss cross-task
    ``unresolved external symbol`` errors -- e.g. a weapon profile or hub
    that calls a dependency's function with a mismatched signature. Those
    only surface when an executable links the archive, which is exactly what
    breaks the final playable build. Catching them here lets the repair loop
    fix them.

    Reuses the same ``builds/cmake-current`` dir the final ``cmake_build``
    stage uses, so vcpkg is shared and the later full build is incremental.
    The project's ``scene.json`` is not required to build the exe (the
    starter-room ships its own default scene).
    """
    build_dir = ctx.project_dir / "builds" / "cmake-current"
    log_path = ctx.project_dir / "logs" / "codegen_verify.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)
    logs: list[str] = []

    def _sink(line: str) -> None:
        logs.append(line)
        ctx.emit(ProjectStage.code_generation, "log", line[:400])

    result = configure_and_build(
        source_dir=ctx.settings.engine_dir,
        build_dir=build_dir,
        generator=ctx.settings.cmake_generator,
        platform=ctx.settings.cmake_generator_platform,
        config=ctx.settings.cmake_build_config,
        vcpkg_root=ctx.settings.vcpkg_root,
        extra_cache=generated_sources_cache(ctx),
        build_target="ptv_starter_room",
        on_line=_sink,
        # Force a full recompile of the generated sources. Without this, an
        # incremental build can reuse a stale ``.obj`` from before a repair
        # rewrote a generated file, reporting a FALSE GREEN while the current
        # on-disk C++ actually fails to compile (exactly the shooting_hub
        # partial-repair regression). The verify MUST reflect current files.
        clean_first=True,
        **proc_hooks(ctx),
    )
    text = "\n".join(logs)
    try:
        log_path.write_text(text, encoding="utf-8")
    except OSError:
        pass
    return result.exit_code == 0, text


# ---------------------------------------------------------------------------
# Agent context excerpts
# ---------------------------------------------------------------------------


def _scene_context_json(
    task: CodeTask,
    scenes: list[SceneSpec],
    sockets_by_asset: dict[str, list[str]] | None = None,
) -> str:
    """Build a JSON string with the SceneSpec excerpts that touch this
    task. For each scene we include:

    * scene_id, name, player_spawn
    * all entities whose mechanic_bindings reference this task
      (id == generated.<task_id> or id == <task_id>)
    * environment / player_avatar entities for ambient context
    * tag glossary (unique tag set across the scene)
    """

    excerpts: list[dict[str, Any]] = []
    task_id = task.id
    target_ids = {f"generated.{task_id}", task_id}

    for scene in scenes:
        relevant_entities: list[dict[str, Any]] = []
        ambient_entities: list[dict[str, Any]] = []
        tags_present: set[str] = set()

        for ent in scene.entities:
            tags_present.update(ent.tags)
            touches = any(
                b.mechanic_id in target_ids for b in ent.mechanic_bindings
            )
            is_ambient = any(
                t in ent.tags for t in ("environment", "player_avatar")
            )
            payload = _entity_summary(ent, sockets_by_asset or {})
            if touches:
                relevant_entities.append(payload)
            elif is_ambient:
                ambient_entities.append(payload)

        excerpts.append(
            {
                "scene_id": scene.scene_id,
                "name": scene.name,
                "player_spawn": _model_to_jsonable(scene.player_spawn),
                "relevant_entities": relevant_entities,
                "ambient_entities": ambient_entities[:8],  # cap for token budget
                "all_tags_in_scene": sorted(tags_present),
            }
        )

    return json.dumps(excerpts, indent=2, sort_keys=True)


def _entity_summary(
    ent, sockets_by_asset: dict[str, list[str]] | None = None
) -> dict[str, Any]:  # ent: EntitySpec
    mesh = _model_to_jsonable(ent.mesh) if ent.mesh else None
    if mesh is not None and sockets_by_asset:
        # Socket TRANSFORMS are per-asset data the agent must never see or
        # guess; socket NAMES are the contract it writes against. Surface the
        # names only.
        names = sockets_by_asset.get(ent.mesh.asset_id)
        if names:
            mesh["available_sockets"] = names
    return {
        "id": ent.id,
        "name": ent.name,
        "tags": list(ent.tags),
        "transform": _model_to_jsonable(ent.transform),
        "mesh": mesh,
        "physics": _model_to_jsonable(ent.physics) if ent.physics else None,
        "mechanic_bindings": [
            _model_to_jsonable(b) for b in ent.mechanic_bindings
        ],
    }


def _model_to_jsonable(value: Any) -> Any:
    if value is None:
        return None
    if hasattr(value, "model_dump"):
        return value.model_dump(mode="json")
    return value


def _buildspec_excerpt(task: CodeTask, bs: Optional[BuildSpec]) -> str:
    """Return a short text excerpt from the BuildSpec covering the
    mechanic this task implements plus any acceptance tests that
    mention it. Falls back to ``"(no BuildSpec available)"``."""

    if bs is None:
        return "(no BuildSpec available)"

    matches_id = {task.id, f"generated.{task.id}"}

    lines: list[str] = []
    lines.append(f"BuildSpec.title: {bs.title}")
    lines.append(f"BuildSpec.pitch: {bs.pitch[:240]}")

    mechanic_blob: list[str] = []
    for m in bs.mechanics:
        # Exact-id match only. Substring matching used to produce false
        # positives (e.g. ``rep`` in ``repulsor_blast``).
        if m.id in matches_id:
            mechanic_blob.append(
                f"  - mechanic_id={m.id} name={m.name}\n"
                f"    summary: {(m.summary or '')[:2_000]}"
            )
    if mechanic_blob:
        lines.append("Matching BuildSpec mechanic entries:")
        lines.extend(mechanic_blob)
    else:
        lines.append(
            "(no exact BuildSpec mechanic match -- use the CodeTask description)"
        )

    test_blob: list[str] = []
    needle = task.id.lower().replace("_", " ")
    for t in bs.acceptance_tests:
        desc = (t.description or "").lower()
        if needle in desc or task.id.lower() in desc:
            test_blob.append(f"  - acceptance_test {t.id}: {t.description[:500]}")
    if test_blob:
        lines.append("Related acceptance tests:")
        lines.extend(test_blob)

    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Test injection helpers (kept exported for direct test use)
# ---------------------------------------------------------------------------


__all__ = [
    "run",
    "run_with_runner",
]
