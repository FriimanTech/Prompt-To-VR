"""Reruns start clean and file locks do not crash a stage.

Covers the Windows lock-safe file helpers, a codegen board that survives a
save failure and forgets tasks the plan dropped, the full-run archive, the
artifact listing's skip rules and stale scene-file cleanup.
"""

from __future__ import annotations

import json
import os
import threading
from pathlib import Path
from types import SimpleNamespace

import pytest

from ptv_orchestrator import fsutil
from ptv_orchestrator.pipeline import codegen_board as board_mod
from ptv_orchestrator.pipeline.codegen_board import CodegenBoard
from ptv_orchestrator.pipeline.run_reset import (
    archive_previous_run,
    remove_stale_scene_files,
)
from ptv_orchestrator.storage import ProjectStore
from ptv_schemas import (
    CodeTask,
    CodeTaskKind,
    MechanicClassification,
    MechanicPlan,
    MechanicPlanItem,
    ProjectStage,
    ProjectStatus,
)


def _plan(*specs: tuple[str, list[str]], description: str = "x" * 32) -> MechanicPlan:
    return MechanicPlan(prompt_id="proj", items=[
        MechanicPlanItem(
            id=tid,
            classification=MechanicClassification.generate,
            code_task=CodeTask(id=tid, kind=CodeTaskKind.mechanic, title=f"Task {tid}",
                               description=description, interface="IMechanic",
                               depends_on=list(deps)),
        )
        for tid, deps in specs
    ])


# ---------------------------------------------------------------------------
# fsutil
# ---------------------------------------------------------------------------


def _locked(times: int, real):
    calls = {"n": 0}

    def fake(src, dst):
        calls["n"] += 1
        if calls["n"] <= times:
            raise PermissionError(5, "Access is denied")
        return real(src, dst)

    return fake, calls


def test_replace_retries_through_a_brief_lock(tmp_path, monkeypatch):
    src, dst = tmp_path / "a.tmp", tmp_path / "a.json"
    src.write_text("new")
    dst.write_text("old")
    fake, calls = _locked(2, os.replace)
    monkeypatch.setattr(fsutil.os, "replace", fake)
    fsutil.replace_with_retry(src, dst)
    assert dst.read_text() == "new" and calls["n"] == 3


def test_replace_gives_up_after_budget(tmp_path, monkeypatch):
    src, dst = tmp_path / "a.tmp", tmp_path / "a.json"
    src.write_text("new")
    fake, _ = _locked(10_000, os.replace)
    monkeypatch.setattr(fsutil.os, "replace", fake)
    with pytest.raises(PermissionError):
        fsutil.replace_with_retry(src, dst, budget_s=0.1)


