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
  repair loop** builds the ``ptv_runtime`` target (which globs in every
  generated mechanic). Include validation only checks *includes*, not
  that the C++ compiles -- this loop closes that gap. On a compile
  failure the compiler errors are mapped back to the owning task and fed
  to that task's agent as a ``repair_context`` so it regenerates the
  file; the stage then rebuilds, up to ``codegen_max_repair_rounds``
  times. Controlled by ``settings.codegen_verify_compile``.

The CodeTask ``test_files[]`` field is intentionally unused: no disk
writes, no CMake test targets, no planner prompts for tests.
"""

from __future__ import annotations

import json
import os
import re
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
from .context import PipelineContext, StageResult
from .prompts import CODE_TASK_SYSTEM


# Maps ``CodeTaskKind`` -> directory under ``engine/generated/``.
_KIND_DIR: dict[CodeTaskKind, str] = {
    CodeTaskKind.mechanic: "mechanics",
    CodeTaskKind.ai: "ai",
    CodeTaskKind.dialogue: "dialogue",
}


# A compile verifier builds the generated C++ and reports ``(ok, log)``.
# Production uses ``_default_verify_compile``; tests inject a stub.
CompileVerifier = Callable[[], "tuple[bool, str]"]


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
        project_dir, plan, max_agents=max_agents
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
    model_id = getattr(ctx.settings, "openai_hard_model", "gpt-5")
    runtime = _StageRuntime(
        ctx=ctx,
        board=board,
        tasks_by_id=tasks_by_id,
        code_edit_base_by_task_id=code_edit_base_by_task_id,
        static=static_ctx,
        gen_root=gen_root,
        repo_root=repo_root,
        model_id=model_id,
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
    verify_excerpt = ""
    if (
        not pre_failed
        and not pre_blocked
        and getattr(ctx.settings, "codegen_verify_compile", True)
        and runtime.produced
    ):
        verifier = compile_verifier or (lambda: _default_verify_compile(ctx))
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

    return _StaticContext(
        build_spec=build_spec,
        scene_specs=scene_specs,
        scene_files=scene_files,
    )


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
        self.agent_runner = agent_runner

        self.produced: list[str] = []
        self._produced_lock = threading.Lock()

    # ------------------------------------------------------------------
    # Worker loop
    # ------------------------------------------------------------------
    def worker_loop(self, agent_id: str) -> None:
        """Pull-based work loop. Each iteration refreshes blocked rows,
        atomically claims the next pending task, runs the agent, writes
        files, and marks done/failed. Exits when no task is claimable.
        """

        while True:
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
                self.board.mark_failed(task_id, error=err)
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
        scene_ctx_json = _scene_context_json(task, self.static.scene_specs)
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
            instructions=CODE_TASK_SYSTEM,
            base_source_block=base_source_block,
            repair_context=repair_context,
            extra_instructions=extra,
        )

        result = run_codegen_agent(
            inp, model=self.model_id, runner=self.agent_runner
        )

        files_written_abs, files_written_rel = self._write_files(task, result)

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
        self, task: CodeTask, result: CodegenAgentResult
    ) -> tuple[list[Path], list[str]]:
        """Validate + write every file in ``result.files``. Returns
        ``(absolute_paths_written, repo-relative posix paths)``.

        Raises if no files were returned, if any path is out of bounds,
        or if any contents fail validation.

        Generated ``.h``/``.hpp`` files are first-class outputs subject
        to the same path and include rules as ``.cpp`` files.  Same-task
        headers are whitelisted so that ``#include "foo_shared.h"`` is
        preserved when the task also emits ``foo_shared.h``.
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
                code, basename, local_headers=local_headers,
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
            code = filter_cpp_only(code, local_headers=local_headers)
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
        """Map compiler error lines to the codegen task that owns the file.

        Builds a ``file -> task_id`` index from every task's
        ``files_written`` (repo-relative posix + basename), then attributes
        each error line to a task by the source path it names. Returns
        ``{task_id: [error lines]}``."""
        file_to_task: dict[str, str] = {}
        for tid in self.tasks_by_id:
            row = self.board.task(tid) or {}
            for f in row.get("files_written", []):
                fp = str(f).replace("\\", "/").lower()
                file_to_task[fp] = tid
                file_to_task[fp.rsplit("/", 1)[-1]] = tid  # basename

        per_task: dict[str, list[str]] = {}
        for line in log.splitlines():
            low = line.lower()
            if not (": error" in low or "error c" in low or "fatal error" in low):
                continue
            # MSVC: C:\path\file.cpp(42): error C2065: ...
            # clang/gcc: /path/file.cpp:42:5: error: ...
            m = re.search(r"([A-Za-z]:[^()<>|]+?\.(?:cpp|h|hpp))\(\d+", line)
            if not m:
                m = re.search(r"([^\s:<>|]+\.(?:cpp|h|hpp)):\d+", line)
            token = m.group(1).replace("\\", "/").lower() if m else ""
            tid: Optional[str] = None
            if token:
                base = token.rsplit("/", 1)[-1]
                for fp, owner in file_to_task.items():
                    if "/" in fp and token.endswith(fp):
                        tid = owner
                        break
                if tid is None:
                    tid = file_to_task.get(base)
            if tid:
                per_task.setdefault(tid, []).append(line.strip())
        return per_task

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

        repair_context = (
            "COMPILER ERRORS for this task:\n"
            + "\n".join(errors[:60])
            + "\n\nFULL BUILD LOG TAIL:\n"
            + _tail(full_log, 8_000)
            + "\n\nCURRENT FILES ON DISK (fix these):\n"
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


def _default_verify_compile(ctx: PipelineContext) -> tuple[bool, str]:
    """Configure the engine and build ONLY the ``ptv_runtime`` target,
    which globs in every generated mechanic. Returns ``(ok, combined_log)``.

    Reuses the same ``builds/cmake-current`` dir the final ``cmake_build``
    stage uses, so vcpkg is shared and the later full build is incremental.
    ``scene.json`` is not required to compile the library.
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
        extra_cache=(("PTV_GENERATED_PROJECT_ID", ctx.project_id),),
        build_target="ptv_runtime",
        on_line=_sink,
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


def _scene_context_json(task: CodeTask, scenes: list[SceneSpec]) -> str:
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
            payload = _entity_summary(ent)
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


def _entity_summary(ent) -> dict[str, Any]:  # ent: EntitySpec
    return {
        "id": ent.id,
        "name": ent.name,
        "tags": list(ent.tags),
        "transform": _model_to_jsonable(ent.transform),
        "mesh": _model_to_jsonable(ent.mesh) if ent.mesh else None,
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
