"""Unit tests for ``ptv_orchestrator.pipeline.codegen_board``.

These tests exercise the ledger's status machine, FCFS claim order,
dependency DAG promotion, atomic-claim safety under concurrent threads,
and resume behaviour. All tests are network-free and run on a
``tmp_path`` so they leave nothing behind.
"""

from __future__ import annotations

import threading
from pathlib import Path

import pytest

from ptv_orchestrator.pipeline.codegen_board import CodegenBoard
from ptv_schemas import (
    CodeTask,
    CodeTaskKind,
    MechanicClassification,
    MechanicPlan,
    MechanicPlanItem,
)


def _task(task_id: str, depends_on: list[str] | None = None) -> CodeTask:
    return CodeTask(
        id=task_id,
        kind=CodeTaskKind.mechanic,
        title=f"Task {task_id}",
        description="x" * 32,
        interface="IMechanic",
        depends_on=list(depends_on or []),
    )


def _plan(*specs: tuple[str, list[str]]) -> MechanicPlan:
    """``specs`` is a sequence of ``(task_id, depends_on)`` tuples in
    MechanicPlan order."""
    items: list[MechanicPlanItem] = []
    for task_id, depends in specs:
        items.append(
            MechanicPlanItem(
                id=task_id,
                classification=MechanicClassification.generate,
                code_task=_task(task_id, depends),
            )
        )
    return MechanicPlan(prompt_id="proj", items=items)


# ---------------------------------------------------------------------------
# Initial states
# ---------------------------------------------------------------------------


def test_init_sets_pending_when_no_deps(tmp_path: Path) -> None:
    board = CodegenBoard.load_or_init(tmp_path, _plan(("a", []), ("b", [])))
    counts = board.status_counts()
    assert counts["pending"] == 2
    assert counts["blocked"] == 0
    # task_order matches plan order.
    assert board.task_order() == ["a", "b"]


def test_init_sets_blocked_when_deps_present(tmp_path: Path) -> None:
    board = CodegenBoard.load_or_init(tmp_path, _plan(("a", []), ("b", ["a"])))
    counts = board.status_counts()
    assert counts["pending"] == 1
    assert counts["blocked"] == 1
    assert board.task("a")["status"] == "pending"  # type: ignore[index]
    assert board.task("b")["status"] == "blocked"  # type: ignore[index]


def test_init_drops_unknown_deps(tmp_path: Path) -> None:
    # Planner accidentally targeted a builtin id (not a code_task).
    board = CodegenBoard.load_or_init(
        tmp_path, _plan(("a", ["builtin.grab"]))
    )
    row = board.task("a")
    assert row is not None
    assert row["depends_on"] == []
    assert row["status"] == "pending"


# ---------------------------------------------------------------------------
# Dependency DAG (40.3 mandatory)
# ---------------------------------------------------------------------------


def test_blocked_not_claimable_until_dep_done(tmp_path: Path) -> None:
    board = CodegenBoard.load_or_init(tmp_path, _plan(("a", []), ("b", ["a"])))
    # First claim must be 'a'; 'b' is still blocked.
    first = board.atomic_claim_next_pending("worker-0")
    assert first is not None
    assert first["id"] == "a"
    # b is not yet pending.
    second = board.atomic_claim_next_pending("worker-1")
    assert second is None


def test_refresh_blocked_promotes_after_done(tmp_path: Path) -> None:
    board = CodegenBoard.load_or_init(tmp_path, _plan(("a", []), ("b", ["a"])))
    first = board.atomic_claim_next_pending("worker-0")
    assert first is not None and first["id"] == "a"

    promoted = board.mark_done("a", files_written=["engine/generated/mechanics/proj/a.cpp"])
    assert promoted == ["b"]
    # Now b is claimable.
    nxt = board.atomic_claim_next_pending("worker-1")
    assert nxt is not None and nxt["id"] == "b"


