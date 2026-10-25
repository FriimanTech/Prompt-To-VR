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

import hashlib
import json
import threading
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable

from ptv_schemas import MechanicPlan

from ..fsutil import atomic_write_text, unlink_with_retry


_VALID_STATES = {"pending", "blocked", "in_progress", "done", "failed"}

# Source-level fingerprints of a task having consumed the socket contract.
# A task whose generated C++ never mentions any of these cannot have been
# written differently because of a socket, so a contract change leaves it
# alone -- which is the difference between re-billing one mechanic and
# re-billing sixteen.
_SOCKET_API_MARKERS = ("ctx.sockets", "socket_world", "has_socket")


def socket_fingerprint(sockets_by_asset: dict[str, list[str]]) -> str:
    """Stable digest of "which asset offers which sockets".

    Exactly the map the codegen agent is shown (see
    ``CodegenAgentInput.sockets_by_asset``), so the digest changes precisely
    when the agent's socket context would change.
    """
    payload = {
        asset_id: sorted(names)
        for asset_id, names in (sockets_by_asset or {}).items()
        if names
    }
    blob = json.dumps(payload, sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(blob.encode("utf-8")).hexdigest()[:16]


def task_fingerprint(item: Any) -> str:
    """Digest of what a plan item asks the codegen agent to write."""
    payload = {
        "classification": getattr(item.classification, "value", str(item.classification)),
        "base_builtin_id": item.base_builtin_id or item.builtin_id or "",
        "code_task": item.code_task.model_dump(mode="json") if item.code_task else None,
    }
    blob = json.dumps(payload, sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(blob.encode("utf-8")).hexdigest()[:16]


def _iso_now() -> str:
    return datetime.now(tz=timezone.utc).isoformat()


def _deep_copy(value: Any) -> Any:
    """JSON round-trip copy: cheap, deterministic, no shared mutable state."""
    return json.loads(json.dumps(value))


def _row_reads_sockets(row: dict[str, Any], repo_root: Path) -> bool:
    """Did this task's generated C++ consume the socket contract?

    Two sources, either sufficient: the CodeTask declared
    ``required_sockets``, or the source the worker actually wrote mentions
    the socket API. A file that has since been deleted counts as reading
    them -- unverifiable, and regenerating is the cheap side of that bet.
    """
    if row.get("required_sockets"):
        return True
    for rel in row.get("files_written") or []:
        path = Path(rel)
        if not path.is_absolute():
            path = repo_root / path
        try:
            text = path.read_text(encoding="utf-8", errors="ignore")
        except OSError:
            return True
        if any(marker in text for marker in _SOCKET_API_MARKERS):
            return True
    return False


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
        # Set when a save could not reach disk; the in-memory board stays
        # authoritative and the next save (or flush) writes it.
        self._dirty = False
        self.last_save_error: str | None = None
        # What load_or_init pruned or reset, for the stage to report.
        self.dropped_tasks: list[str] = []
        self.deleted_files: list[str] = []
        self.changed_tasks: list[str] = []
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
        repo_root: Path | None = None,
    ) -> "CodegenBoard":
        """Load an existing board (resume) or initialise a new one from
        ``plan``. New tasks from the plan are appended in plan order.
        Existing rows in ``done`` / ``failed`` / ``in_progress`` are left
        alone unless their CodeTask changed; rows in ``pending`` /
        ``blocked`` have their status recomputed against the latest
        ``depends_on``. Rows for tasks the plan no longer has are dropped,
        and (given ``repo_root``) so are the files only they wrote.
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
        board._drop_tasks_not_in_plan(plan, repo_root)
        board._reset_dependents_of_changed()
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
            required_sockets = list(
                getattr(item.code_task, "required_sockets", None) or []
            )

            if task_id not in seen_order:
                order.append(task_id)
                seen_order.add(task_id)

            fingerprint = task_fingerprint(item)
            row = tasks.get(task_id)
            if row is None:
                tasks[task_id] = {
                    "mechanic_plan_item_id": item.id,
                    "status": "pending" if not depends else "blocked",
                    "depends_on": depends,
                    "required_sockets": required_sockets,
                    "task_fingerprint": fingerprint,
                    "agent_id": None,
                    "claimed_at": None,
                    "completed_at": None,
                    "files_written": [],
                    "exports": {},
                    "error": None,
                }
                continue

            # Existing row: refresh deps + plan link, but never demote a
            # terminal or in-flight task -- unless the task it was written
            # for has changed, in which case its code answers a question
            # nobody is asking any more.
            row["mechanic_plan_item_id"] = item.id
            row["depends_on"] = depends
            row["required_sockets"] = required_sockets
            previous = row.get("task_fingerprint")
            row["task_fingerprint"] = fingerprint
            if (previous is not None and previous != fingerprint
                    and row.get("status") in ("done", "failed")):
                row.update(status="pending", agent_id=None, claimed_at=None,
                           completed_at=None, error=None)
                self.changed_tasks.append(task_id)
            if row.get("status") not in ("done", "failed", "in_progress"):
                row["status"] = "pending" if not depends else "blocked"

    def _reset_dependents_of_changed(self) -> None:
        """A task written against exports that are about to be rewritten has
        to be rewritten too (transitively)."""
        if not self.changed_tasks:
            return
        tasks: dict[str, Any] = self._data["tasks"]
        invalid = set(self.changed_tasks)
        growing = True
        while growing:
            growing = False
            for tid, row in tasks.items():
                if tid in invalid or row.get("status") != "done":
                    continue
                if any(d in invalid for d in row.get("depends_on", [])):
                    row.update(status="pending", agent_id=None, claimed_at=None,
                               completed_at=None, error=None)
                    invalid.add(tid)
                    self.changed_tasks.append(tid)
                    growing = True
        done_ids = {t for t, r in tasks.items() if r.get("status") == "done"}
        for tid in invalid:
            row = tasks[tid]
            if not all(d in done_ids for d in row.get("depends_on", [])):
                row["status"] = "blocked"

    def _drop_tasks_not_in_plan(self, plan: MechanicPlan, repo_root: Path | None) -> None:
        """Forget tasks an earlier plan had and this one does not.

        Left on the board they are claimed, fail with "not present in plan"
        and fail the stage; left on disk their C++ is still swept into the
        build by the generated-sources glob. A file is only deleted when no
        current task lists it or is named by it -- shared headers are
        sometimes recorded by a sibling task."""
        current = {it.code_task.id for it in plan.items if it.code_task is not None}
        tasks: dict[str, Any] = self._data["tasks"]
        stale = [tid for tid in tasks if tid not in current]
        if not stale:
            return
        owned: set[str] = set()
        for tid in current:
            owned.update((tasks.get(tid) or {}).get("files_written") or [])
        for tid in stale:
            row = tasks.pop(tid)
            self.dropped_tasks.append(tid)
            if repo_root is None:
                continue
            for rel in row.get("files_written") or []:
                name = Path(rel).name
                if rel in owned or any(
                    name.startswith(f"{c}_") or name.startswith(f"{c}.") for c in current
                ):
                    continue
                path = Path(rel) if Path(rel).is_absolute() else Path(repo_root) / rel
                if path.is_file() and unlink_with_retry(path):
                    self.deleted_files.append(rel)
        self._data["task_order"] = [t for t in self._data["task_order"] if t in current]
        for row in tasks.values():
            row["depends_on"] = [d for d in row.get("depends_on", []) if d in current]

    # ------------------------------------------------------------------
    # Persistence
    # ------------------------------------------------------------------
    def _save_locked(self) -> None:
        """Persist the board to disk atomically. Caller MUST hold
        ``self._lock``.

        Never raises for a file another process is holding: the board in
        memory is what the workers act on, and the file is only the ledger a
        later Resume reads. A failed save marks the board dirty and the next
        save writes everything."""
        try:
            atomic_write_text(self.path, json.dumps(self._data, indent=2, sort_keys=True))
        except OSError as exc:
            self._dirty = True
            self.last_save_error = f"{type(exc).__name__}: {exc}"
            return
        self._dirty = False

    def flush(self) -> bool:
        """Write the board if a save was missed. Returns True when the file
        on disk matches memory."""
        with self._lock:
            if self._dirty:
                self._save_locked()
            return not self._dirty

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

    def invalidate_on_socket_change(
        self,
        sockets_by_asset: dict[str, list[str]],
        repo_root: Path,
    ) -> list[str]:
        """Reset ``done`` tasks whose C++ depended on a socket map that moved.

        The board is what makes a resume cheap, and it is also what makes a
        FIX invisible: with all sixteen tasks ``done``, re-running
        code_generation claims nothing and the stage no-ops, so socket-blind
        C++ survives forever. Fingerprinting the contract into the board is
        what turns "fix asset_contract, re-run codegen" into the thing it
        looks like.

        Only tasks that actually read sockets are reset -- by their declared
        ``required_sockets`` or by their generated source mentioning the
        socket API. A task that never touched a socket cannot have been
        written differently because of one.

        Returns the reset task ids, in ``task_order``.
        """
        fingerprint = socket_fingerprint(sockets_by_asset)
        with self._lock:
            previous = self._data.get("socket_fingerprint")
            self._data["socket_fingerprint"] = fingerprint
            if previous == fingerprint:
                return []

            # ``previous is None`` means the board predates this check, so
            # there is no evidence those tasks ever saw this contract -- and
            # in the case that motivated this, they had seen an EMPTY one.
            tasks = self._data["tasks"]
            order = list(self._data.get("task_order", []))

            invalid = {
                tid
                for tid in order
                if (tasks.get(tid) or {}).get("status") == "done"
                and _row_reads_sockets(tasks[tid], repo_root)
            }
            return self._reset_with_dependents_locked(invalid)

    def invalidate_on_api_contract_change(
        self, version: int, repo_root: Path, is_stale: Any
    ) -> list[tuple[str, list[str]]]:
        """Reset ``done`` tasks whose C++ breaks a newer engine contract.

        ``is_stale(files)`` maps {file name: contents} -> reasons (empty when
        the code still satisfies the contract); see ``codegen_contracts``.
        Checked only when ``version`` differs from what the board last saw,
        so a resume with nothing new costs one dict lookup. A task whose
        files are missing is left to the normal resume path.

        Returns ``(task_id, reasons)`` for every reset task, dependents
        included (with an empty reason list).
        """
        with self._lock:
            previous = self._data.get("api_contract_version")
            self._data["api_contract_version"] = int(version)
            if previous == int(version):
                return []
            tasks = self._data["tasks"]
            order = list(self._data.get("task_order", []))
            reasons: dict[str, list[str]] = {}
            for tid in order:
                row = tasks.get(tid) or {}
                if row.get("status") != "done":
                    continue
                files: dict[str, str] = {}
                for rel in row.get("files_written") or []:
                    path = Path(rel)
                    if not path.is_absolute():
                        path = repo_root / path
                    try:
                        files[path.name] = path.read_text(
                            encoding="utf-8", errors="ignore")
                    except OSError:
                        continue
                found = list(is_stale(files)) if files else []
                if found:
                    reasons[tid] = found
            if not reasons:
                self._save_locked()
                return []
            reset = self._reset_with_dependents_locked(set(reasons))
            return [(tid, reasons.get(tid, [])) for tid in reset]

    def _reset_with_dependents_locked(self, invalid: set[str]) -> list[str]:
        """Reset ``invalid`` and every done task depending on one of them
        back to pending/blocked. Caller holds the lock."""
        tasks = self._data["tasks"]
        order = list(self._data.get("task_order", []))
        # Transitive closure over ``depends_on``: a task written against an
        # export that is about to change has to be rewritten too.
        growing = True
        while growing:
            growing = False
            for tid in order:
                row = tasks.get(tid)
                if row is None or tid in invalid:
                    continue
                if row.get("status") != "done":
                    continue
                if any(d in invalid for d in row.get("depends_on", [])):
                    invalid.add(tid)
                    growing = True

        for tid in invalid:
            tasks[tid].update(
                status="pending",
                agent_id=None,
                claimed_at=None,
                completed_at=None,
                error=None,
            )
        # Anything now waiting on a dep that is no longer done goes back to
        # blocked, so the DAG order is preserved on the re-run.
        done_ids = {
            tid for tid, row in tasks.items() if row.get("status") == "done"
        }
        for tid in invalid:
            row = tasks[tid]
            if not all(d in done_ids for d in row.get("depends_on", [])):
                row["status"] = "blocked"

        self._save_locked()
        return [tid for tid in order if tid in invalid]

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