def test_atomic_write_uses_unique_temp_names(tmp_path):
    target = tmp_path / "board.json"
    errors: list[Exception] = []

    def writer(k: int) -> None:
        try:
            for i in range(20):
                fsutil.atomic_write_text(target, json.dumps({"k": k, "i": i}))
        except Exception as exc:  # noqa: BLE001
            errors.append(exc)

    threads = [threading.Thread(target=writer, args=(k,)) for k in range(4)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    assert not errors
    json.loads(target.read_text())
    assert not list(tmp_path.glob("*.tmp"))


def test_move_keeps_the_result_when_the_source_stays_locked(tmp_path, monkeypatch):
    """The asset_rigging failure: the copy landed but deleting the staged
    download raised, and the paid rig was recorded as failed."""
    src = tmp_path / "staging" / "model.glb"
    src.parent.mkdir()
    src.write_bytes(b"glb")
    dst = tmp_path / "model_rigged.glb"

    real_replace = os.replace

    def replace_fails_for_src(a, b):
        if Path(a) == src:
            raise PermissionError(32, "in use")
        return real_replace(a, b)

    monkeypatch.setattr(fsutil.os, "replace", replace_fails_for_src)
    monkeypatch.setattr(Path, "unlink", lambda self, missing_ok=False: (_ for _ in ()).throw(
        PermissionError(32, "in use")))
    fsutil.move_with_retry(src, dst, budget_s=0.05)
    assert dst.read_bytes() == b"glb"


# ---------------------------------------------------------------------------
# Codegen board
# ---------------------------------------------------------------------------


def test_board_save_failure_does_not_raise_and_flushes_later(tmp_path, monkeypatch):
    board = CodegenBoard.load_or_init(tmp_path, _plan(("a", [])))
    real = board_mod.atomic_write_text

    def boom(*_a, **_k):
        raise PermissionError(5, "Access is denied")

    monkeypatch.setattr(board_mod, "atomic_write_text", boom)
    claimed = board.atomic_claim_next_pending("w0")  # used to raise
    assert claimed is not None and claimed["id"] == "a"
    board.mark_done("a", files_written=[])
    assert board.flush() is False and "Access is denied" in (board.last_save_error or "")

    monkeypatch.setattr(board_mod, "atomic_write_text", real)
    assert board.flush() is True
    on_disk = json.loads((tmp_path / "codegen_board.json").read_text())
    assert on_disk["tasks"]["a"]["status"] == "done"


def test_board_drops_tasks_the_plan_no_longer_has(tmp_path):
    repo = tmp_path / "repo"
    gen = repo / "engine" / "generated" / "mechanics" / "p"
    gen.mkdir(parents=True)
    proj = tmp_path / "proj"
    proj.mkdir()

    old = CodegenBoard.load_or_init(proj, _plan(("wave_flow", []), ("hud", [])))
    for name in ("wave_flow_pool.cpp", "hud_shared.h", "hud_view.cpp"):
        (gen / name).write_text("//")
    old.mark_done("wave_flow", files_written=[
        "engine/generated/mechanics/p/wave_flow_pool.cpp",
        # a sibling's shared header recorded on this row too
        "engine/generated/mechanics/p/hud_shared.h",
    ])
    old.mark_done("hud", files_written=["engine/generated/mechanics/p/hud_view.cpp"])

    new = CodegenBoard.load_or_init(proj, _plan(("hud", [])), repo_root=repo)
    assert new.dropped_tasks == ["wave_flow"]
    assert new.task_order() == ["hud"]
    assert new.task("wave_flow") is None
    assert not (gen / "wave_flow_pool.cpp").exists()
    assert (gen / "hud_shared.h").exists(), "a current task's file must survive"
    assert new.atomic_claim_next_pending("w0") is None  # hud is done, nothing stale


def test_board_regenerates_a_task_whose_codetask_changed(tmp_path):
    first = CodegenBoard.load_or_init(tmp_path, _plan(("hub", []), ("leaf", ["hub"])))
    first.mark_done("hub", files_written=[])
    first.mark_done("leaf", files_written=[])

    same = CodegenBoard.load_or_init(tmp_path, _plan(("hub", []), ("leaf", ["hub"])))
    assert same.changed_tasks == []

    changed = CodegenBoard.load_or_init(
        tmp_path, _plan(("hub", []), ("leaf", ["hub"]), description="y" * 40))
    assert set(changed.changed_tasks) == {"hub", "leaf"}
    assert changed.task("hub")["status"] == "pending"
    assert changed.task("leaf")["status"] == "blocked"


# ---------------------------------------------------------------------------
# Full-run archive
# ---------------------------------------------------------------------------


def _ctx(tmp_path: Path, keep: int = 1):
    projects = tmp_path / "projects"
    store = ProjectStore(projects)
    state = store.create("Goblins", "a goblin game")
    pid = state.project_id
    events: list[tuple] = []
    ctx = SimpleNamespace(
        project_id=pid,
        project_dir=projects / pid,
        settings=SimpleNamespace(repo_root=tmp_path, full_run_archive_keep=keep),
        store=store,
        emit=lambda *a: events.append(a),
    )
    return ctx, events


def test_archive_moves_the_previous_run_aside(tmp_path):
    ctx, events = _ctx(tmp_path)
    proj = ctx.project_dir
    (proj / "codegen_board.json").write_text("{}")
    (proj / "scene_spec.old_arena.json").write_text("{}")
    (proj / "assets" / "goblin").mkdir(parents=True)
    (proj / "assets" / "goblin" / "model.glb").write_bytes(b"g")
    (proj / "builds" / "quest").mkdir(parents=True, exist_ok=True)
    (proj / "builds" / "quest" / "cache.bin").write_bytes(b"c")
    gen = tmp_path / "engine" / "generated" / "mechanics" / ctx.project_id
    gen.mkdir(parents=True)
    (gen / "wave_flow.cpp").write_text("//")
    state = ctx.store.load_state(ctx.project_id)
    ctx.store.record_stage(state, ProjectStage.build_spec, ProjectStatus.succeeded)
    state = ctx.store.load_state(ctx.project_id)
    state.latest_build_apk = "old.apk"
    ctx.store.save_state(state)

    archive = archive_previous_run(ctx)

    assert archive is not None and archive.parent.name == "previous_runs"
    assert (archive / "codegen_board.json").exists()
    assert (archive / "scene_spec.old_arena.json").exists()
    assert (archive / "assets" / "goblin" / "model.glb").exists()
    assert (archive / "engine_generated" / "mechanics" / "wave_flow.cpp").exists()
    assert not gen.exists()
    assert not (proj / "codegen_board.json").exists()
    # Kept: the prompt, the state (with history archived), build caches.
    assert (proj / "prompt.txt").exists()
    assert (proj / "builds" / "quest" / "cache.bin").exists()
    assert (proj / "assets").is_dir() and not any((proj / "assets").iterdir())
    fresh = ctx.store.load_state(ctx.project_id)
    assert fresh.stages == [] and fresh.latest_build_apk is None
    old_state = json.loads((archive / "project_state.json").read_text())
    assert old_state["stages"]
    assert any("archived the previous run" in str(e) for e in events)


def test_archive_keeps_only_the_newest(tmp_path):
    ctx, _ = _ctx(tmp_path, keep=1)
    root = ctx.project_dir / "previous_runs"
    (root / "20200101-000000").mkdir(parents=True)
    (ctx.project_dir / "build_spec.json").write_text("{}")
    archive = archive_previous_run(ctx)
    assert [p.name for p in root.iterdir()] == [archive.name]


def test_archive_of_a_new_project_is_a_no_op(tmp_path):
    ctx, _ = _ctx(tmp_path)
    assert archive_previous_run(ctx) is None
    assert not (ctx.project_dir / "previous_runs").exists()


def test_resume_paths_do_not_archive():
    from ptv_orchestrator.pipeline import orchestrator

    src = Path(orchestrator.__file__).read_text(encoding="utf-8")
    body = src.split("def run_pipeline", 1)[1].split("\ndef ", 1)[0]
    assert "archive_previous_run(ctx)" in body
    for fn in ("def resume_pipeline", "def resume_from_stage", "def run_stage",
               "def _run_stages_from"):
        part = src.split(fn, 1)[1].split("\ndef ", 1)[0]
        assert "archive_previous_run" not in part, fn


# ---------------------------------------------------------------------------
# Artifact listing + stale scene files
# ---------------------------------------------------------------------------


def test_artifact_listing_skips_staging_and_temp_files(tmp_path):
    store = ProjectStore(tmp_path / "projects")
    state = store.create("Goblins", "a goblin game")
    proj = store.dir_for(state.project_id)
    (proj / "build_spec.json").write_text("{}")
    (proj / "codegen_board.json.123.456.tmp").write_text("{")
    (proj / "assets" / "g" / ".rig_download").mkdir(parents=True)
    (proj / "assets" / "g" / ".rig_download" / "model.glb").write_bytes(b"x")
    (proj / "assets" / "g" / "model_uthana_rigged.download").write_bytes(b"x")
    (proj / "previous_runs" / "old").mkdir(parents=True)
    (proj / "previous_runs" / "old" / "scene.json").write_text("{}")

    names = {a.path.rsplit("/", 1)[-1] for a in store.list_artifacts(state.project_id)}
    assert "build_spec.json" in names
    assert not any(n.endswith((".tmp", ".download")) for n in names)
    assert "model.glb" not in names and "scene.json" not in names


def test_stale_scene_files_are_removed(tmp_path):
    for name in ("scene_spec.json", "scene_spec.scrapyard.json",
                 "scene_spec.ruins_arena.json", "scene.ruins_arena.json",
                 "scene.scrapyard.json", "scene.json"):
        (tmp_path / name).write_text("{}")
    assert remove_stale_scene_files(tmp_path, "scene_spec", ["scrapyard"]) == [
        "scene_spec.ruins_arena.json"]
    assert remove_stale_scene_files(tmp_path, "scene", ["scrapyard"]) == [
        "scene.ruins_arena.json"]
    left = sorted(p.name for p in tmp_path.iterdir())
    assert left == ["scene.json", "scene.scrapyard.json", "scene_spec.json",
                    "scene_spec.scrapyard.json"]