def test_mark_failed_does_not_promote_dependents(tmp_path: Path) -> None:
    board = CodegenBoard.load_or_init(tmp_path, _plan(("a", []), ("b", ["a"])))
    first = board.atomic_claim_next_pending("worker-0")
    assert first is not None and first["id"] == "a"
    board.mark_failed("a", error="boom")
    # b must remain blocked.
    assert board.task("b")["status"] == "blocked"  # type: ignore[index]
    nxt = board.atomic_claim_next_pending("worker-1")
    assert nxt is None


# ---------------------------------------------------------------------------
# FCFS order = MechanicPlan order
# ---------------------------------------------------------------------------


def test_fcfs_follows_plan_order(tmp_path: Path) -> None:
    plan = _plan(("alpha", []), ("beta", []), ("gamma", []))
    board = CodegenBoard.load_or_init(tmp_path, plan)
    order = []
    while True:
        claimed = board.atomic_claim_next_pending(f"worker-{len(order)}")
        if claimed is None:
            break
        order.append(claimed["id"])
        board.mark_done(claimed["id"], files_written=[])
    assert order == ["alpha", "beta", "gamma"]


# ---------------------------------------------------------------------------
# Atomic claim under concurrency
# ---------------------------------------------------------------------------


def test_atomic_claim_under_concurrent_threads(tmp_path: Path) -> None:
    """20 threads competing for 10 pending tasks: every task is claimed
    exactly once and the union of claims equals the plan."""

    n_tasks = 10
    plan = _plan(*((f"t{i:02d}", []) for i in range(n_tasks)))
    board = CodegenBoard.load_or_init(tmp_path, plan)

    claims: list[str] = []
    claims_lock = threading.Lock()
    start_barrier = threading.Barrier(20)

    def worker(name: str) -> None:
        start_barrier.wait()
        while True:
            row = board.atomic_claim_next_pending(name)
            if row is None:
                return
            with claims_lock:
                claims.append(row["id"])
            # Hand the task back as 'done' so the loop drains.
            board.mark_done(row["id"], files_written=[])

    threads = [
        threading.Thread(target=worker, args=(f"worker-{i}",))
        for i in range(20)
    ]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    # Every task claimed exactly once.
    assert sorted(claims) == sorted([f"t{i:02d}" for i in range(n_tasks)])
    # Board is fully drained.
    assert board.has_remaining_work() is False
    assert board.status_counts()["done"] == n_tasks


def test_concurrent_claim_never_returns_same_task(tmp_path: Path) -> None:
    """With a single pending task, only one of two competing threads
    receives a non-None row."""

    plan = _plan(("only", []))
    board = CodegenBoard.load_or_init(tmp_path, plan)

    results: list[dict | None] = []
    results_lock = threading.Lock()
    barrier = threading.Barrier(2)

    def worker(name: str) -> None:
        barrier.wait()
        row = board.atomic_claim_next_pending(name)
        with results_lock:
            results.append(row)

    t1 = threading.Thread(target=worker, args=("a",))
    t2 = threading.Thread(target=worker, args=("b",))
    t1.start()
    t2.start()
    t1.join()
    t2.join()

    non_none = [r for r in results if r is not None]
    assert len(non_none) == 1
    assert non_none[0]["id"] == "only"


# ---------------------------------------------------------------------------
# Resume
# ---------------------------------------------------------------------------


def test_resume_skips_done_tasks(tmp_path: Path) -> None:
    plan = _plan(("a", []), ("b", ["a"]))
    board1 = CodegenBoard.load_or_init(tmp_path, plan)
    claimed = board1.atomic_claim_next_pending("w0")
    assert claimed is not None and claimed["id"] == "a"
    board1.mark_done("a", files_written=["engine/generated/mechanics/proj/a.cpp"])

    # New board over the same dir simulates a resumed pipeline.
    board2 = CodegenBoard.load_or_init(tmp_path, plan)
    counts = board2.status_counts()
    assert counts["done"] == 1
    # b is pending now that 'a' is done.
    assert board2.task("b")["status"] == "pending"  # type: ignore[index]


def test_resume_preserves_failed(tmp_path: Path) -> None:
    plan = _plan(("a", []))
    b1 = CodegenBoard.load_or_init(tmp_path, plan)
    b1.atomic_claim_next_pending("w")
    b1.mark_failed("a", error="oops")

    b2 = CodegenBoard.load_or_init(tmp_path, plan)
    assert b2.task("a")["status"] == "failed"  # type: ignore[index]
    # Failed tasks remain failed until a human resets them; nothing to
    # claim.
    assert b2.atomic_claim_next_pending("w2") is None


