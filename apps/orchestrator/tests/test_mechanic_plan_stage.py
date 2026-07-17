"""Tests for the agent-based MechanicPlan stage + binding validator.

These exercise ``stage_mechanic_plan.run_with_planner`` with an injected
stub ``MechanicPlanner`` so the OpenAI Agents SDK is never touched, plus
the deterministic ``binding_validator`` in isolation.
"""

from __future__ import annotations

import threading
from pathlib import Path

from ptv_orchestrator.events import EventBus, PipelineEvent
from ptv_orchestrator.pipeline import stage_mechanic_plan
from ptv_orchestrator.pipeline.binding_validator import validate_bindings
from ptv_orchestrator.pipeline.context import PipelineContext
from ptv_orchestrator.pipeline.mechanic_plan_agent import MechanicPlanner
from ptv_orchestrator.settings import Settings
from ptv_orchestrator.storage import ProjectStore
from ptv_schemas import (
    AssetEntry,
    AssetItem,
    AssetManifest,
    BuildSpec,
    ControllerBinding,
    ControllerControl,
    ControllerHand,
    MechanicClassification,
    MechanicEntry,
    MechanicPlan,
    MechanicPlanItem,
    MechanicType,
    ReferenceImageRequest,
    SceneEntry,
    TestEntry,
)
from ptv_schemas.asset_manifest import AssetCategory
from ptv_schemas.build_spec import GameType
from ptv_schemas.code_task import CodeTask, CodeTaskKind


# ---------------------------------------------------------------------------
# Fixtures / helpers
# ---------------------------------------------------------------------------


class _ListBus(EventBus):
    """Synchronous bus that records every published event."""

    def __init__(self) -> None:
        super().__init__()
        self.published: list[PipelineEvent] = []
        self._pub_lock = threading.Lock()

    def publish(self, event: PipelineEvent) -> None:
        with self._pub_lock:
            self.published.append(event)


def _seed(tmp_path: Path) -> tuple[ProjectStore, str, Settings]:
    settings = Settings(
        projects_dir=tmp_path / "projects",
        engine_dir=tmp_path / "engine",
        samples_dir=tmp_path / "samples",
        repo_root=tmp_path,
    )
    store = ProjectStore(settings.projects_dir)
    state = store.create("Demo", "prompt here")
    pid = state.project_id

    bs = BuildSpec(
        prompt_id="demo",
        title="Demo",
        pitch="A tiny shooter demo.",
        game_type=GameType.shooter,
        scenes=[SceneEntry(id="arena", name="Arena", summary="combat arena", entry_point=True)],
        mechanics=[
            MechanicEntry(id="grab", name="Grab", summary="hold weapons"),
            MechanicEntry(
                id="shooting_hub",
                name="Shooting Hub",
                type=MechanicType.hub,
                summary="route trigger to weapon profile",
            ),
            MechanicEntry(id="jump", name="Jump", summary="press a to jump"),
        ],
        assets=[AssetEntry(id="rifle", name="Rifle", category="prop", summary="a rifle")],
        acceptance_tests=[TestEntry(id="t1", description="can shoot")],
    )
    store.save_build_spec(pid, bs)

    manifest = AssetManifest(
        prompt_id="demo",
        items=[
            AssetItem(
                id="rifle",
                name="Rifle",
                category=AssetCategory.prop,
                style_notes="sci-fi rifle",
                reference=ReferenceImageRequest(prompt="rifle"),
                target_scale_meters=0.8,
            )
        ],
    )
    store.save_asset_manifest(pid, manifest)
    return store, pid, settings


def _ctx(store: ProjectStore, pid: str, settings: Settings) -> tuple[PipelineContext, _ListBus]:
    bus = _ListBus()
    return (
        PipelineContext(project_id=pid, settings=settings, store=store, bus=bus),
        bus,
    )


def _stub_planner(plan: MechanicPlan) -> MechanicPlanner:
    return MechanicPlanner(instructions="x", runner=lambda _msg: plan)


def _code_task(task_id: str, depends_on: list[str] | None = None) -> CodeTask:
    return CodeTask(
        id=task_id,
        kind=CodeTaskKind.mechanic,
        title=f"Task {task_id}",
        description="x" * 40,
        interface="IMechanic",
        depends_on=list(depends_on or []),
    )


# ---------------------------------------------------------------------------
# Stage tests (stubbed planner)
# ---------------------------------------------------------------------------


def test_stage_saves_plan_and_summary(tmp_path):
    store, pid, settings = _seed(tmp_path)
    ctx, bus = _ctx(store, pid, settings)

    plan = MechanicPlan(
        prompt_id="demo",
        items=[
            MechanicPlanItem(
                id="grab",
                classification=MechanicClassification.builtin_config_code_edit,
                base_builtin_id="builtin.grab",
                binding=ControllerBinding(
                    control=ControllerControl.grip, hand=ControllerHand.either
                ),
                code_task=_code_task("custom_grab"),
            ),
            MechanicPlanItem(
                id="shooting_hub",
                classification=MechanicClassification.generate,
                binding=ControllerBinding(
                    control=ControllerControl.trigger, hand=ControllerHand.right
                ),
                code_task=_code_task("shooting_hub"),
            ),
            MechanicPlanItem(
                id="jump",
                classification=MechanicClassification.generate,
                binding=ControllerBinding(
                    control=ControllerControl.a, hand=ControllerHand.right
                ),
                code_task=_code_task("jump", depends_on=[]),
            ),
        ],
    )

    result = stage_mechanic_plan.run_with_planner(ctx, planner=_stub_planner(plan))

    # Plan saved + on context.
    saved = store.load_mechanic_plan(pid)
    assert {it.id for it in saved.items} == {"grab", "shooting_hub", "jump"}
    assert ctx.mechanic_plan is not None
    assert result.data is plan

    statuses = [(e.status, e.message) for e in bus.published]
    assert any(s == "succeeded" for s, _ in statuses)
    # Summary mentions code_edit + generate counts.
    info = [m for s, m in statuses if s == "info"]
    assert any("code_edit=1" in m and "generate=2" in m for m in info), info


