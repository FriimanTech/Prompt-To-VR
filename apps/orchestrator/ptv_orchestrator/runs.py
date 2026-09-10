"""In-flight pipeline runs: one handle per project, cancellable.

Two problems this solves.

**There was no way to stop a run.** Stages go into FastAPI
``BackgroundTasks``; once a stage was in a Tripo poll or a twenty-minute
cmake build the only way out was killing the server. A ``RunHandle`` carries
a cancel flag that the long-running loops check, plus the child processes
they spawned so those can be killed too (cmake spawns msbuild, which spawns
compilers -- terminating just the parent leaves the tree running and the
build directory locked).

**A killed server left the project stuck.** ``ProjectState.status`` is on
disk; the old per-project ``threading.Lock`` was not. So Ctrl+C during a
stage left ``status: "running"`` on disk with nothing running, and the UI --
which disables every button while ``status === "running"`` -- refused to let
the user do anything ever again. :func:`reconcile_interrupted_runs` runs at
startup and stamps those projects ``aborted``: a freshly started process
cannot have a run in flight, so the reconciliation is always safe.

This module also subsumes the old lock. "One run per project at a time" is
the same guarantee, just with a handle attached.
"""

from __future__ import annotations

import subprocess
import sys
import threading
from typing import Callable, Iterator, Optional

from ptv_schemas import ProjectStatus, StageRecord

from .storage import ProjectStore

ABORT_MESSAGE = "aborted by user"
RESTART_MESSAGE = "interrupted by an orchestrator restart"


class RunCancelled(RuntimeError):
    """Raised inside a stage when the user aborted the run.

    Stages let it propagate; the orchestrator turns it into an ``aborted``
    stage record rather than a failure, because the user asking a run to
    stop is not the pipeline breaking.
    """


class RunHandle:
    """One in-flight run: a cancel flag plus the processes it spawned."""

    def __init__(self, project_id: str) -> None:
        self.project_id = project_id
        self._cancel = threading.Event()
        self._procs: list[subprocess.Popen] = []
        self._lock = threading.Lock()

    # -- cancellation ---------------------------------------------------
    @property
    def cancelled(self) -> bool:
        return self._cancel.is_set()

    def raise_if_cancelled(self) -> None:
        if self._cancel.is_set():
            raise RunCancelled(ABORT_MESSAGE)

    def cancel(self) -> None:
        """Ask the run to stop and kill anything it has running."""
        self._cancel.set()
        self.kill_processes()

    # -- child processes ------------------------------------------------
    def register_process(self, proc: subprocess.Popen) -> None:
        """Track a child so an abort can kill it.

        If the run was already cancelled the child is killed immediately:
        without that, a process spawned in the window between the abort and
        the next cancellation check would outlive the run.
        """
        with self._lock:
            self._procs.append(proc)
        if self._cancel.is_set():
            _kill_tree(proc)

    def forget_process(self, proc: subprocess.Popen) -> None:
        with self._lock:
            if proc in self._procs:
                self._procs.remove(proc)

    def kill_processes(self) -> int:
        with self._lock:
            procs = list(self._procs)
            self._procs.clear()
        killed = 0
        for proc in procs:
            if _kill_tree(proc):
                killed += 1
        return killed


def _kill_tree(proc: subprocess.Popen) -> bool:
    """Kill a child and its descendants. Returns whether anything died.

    On Windows ``Popen.terminate()`` only signals the process itself, so
    killing cmake would leave msbuild and the compilers running and holding
    the build directory. ``taskkill /T`` takes the whole tree.
    """
    if proc.poll() is not None:
        return False
    try:
        if sys.platform == "win32":
            subprocess.run(
                ["taskkill", "/F", "/T", "/PID", str(proc.pid)],
                capture_output=True,
                check=False,
            )
        else:
            proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
        return True
    except Exception:
        # A process that vanished on its own is not a failure to kill.
        return False


class RunRegistry:
    """Which project is running, and how to stop it."""

    def __init__(self) -> None:
        self._handles: dict[str, RunHandle] = {}
        self._guard = threading.Lock()

    def start(self, project_id: str) -> Optional[RunHandle]:
        """Claim the project. ``None`` means a run is already in flight."""
        with self._guard:
            if project_id in self._handles:
                return None
            handle = RunHandle(project_id)
            self._handles[project_id] = handle
            return handle

    def finish(self, project_id: str) -> None:
        with self._guard:
            self._handles.pop(project_id, None)

    def get(self, project_id: str) -> Optional[RunHandle]:
        with self._guard:
            return self._handles.get(project_id)

    def abort(self, project_id: str) -> bool:
        """Cancel the run and kill its children. ``False`` if none exists."""
        handle = self.get(project_id)
        if handle is None:
            return False
        handle.cancel()
        return True

    def active_project_ids(self) -> list[str]:
        with self._guard:
            return sorted(self._handles)


_registry = RunRegistry()


def get_registry() -> RunRegistry:
    return _registry


# ---------------------------------------------------------------------------
# Startup reconciliation
# ---------------------------------------------------------------------------


def _dangling_stage_records(state) -> Iterator[StageRecord]:
    """Stage records left at ``running`` with no terminal record after them."""
    terminal = {
        ProjectStatus.succeeded,
        ProjectStatus.failed,
        ProjectStatus.aborted,
    }
    latest: dict[str, StageRecord] = {}
    for record in state.stages:
        latest[record.stage.value] = record
    for record in latest.values():
        if record.status not in terminal:
            yield record


def reconcile_interrupted_runs(
    store: ProjectStore, *, emit: Optional[Callable[[str, str], None]] = None
) -> list[str]:
    """Stamp every project the last process left mid-run as ``aborted``.

    Called from the app's lifespan hook. A process that has just started
    owns no runs, so anything on disk claiming to be ``running`` is a
    leftover from a Ctrl+C or a crash. Returns the project ids repaired.
    """
    repaired: list[str] = []
    for state in store.list_projects():
        project_id = state.project_id
        dangling = list(_dangling_stage_records(state))
        if state.status != ProjectStatus.running and not dangling:
            continue

        stage = state.current_stage
        store.record_stage(
            state,
            stage,
            ProjectStatus.aborted,
            message=RESTART_MESSAGE,
        )
        repaired.append(project_id)
        if emit is not None:
            emit(project_id, stage.value)
    return repaired
