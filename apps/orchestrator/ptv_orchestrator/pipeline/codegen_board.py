"""Codegen ledger / work-queue board.

The board is the single source of truth for the multi-agent code-generation
stage. It is persisted to ``projects/<project_id>/codegen_board.json`` and
read + mutated by every worker in the ``stage_code_generation`` thread pool.

Design notes
------------

* Each ``CodeTask`` in the MechanicPlan becomes one row keyed by ``task_id``.
* Rows carry a ``status`` (``pending`` / ``blocked`` / ``in_progress`` /
  ``done`` / ``failed``), a ``depends_on`` list (other task ids), an
  ``agent_id`` set on claim, the ``files_written`` paths the worker wrote
  to disk, and an ``exports`` dict the worker fills in so downstream
  dependent tasks can read its public surface.
* ``atomic_claim_next_pending`` is the only mutation that may be called by
  multiple workers concurrently. It holds an in-process
  ``threading.Lock``, re-runs ``refresh_blocked`` under the lock, then
  picks the first ``pending`` task in MechanicPlan order (FCFS) whose
  deps are all ``done``, flips its status to ``in_progress``, stamps the
  ``agent_id`` / ``claimed_at``, persists with a temp-file + rename, and
  returns the row to the caller. Two workers can never claim the same row.
* The board exposes a ``snapshot()`` deep-copy of its state so agent
  context can be built without taking the lock for the duration of an
  LLM call.

The board is single-process: it relies on the ``ThreadPoolExecutor`` model
used by ``stage_code_generation`` (all workers live in one Python
process), so the in-process ``threading.Lock`` plus an atomic temp+rename
on the JSON file is sufficient. A future multi-process orchestrator would
need an OS-level file lock here.
"""

from __future__ import annotations

import json
import os
import threading
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable

from ptv_schemas import MechanicPlan


_VALID_STATES = {"pending", "blocked", "in_progress", "done", "failed"}


def _iso_now() -> str:
    return datetime.now(tz=timezone.utc).isoformat()


def _deep_copy(value: Any) -> Any:
    """JSON round-trip copy: cheap, deterministic, no shared mutable state."""
    return json.loads(json.dumps(value))


