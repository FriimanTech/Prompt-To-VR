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
