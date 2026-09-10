"""Aborting a run, and recovering from a server killed mid-run.

Two failure modes are pinned here.

**There was no way to stop a run.** Once a stage was in flight the only
option was killing the server, which caused the second problem.

**A killed server locked the project.** ``ProjectState.status`` is on disk
while the in-flight registry is in memory, so Ctrl+C during a stage left
``status: "running"`` persisted with nothing running. The web panel disables
every button while a project claims to be running, so the project became
permanently untouchable until someone hand-edited the JSON.
"""

from __future__ import annotations

import subprocess
import sys
import threading
from pathlib import Path

import pytest

from ptv_orchestrator.events import EventBus, PipelineEvent
from ptv_orchestrator.pipeline import orchestrator
from ptv_orchestrator.pipeline.context import PipelineContext, StageResult
from ptv_orchestrator.runs import (
    RunCancelled,
    RunRegistry,
    reconcile_interrupted_runs,
)
from ptv_orchestrator.settings import Settings
from ptv_orchestrator.storage import ProjectStore
from ptv_schemas import ProjectStage, ProjectStatus


class _ListBus(EventBus):
    def __init__(self) -> None:
        super().__init__()
        self.published: list[PipelineEvent] = []
        self._pub_lock = threading.Lock()

    def publish(self, event: PipelineEvent) -> None:
        with self._pub_lock:
            self.published.append(event)


@pytest.fixture(autouse=True)
def _clean_registry():
    """Each test starts with no run claimed."""
    registry = orchestrator.get_registry()
    for pid in registry.active_project_ids():
        registry.finish(pid)
    yield
    for pid in registry.active_project_ids():
        registry.finish(pid)


def _settings(tmp_path: Path) -> Settings:
    return Settings(
        projects_dir=tmp_path / "projects",
        engine_dir=tmp_path / "engine",
        samples_dir=tmp_path / "samples",
        repo_root=tmp_path,
    )


def _seed(tmp_path: Path) -> tuple[ProjectStore, str, Settings]:
    settings = _settings(tmp_path)
    store = ProjectStore(settings.projects_dir)
    state = store.create("Demo", "a demo prompt for the tests")
    return store, state.project_id, settings


# ---------------------------------------------------------------------------
# The registry itself
# ---------------------------------------------------------------------------


def test_only_one_run_per_project() -> None:
    """Subsumes the old per-project lock: two cmake builds against the same
    build dir race on the vcpkg lock and one gets killed."""
    registry = RunRegistry()

    first = registry.start("proj")
    assert first is not None
    assert registry.start("proj") is None
    assert registry.start("other") is not None

    registry.finish("proj")
    assert registry.start("proj") is not None


def test_cancelled_handle_raises_at_checkpoints() -> None:
    registry = RunRegistry()
    handle = registry.start("proj")
    assert handle is not None

    handle.raise_if_cancelled()  # no-op while live

    assert registry.abort("proj") is True
    assert handle.cancelled is True
    with pytest.raises(RunCancelled):
        handle.raise_if_cancelled()


def test_aborting_an_unknown_project_is_not_an_error() -> None:
    assert RunRegistry().abort("nobody") is False


