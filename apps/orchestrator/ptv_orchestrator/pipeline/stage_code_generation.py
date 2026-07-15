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
from typing import Any, Optional

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


# ---------------------------------------------------------------------------
# Public entry point (matches the StageFn signature in orchestrator.py)
# ---------------------------------------------------------------------------


def run(ctx: PipelineContext) -> StageResult:
    """Standard stage entry point used by the orchestrator."""
    return run_with_runner(ctx, agent_runner=None)


def run_with_runner(
    ctx: PipelineContext, agent_runner: Optional[RunnerFn] = None
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
        self, *, agent_id: str, task_id: str
    ) -> tuple[list[str], dict[str, Any]]:
        """Run the agent for ``task_id``, validate + write its files,
        return ``(files_written_rel_paths, exports)``."""

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