def test_stage_normalizes_face_button_hand(tmp_path):
    store, pid, settings = _seed(tmp_path)
    ctx, bus = _ctx(store, pid, settings)

    # 'a' bound to the LEFT hand is physically impossible (A is right-only).
    plan = MechanicPlan(
        prompt_id="demo",
        items=[
            MechanicPlanItem(
                id="jump",
                classification=MechanicClassification.generate,
                binding=ControllerBinding(
                    control=ControllerControl.a, hand=ControllerHand.left
                ),
                code_task=_code_task("jump"),
            ),
        ],
    )

    stage_mechanic_plan.run_with_planner(ctx, planner=_stub_planner(plan))

    saved = store.load_mechanic_plan(pid)
    jump = next(it for it in saved.items if it.id == "jump")
    assert jump.binding.hand == ControllerHand.right
    warnings = [e.message for e in bus.published if e.status == "warn"]
    assert any("right-controller only" in m for m in warnings), warnings


def test_stage_allows_shared_control_binding(tmp_path):
    """Two mechanics binding the same control on the same hand is allowed
    (e.g. a UI pointer clicking with the trigger while that hand also
    grabs / fires a weapon). The stage must NOT reject the plan."""
    store, pid, settings = _seed(tmp_path)
    ctx, bus = _ctx(store, pid, settings)

    plan = MechanicPlan(
        prompt_id="demo",
        items=[
            MechanicPlanItem(
                id="shooting_hub",
                classification=MechanicClassification.generate,
                binding=ControllerBinding(
                    control=ControllerControl.trigger, hand=ControllerHand.right
                ),
                code_task=_code_task("shooting_hub"),
            ),
            MechanicPlanItem(
                id="ui_pointer",
                classification=MechanicClassification.generate,
                binding=ControllerBinding(
                    control=ControllerControl.trigger, hand=ControllerHand.right
                ),
                code_task=_code_task("ui_pointer"),
            ),
        ],
    )

    stage_mechanic_plan.run_with_planner(ctx, planner=_stub_planner(plan))

    saved = store.load_mechanic_plan(pid)
    assert {it.id for it in saved.items} == {"shooting_hub", "ui_pointer"}
    statuses = [e.status for e in bus.published]
    assert "succeeded" in statuses
    assert "failed" not in statuses


# ---------------------------------------------------------------------------
# Binding validator unit tests
# ---------------------------------------------------------------------------


def _item(mid: str, control: ControllerControl, hand: ControllerHand) -> MechanicPlanItem:
    return MechanicPlanItem(
        id=mid,
        classification=MechanicClassification.generate,
        binding=ControllerBinding(control=control, hand=hand),
        code_task=_code_task(mid),
    )


def test_validator_allows_same_control_same_hand():
    """Sharing a control on the same hand is allowed (not a conflict)."""
    plan = MechanicPlan(
        prompt_id="demo",
        items=[
            _item("fire", ControllerControl.trigger, ControllerHand.right),
            _item("zoom", ControllerControl.trigger, ControllerHand.right),
        ],
    )
    res = validate_bindings(plan)
    assert res.ok
    assert not res.errors


def test_validator_allows_either_hand_with_specific_hand():
    """An 'either' binding alongside a specific-hand binding on the same
    control is allowed."""
    plan = MechanicPlan(
        prompt_id="demo",
        items=[
            _item("grab_all", ControllerControl.trigger, ControllerHand.either),
            _item("fire", ControllerControl.trigger, ControllerHand.right),
        ],
    )
    res = validate_bindings(plan)
    assert res.ok
    assert not res.errors


def test_validator_allows_same_control_different_hands():
    plan = MechanicPlan(
        prompt_id="demo",
        items=[
            _item("fire_left", ControllerControl.trigger, ControllerHand.left),
            _item("fire_right", ControllerControl.trigger, ControllerHand.right),
        ],
    )
    res = validate_bindings(plan)
    assert res.ok, res.errors


def test_validator_warns_on_engine_reserved_control():
    plan = MechanicPlan(
        prompt_id="demo",
        items=[_item("dash", ControllerControl.thumbstick, ControllerHand.left)],
    )
    res = validate_bindings(plan)
    assert res.ok  # reserved reuse is a warning, not an error
    assert any("locomotion" in w for w in res.warnings)


def test_validator_ignores_control_none():
    plan = MechanicPlan(
        prompt_id="demo",
        items=[
            _item("rifle_blast", ControllerControl.none, ControllerHand.right),
            _item("pistol_blast", ControllerControl.none, ControllerHand.right),
        ],
    )
    res = validate_bindings(plan)
    assert res.ok
    assert not res.errors