def test_abort_kills_a_registered_child_process() -> None:
    """cmake spawns msbuild spawns the compilers; terminating only the
    parent leaves them holding the build directory."""
    registry = RunRegistry()
    handle = registry.start("proj")
    assert handle is not None

    proc = subprocess.Popen(
        [sys.executable, "-c", "import time; time.sleep(120)"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    try:
        handle.register_process(proc)
        registry.abort("proj")
        assert proc.wait(timeout=30) is not None
        assert proc.poll() is not None, "child survived the abort"
    finally:
        if proc.poll() is None:  # pragma: no cover - defensive
            proc.kill()


def test_a_process_started_after_the_abort_is_killed_immediately() -> None:
    """Closes the race between setting the flag and the next spawn."""
    registry = RunRegistry()
    handle = registry.start("proj")
    assert handle is not None
    registry.abort("proj")

    proc = subprocess.Popen(
        [sys.executable, "-c", "import time; time.sleep(120)"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    try:
        handle.register_process(proc)
        assert proc.wait(timeout=30) is not None
    finally:
        if proc.poll() is None:  # pragma: no cover - defensive
            proc.kill()


# ---------------------------------------------------------------------------
# Aborting a live run through the orchestrator
# ---------------------------------------------------------------------------


def test_abort_mid_stage_stamps_aborted_and_frees_the_project(
    tmp_path: Path, monkeypatch
) -> None:
    store, pid, settings = _seed(tmp_path)
    bus = _ListBus()
    monkeypatch.setattr(orchestrator, "get_bus", lambda: bus)

    entered = threading.Event()
    released = threading.Event()

    def slow_stage(ctx: PipelineContext) -> StageResult:
        ctx.store.record_stage(
            ctx.store.load_state(ctx.project_id),
            ProjectStage.build_spec,
            ProjectStatus.running,
        )
        entered.set()
        # Stand in for a provider call: wait, then hit a checkpoint.
        released.wait(timeout=10)
        ctx.raise_if_cancelled()
        raise AssertionError("the stage should have been cancelled")

    monkeypatch.setattr(
        orchestrator,
        "_stages_for",
        lambda _device: [(ProjectStage.build_spec, slow_stage)],
    )

    worker = threading.Thread(
        target=orchestrator.run_pipeline, args=(pid, settings)
    )
    worker.start()
    assert entered.wait(timeout=10), "stage never started"

    assert orchestrator.get_registry().abort(pid) is True
    released.set()
    worker.join(timeout=15)
    assert not worker.is_alive()

    state = store.load_state(pid)
    assert state.status == ProjectStatus.aborted
    assert "aborted" in state.stages[-1].message

    # And the project is immediately re-runnable: the claim is released.
    assert orchestrator.get_registry().get(pid) is None
    assert orchestrator.get_registry().start(pid) is not None


def test_a_stage_that_returns_without_stamping_never_leaves_running(
    tmp_path: Path, monkeypatch
) -> None:
    """The real cause of the stuck project: nothing guaranteed a terminal
    status, so a stage that returned quietly left the UI locked."""
    store, pid, settings = _seed(tmp_path)
    bus = _ListBus()
    monkeypatch.setattr(orchestrator, "get_bus", lambda: bus)

    def silent_stage(ctx: PipelineContext) -> StageResult:
        return StageResult(stage=ProjectStage.build_spec)

    monkeypatch.setattr(
        orchestrator,
        "_stages_for",
        lambda _device: [(ProjectStage.build_spec, silent_stage)],
    )

    orchestrator.run_pipeline(pid, settings)

    assert store.load_state(pid).status != ProjectStatus.running


def test_a_failing_stage_is_stamped_failed_on_every_entry_point(
    tmp_path: Path, monkeypatch
) -> None:
    """``run_stage`` and ``resume_pipeline`` used to have no failure handler
    at all, so a crash there left the project 'running' forever."""
    store, pid, settings = _seed(tmp_path)
    bus = _ListBus()
    monkeypatch.setattr(orchestrator, "get_bus", lambda: bus)

    def boom(ctx: PipelineContext) -> StageResult:
        raise RuntimeError("provider exploded")

    monkeypatch.setattr(
        orchestrator,
        "_stages_for",
        lambda _device: [(ProjectStage.build_spec, boom)],
    )
    monkeypatch.setattr(
        orchestrator,
        "_stages_for_project",
        lambda _ctx: [(ProjectStage.build_spec, boom)],
    )

    with pytest.raises(RuntimeError, match="provider exploded"):
        orchestrator.run_stage(pid, ProjectStage.build_spec, settings)

    state = store.load_state(pid)
    assert state.status == ProjectStatus.failed
    assert "provider exploded" in state.stages[-1].message


def test_a_second_run_is_skipped_not_stacked(tmp_path: Path, monkeypatch) -> None:
    store, pid, settings = _seed(tmp_path)
    bus = _ListBus()
    monkeypatch.setattr(orchestrator, "get_bus", lambda: bus)

    # Claim the project as if a run were already in flight.
    assert orchestrator.get_registry().start(pid) is not None

    orchestrator.run_pipeline(pid, settings)

    assert any(e.status == "skipped" for e in bus.published), bus.published


# ---------------------------------------------------------------------------
# Startup recovery
# ---------------------------------------------------------------------------


def test_startup_clears_a_state_left_running_by_a_killed_server(
    tmp_path: Path,
) -> None:
    store, pid, settings = _seed(tmp_path)

    # Exactly what Ctrl+C during scene_spec leaves behind.
    state = store.load_state(pid)
    store.record_stage(state, ProjectStage.scene_spec, ProjectStatus.running)
    assert store.load_state(pid).status == ProjectStatus.running

    repaired = reconcile_interrupted_runs(store)

    assert repaired == [pid]
    healed = store.load_state(pid)
    assert healed.status == ProjectStatus.aborted
    assert "restart" in healed.stages[-1].message
    # The stage that was interrupted is the one blamed, so Resume picks it up.
    assert healed.current_stage == ProjectStage.scene_spec


def test_startup_leaves_finished_projects_alone(tmp_path: Path) -> None:
    store, pid, settings = _seed(tmp_path)
    state = store.load_state(pid)
    store.record_stage(state, ProjectStage.build_spec, ProjectStatus.running)
    store.record_stage(
        store.load_state(pid), ProjectStage.build_spec, ProjectStatus.succeeded
    )
    before = store.load_state(pid)

    assert reconcile_interrupted_runs(store) == []

    after = store.load_state(pid)
    assert after.status == ProjectStatus.succeeded
    assert len(after.stages) == len(before.stages)


def test_startup_hook_runs_the_reconcile(tmp_path: Path, monkeypatch) -> None:
    """Wiring check: the lifespan hook is what makes recovery automatic."""
    from fastapi.testclient import TestClient

    import ptv_orchestrator.main as main_mod

    store, pid, settings = _seed(tmp_path)
    state = store.load_state(pid)
    store.record_stage(state, ProjectStage.scene_spec, ProjectStatus.running)

    monkeypatch.setattr(main_mod, "get_settings", lambda: settings)

    with TestClient(main_mod.app):
        pass  # entering the context runs lifespan startup

    assert store.load_state(pid).status == ProjectStatus.aborted


# ---------------------------------------------------------------------------
# The Abort endpoint
# ---------------------------------------------------------------------------


def test_abort_endpoint_clears_a_stale_running_state(
    tmp_path: Path, monkeypatch
) -> None:
    """The button doubles as a manual unstick for the Ctrl+C case."""
    from fastapi.testclient import TestClient

    import ptv_orchestrator.api.routes as routes_mod
    import ptv_orchestrator.main as main_mod

    store, pid, settings = _seed(tmp_path)
    monkeypatch.setattr(main_mod, "get_settings", lambda: settings)
    monkeypatch.setattr(routes_mod, "_store", lambda: store)

    with TestClient(main_mod.app) as client:
        # Re-dirty the state AFTER startup reconciliation has run.
        store.record_stage(
            store.load_state(pid), ProjectStage.scene_spec, ProjectStatus.running
        )

        body = client.post(f"/api/projects/{pid}/abort").json()

        assert body == {"ok": True, "aborted": False, "reconciled": True}
        assert store.load_state(pid).status == ProjectStatus.aborted

        # Idempotent: a second press reports there was nothing to do.
        again = client.post(f"/api/projects/{pid}/abort").json()
        assert again == {"ok": True, "aborted": False, "reconciled": False}


def test_abort_endpoint_cancels_a_live_run(tmp_path: Path, monkeypatch) -> None:
    from fastapi.testclient import TestClient

    import ptv_orchestrator.api.routes as routes_mod
    import ptv_orchestrator.main as main_mod

    store, pid, settings = _seed(tmp_path)
    monkeypatch.setattr(main_mod, "get_settings", lambda: settings)
    monkeypatch.setattr(routes_mod, "_store", lambda: store)

    handle = orchestrator.get_registry().start(pid)
    assert handle is not None

    with TestClient(main_mod.app) as client:
        body = client.post(f"/api/projects/{pid}/abort").json()

    assert body == {"ok": True, "aborted": True, "reconciled": False}
    assert handle.cancelled is True


def test_abort_endpoint_404s_for_an_unknown_project(
    tmp_path: Path, monkeypatch
) -> None:
    from fastapi.testclient import TestClient

    import ptv_orchestrator.api.routes as routes_mod
    import ptv_orchestrator.main as main_mod

    store, _pid, settings = _seed(tmp_path)
    monkeypatch.setattr(main_mod, "get_settings", lambda: settings)
    monkeypatch.setattr(routes_mod, "_store", lambda: store)

    with TestClient(main_mod.app) as client:
        assert client.post("/api/projects/nope/abort").status_code == 404


def test_startup_clears_a_dangling_stage_record(tmp_path: Path) -> None:
    """A stage left ``running`` with no terminal record after it -- the
    shape that made Resume sit and do nothing."""
    store, pid, settings = _seed(tmp_path)
    state = store.load_state(pid)
    store.record_stage(state, ProjectStage.build_spec, ProjectStatus.running)
    store.record_stage(
        store.load_state(pid), ProjectStage.build_spec, ProjectStatus.succeeded
    )
    store.record_stage(
        store.load_state(pid), ProjectStage.scene_spec, ProjectStatus.running
    )
    # Force the top-level status to look settled while a stage is dangling.
    settled = store.load_state(pid)
    settled.status = ProjectStatus.failed
    store.save_state(settled)

    assert reconcile_interrupted_runs(store) == [pid]
    assert store.load_state(pid).status == ProjectStatus.aborted