def test_reset_stuck_in_progress_restores_pending(tmp_path: Path) -> None:
    plan = _plan(("a", []), ("b", ["a"]))
    b1 = CodegenBoard.load_or_init(tmp_path, plan)
    claimed = b1.atomic_claim_next_pending("w0")
    assert claimed is not None
    # Simulate crash: drop the board reference and reload without
    # calling mark_done/mark_failed.

    b2 = CodegenBoard.load_or_init(tmp_path, plan)
    assert b2.task("a")["status"] == "in_progress"  # type: ignore[index]
    assert b2.stuck_in_progress_ids() == ["a"]

    reset = b2.reset_stuck_in_progress()
    assert reset == ["a"]
    assert b2.task("a")["status"] == "pending"  # type: ignore[index]
    assert b2.atomic_claim_next_pending("w1")["id"] == "a"  # type: ignore[index]


def test_status_counts_reports_all_states(tmp_path: Path) -> None:
    plan = _plan(("a", []), ("b", ["a"]), ("c", ["a"]))
    board = CodegenBoard.load_or_init(tmp_path, plan)
    counts = board.status_counts()
    assert counts == {
        "pending": 1,
        "blocked": 2,
        "in_progress": 0,
        "done": 0,
        "failed": 0,
    }


def test_idempotent_merge_does_not_duplicate(tmp_path: Path) -> None:
    plan = _plan(("a", []))
    b1 = CodegenBoard.load_or_init(tmp_path, plan)
    assert b1.task_order() == ["a"]
    b2 = CodegenBoard.load_or_init(tmp_path, plan)
    assert b2.task_order() == ["a"]
    assert b2.status_counts()["pending"] == 1


def test_self_reference_is_dropped(tmp_path: Path) -> None:
    plan = _plan(("a", ["a"]))
    board = CodegenBoard.load_or_init(tmp_path, plan)
    row = board.task("a")
    assert row is not None
    assert row["depends_on"] == []
    assert row["status"] == "pending"


def test_mark_done_missing_task_raises(tmp_path: Path) -> None:
    board = CodegenBoard.load_or_init(tmp_path, _plan(("a", [])))
    with pytest.raises(KeyError):
        board.mark_done("does_not_exist", files_written=[])


def test_files_written_and_exports_persisted(tmp_path: Path) -> None:
    plan = _plan(("a", []))
    board = CodegenBoard.load_or_init(tmp_path, plan)
    board.atomic_claim_next_pending("worker-0")
    board.mark_done(
        "a",
        files_written=["engine/generated/mechanics/proj/a.cpp"],
        exports={"registered_mechanic_id": "generated.a"},
    )
    row = board.task("a")
    assert row is not None
    assert row["files_written"] == ["engine/generated/mechanics/proj/a.cpp"]
    assert row["exports"] == {"registered_mechanic_id": "generated.a"}
    # Reload to confirm round-trip persistence.
    b2 = CodegenBoard.load_or_init(tmp_path, plan)
    row2 = b2.task("a")
    assert row2 is not None
    assert row2["files_written"] == ["engine/generated/mechanics/proj/a.cpp"]
    assert row2["exports"] == {"registered_mechanic_id": "generated.a"}

# ---------------------------------------------------------------------------
# Socket-contract invalidation
#
# The board is what makes a resume cheap, and it is also what made a FIX
# invisible: with every task ``done``, re-running code_generation claimed
# nothing and the socket-blind C++ survived.
# ---------------------------------------------------------------------------


def _finish(board: CodegenBoard, task_id: str, repo_root: Path, body: str):
    """Claim + complete ``task_id``, writing ``body`` as its generated C++."""
    board.atomic_claim_next_pending("worker-0")
    rel = f"engine/generated/mechanics/proj/{task_id}.cpp"
    path = repo_root / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(body, encoding="utf-8")
    board.mark_done(task_id, files_written=[rel])