class CodegenBoard:
    """Per-project codegen ledger.

    Instances are created via :meth:`load_or_init`; constructing one
    directly is only for tests.
    """

    VERSION = 1

    def __init__(self, project_dir: Path, max_agents: int = 3) -> None:
        self.project_dir = Path(project_dir)
        self.path = self.project_dir / "codegen_board.json"
        self.max_agents = max_agents
        self._lock = threading.Lock()
        self._data: dict[str, Any] = {
            "version": self.VERSION,
            "max_agents": max_agents,
            "task_order": [],
            "tasks": {},
        }

    # ------------------------------------------------------------------
    # Construction / merge
    # ------------------------------------------------------------------
    @classmethod
    def load_or_init(
        cls,
        project_dir: Path,
        plan: MechanicPlan,
        *,
        max_agents: int = 3,
    ) -> "CodegenBoard":
        """Load an existing board (resume) or initialise a new one from
        ``plan``. New tasks from the plan are appended in plan order.
        Existing rows in ``done`` / ``failed`` / ``in_progress`` are left
        alone; rows in ``pending`` / ``blocked`` have their status
        recomputed against the latest ``depends_on``.
        """
        board = cls(project_dir, max_agents=max_agents)
        if board.path.exists():
            try:
                board._data = json.loads(board.path.read_text(encoding="utf-8"))
            except Exception:
                # A corrupt board JSON should never wedge the stage.
                # Reset to the empty default and rebuild from the plan.
                board._data = {
                    "version": cls.VERSION,
                    "max_agents": max_agents,
                    "task_order": [],
                    "tasks": {},
                }
        # Always honour the latest configured concurrency cap.
        board._data["max_agents"] = max_agents
        board._data.setdefault("task_order", [])
        board._data.setdefault("tasks", {})
        board._merge_plan_locked(plan)
        with board._lock:
            board._refresh_blocked_locked()
            board._save_locked()
        return board

    def _merge_plan_locked(self, plan: MechanicPlan) -> None:
        """Append new tasks from the plan; refresh deps on existing
        non-terminal tasks. Caller does not need to hold the lock yet;
        this only mutates ``_data`` and persists via the public
        ``_save_locked``-protected path."""
        tasks: dict[str, Any] = self._data["tasks"]
        order: list[str] = self._data["task_order"]
        seen_order = set(order)

        # Identify the set of generate-task ids in this plan so we can
        # filter ``depends_on`` references to non-existent tasks (e.g.
        # the planner accidentally targeted a builtin id).
        plan_codegen_ids: set[str] = {
            item.code_task.id for item in plan.items if item.code_task is not None
        }

        for item in plan.items:
            if item.code_task is None:
                continue
            task_id = item.code_task.id
            depends_raw = list(getattr(item.code_task, "depends_on", []) or [])
            # Strip self-references and references to ids that aren't
            # generate tasks. The board only tracks codegen tasks; a
            # planner dep on a builtin is a no-op for ordering.
            depends = [d for d in depends_raw if d in plan_codegen_ids and d != task_id]

            if task_id not in seen_order:
                order.append(task_id)
                seen_order.add(task_id)

            row = tasks.get(task_id)
            if row is None:
                tasks[task_id] = {
                    "mechanic_plan_item_id": item.id,
                    "status": "pending" if not depends else "blocked",
                    "depends_on": depends,
                    "agent_id": None,
                    "claimed_at": None,
                    "completed_at": None,
                    "files_written": [],
                    "exports": {},
                    "error": None,
                }
                continue

            # Existing row: refresh deps + plan link, but never demote a
            # terminal or in-flight task.
            row["mechanic_plan_item_id"] = item.id
            row["depends_on"] = depends
            if row.get("status") not in ("done", "failed", "in_progress"):
                row["status"] = "pending" if not depends else "blocked"

    # ------------------------------------------------------------------
    # Persistence
    # ------------------------------------------------------------------
    def _save_locked(self) -> None:
        """Persist the board to disk atomically. Caller MUST hold
        ``self._lock``."""
        self.path.parent.mkdir(parents=True, exist_ok=True)
        tmp = self.path.with_suffix(self.path.suffix + ".tmp")
        tmp.write_text(
            json.dumps(self._data, indent=2, sort_keys=True),
            encoding="utf-8",
        )
        os.replace(tmp, self.path)

    # ------------------------------------------------------------------
    # Read-only views
    # ------------------------------------------------------------------
    def snapshot(self) -> dict[str, Any]:
        """Deep-copy of the current board JSON. Safe to hand to an
        agent or persist without holding the lock."""
        with self._lock:
            return _deep_copy(self._data)

    def task_order(self) -> list[str]:
        with self._lock:
            return list(self._data.get("task_order", []))

    def task(self, task_id: str) -> dict[str, Any] | None:
        with self._lock:
            row = self._data["tasks"].get(task_id)
            return _deep_copy(row) if row is not None else None

    def status_counts(self) -> dict[str, int]:
        with self._lock:
            counts: dict[str, int] = {s: 0 for s in _VALID_STATES}
            for row in self._data["tasks"].values():
                s = row.get("status", "pending")
                counts[s] = counts.get(s, 0) + 1
            return counts

    def has_remaining_work(self) -> bool:
        with self._lock:
            for row in self._data["tasks"].values():
                if row.get("status") in ("pending", "blocked", "in_progress"):
                    return True
            return False

    # ------------------------------------------------------------------
    # DAG maintenance
    # ------------------------------------------------------------------
    def _refresh_blocked_locked(self) -> list[str]:
        """Promote blocked->pending for every row whose deps are all
        done. Returns the list of task ids that transitioned. Caller
        MUST hold ``self._lock``."""
        tasks = self._data["tasks"]
        done_ids = {
            tid for tid, row in tasks.items() if row.get("status") == "done"
        }
        promoted: list[str] = []
        for tid, row in tasks.items():
            if row.get("status") != "blocked":
                continue
            deps = row.get("depends_on", [])
            if all(d in done_ids for d in deps):
                row["status"] = "pending"
                promoted.append(tid)
        return promoted

    def refresh_blocked(self) -> list[str]:
        """Public DAG maintenance entry-point. Workers call this at the
        start of every loop iteration so promotions happen as soon as
        any worker is next able to claim. ``mark_done`` calls it
        internally too (belt-and-suspenders)."""
        with self._lock:
            promoted = self._refresh_blocked_locked()
            if promoted:
                self._save_locked()
            return promoted

    # ------------------------------------------------------------------
    # Atomic claim
    # ------------------------------------------------------------------
    def atomic_claim_next_pending(
        self, agent_id: str
    ) -> dict[str, Any] | None:
        """Atomically claim the next FCFS-eligible pending task.

        Walks ``task_order`` (which is the MechanicPlan item order), and
        returns the first ``pending`` row whose deps are all ``done``.
        Marks the row ``in_progress``, stamps ``agent_id`` and
        ``claimed_at``, persists, and returns a deep-copy of the row with
        an added ``id`` field. Returns ``None`` if nothing is claimable.
        """
        with self._lock:
            # Bring blocked->pending up to date before scanning so a
            # worker that just stamped 'done' picks up its downstream
            # task on the next claim.
            self._refresh_blocked_locked()

            tasks = self._data["tasks"]
            order = self._data.get("task_order", [])
            done_ids = {
                tid for tid, row in tasks.items() if row.get("status") == "done"
            }

            for tid in order:
                row = tasks.get(tid)
                if row is None:
                    continue
                if row.get("status") != "pending":
                    continue
                deps = row.get("depends_on", [])
                if not all(d in done_ids for d in deps):
                    # Defensive: should already be blocked.
                    row["status"] = "blocked"
                    continue
                row["status"] = "in_progress"
                row["agent_id"] = agent_id
                row["claimed_at"] = _iso_now()
                row["error"] = None
                self._save_locked()
                claimed = _deep_copy(row)
                claimed["id"] = tid
                return claimed

            return None

    # ------------------------------------------------------------------
    # Terminal transitions
    # ------------------------------------------------------------------
    def mark_done(
        self,
        task_id: str,
        files_written: Iterable[str],
        exports: dict[str, Any] | None = None,
    ) -> list[str]:
        """Mark ``task_id`` as ``done`` and promote any newly-eligible
        dependents from ``blocked`` to ``pending``. Returns the list of
        promoted task ids."""
        with self._lock:
            row = self._data["tasks"].get(task_id)
            if row is None:
                raise KeyError(f"task {task_id!r} not on board")
            row["status"] = "done"
            row["completed_at"] = _iso_now()
            row["files_written"] = list(files_written)
            if exports is not None:
                row["exports"] = dict(exports)
            row["error"] = None
            promoted = self._refresh_blocked_locked()
            self._save_locked()
            return promoted

    def mark_failed(self, task_id: str, error: str) -> None:
        """Mark ``task_id`` as ``failed`` with an error string. Failed
        tasks do not promote their dependents."""
        with self._lock:
            row = self._data["tasks"].get(task_id)
            if row is None:
                raise KeyError(f"task {task_id!r} not on board")
            row["status"] = "failed"
            row["completed_at"] = _iso_now()
            row["error"] = (error or "")[:2000]
            self._save_locked()

    # ------------------------------------------------------------------
    # Resume helpers
    # ------------------------------------------------------------------
    def stuck_in_progress_ids(self) -> list[str]:
        """Tasks left ``in_progress`` from a previous (crashed) run.
        The caller may reset these to ``pending`` so they can be claimed
        again, or leave them alone for human inspection."""
        with self._lock:
            return [
                tid
                for tid, row in self._data["tasks"].items()
                if row.get("status") == "in_progress"
            ]

    def reset_stuck_in_progress(self) -> list[str]:
        """Reset stuck-in-progress tasks back to ``pending`` (or
        ``blocked`` if their deps are not done). Returns the list of ids
        that were reset."""
        with self._lock:
            reset: list[str] = []
            done_ids = {
                tid
                for tid, row in self._data["tasks"].items()
                if row.get("status") == "done"
            }
            for tid, row in self._data["tasks"].items():
                if row.get("status") != "in_progress":
                    continue
                row["agent_id"] = None
                row["claimed_at"] = None
                deps = row.get("depends_on", [])
                if all(d in done_ids for d in deps):
                    row["status"] = "pending"
                else:
                    row["status"] = "blocked"
                reset.append(tid)
            if reset:
                self._save_locked()
            return reset

    def reset_failed(self) -> list[str]:
        """Reset ``failed`` tasks back to ``pending`` (or ``blocked`` if
        their deps are not done) so they can be retried on resume.
        Returns the list of ids that were reset."""
        with self._lock:
            reset: list[str] = []
            done_ids = {
                tid
                for tid, row in self._data["tasks"].items()
                if row.get("status") == "done"
            }
            for tid, row in self._data["tasks"].items():
                if row.get("status") != "failed":
                    continue
                row["agent_id"] = None
                row["claimed_at"] = None
                row["completed_at"] = None
                row["error"] = None
                deps = row.get("depends_on", [])
                if all(d in done_ids for d in deps):
                    row["status"] = "pending"
                else:
                    row["status"] = "blocked"
                reset.append(tid)
            if reset:
                self._refresh_blocked_locked()
                self._save_locked()
            return reset