def test_unchanged_socket_contract_resets_nothing(tmp_path: Path) -> None:
    """The order the stage actually uses: fingerprint first, then generate."""
    repo = tmp_path / "repo"
    board = CodegenBoard.load_or_init(tmp_path, _plan(("a", [])))
    sockets = {"pistol": ["grip", "muzzle"]}
    board.invalidate_on_socket_change(sockets, repo)
    _finish(board, "a", repo, "ctx.sockets->socket_world(self, \"muzzle\");")

    # A resume against the same contract must stay cheap.
    assert board.invalidate_on_socket_change(sockets, repo) == []
    assert board.task("a")["status"] == "done"


def test_changed_socket_contract_resets_only_socket_readers(tmp_path: Path) -> None:
    repo = tmp_path / "repo"
    board = CodegenBoard.load_or_init(tmp_path, _plan(("shoot", []), ("hud", [])))
    board.invalidate_on_socket_change({}, repo)
    _finish(board, "shoot", repo, "ctx.sockets->socket_world(weapon, \"muzzle\");")
    _finish(board, "hud", repo, "draw_score_panel();")

    reset = board.invalidate_on_socket_change({"pistol": ["muzzle"]}, repo)

    # The HUD never touched a socket, so it cannot have been written
    # differently because of one -- re-billing it would be waste.
    assert reset == ["shoot"]
    assert board.task("shoot")["status"] == "pending"
    assert board.task("hud")["status"] == "done"


def test_declared_required_sockets_counts_even_without_source(tmp_path: Path) -> None:
    repo = tmp_path / "repo"
    plan = _plan(("recoil", []))
    plan.items[0].code_task.required_sockets = ["grip"]
    board = CodegenBoard.load_or_init(tmp_path, plan)
    board.invalidate_on_socket_change({}, repo)
    _finish(board, "recoil", repo, "// no socket calls in this file")

    assert board.invalidate_on_socket_change({"rifle": ["grip"]}, repo) == [
        "recoil"
    ]


def test_dependents_of_a_reset_task_are_reset_and_reblocked(tmp_path: Path) -> None:
    """A task written against an export that is about to change has to be
    rewritten too, and must wait its turn in the DAG."""
    repo = tmp_path / "repo"
    board = CodegenBoard.load_or_init(
        tmp_path, _plan(("hub", []), ("profile", ["hub"]))
    )
    board.invalidate_on_socket_change({}, repo)
    _finish(board, "hub", repo, "ctx.sockets->has_socket(weapon, \"muzzle\");")
    _finish(board, "profile", repo, "use_hub_api();")

    reset = board.invalidate_on_socket_change({"pistol": ["muzzle"]}, repo)

    assert reset == ["hub", "profile"]
    assert board.task("hub")["status"] == "pending"
    assert board.task("profile")["status"] == "blocked"


def test_legacy_board_without_a_fingerprint_is_invalidated(tmp_path: Path) -> None:
    """The case that motivated all of this: sixteen done tasks generated
    against an EMPTY contract, and no record that they ever saw one."""
    repo = tmp_path / "repo"
    board = CodegenBoard.load_or_init(tmp_path, _plan(("shoot", [])))
    _finish(board, "shoot", repo, "ctx.sockets->socket_world(weapon, \"muzzle\");")
    assert "socket_fingerprint" not in board.snapshot()

    assert board.invalidate_on_socket_change({"pistol": ["muzzle"]}, repo) == [
        "shoot"
    ]
    assert board.has_remaining_work()


def test_deleted_generated_file_is_treated_as_a_socket_reader(tmp_path: Path) -> None:
    repo = tmp_path / "repo"
    board = CodegenBoard.load_or_init(tmp_path, _plan(("a", [])))
    board.invalidate_on_socket_change({}, repo)
    _finish(board, "a", repo, "no sockets here")
    (repo / "engine/generated/mechanics/proj/a.cpp").unlink()

    # Unverifiable, and regenerating is the cheap side of that bet.
    assert board.invalidate_on_socket_change({"pistol": ["muzzle"]}, repo) == [
        "a"
    ]
