"""The project's target device is the single source of truth.

``ProjectState.target_device`` comes from the New Project toggle. Before
this was wired up, the build_spec stage never told the planner which device
was selected and never corrected the answer, so a Quest project happily
wrote ``"target_device": "pcvr"`` into ``build_spec.json`` (the schema
default) -- and because nothing downstream read that field, the mismatch
went unnoticed until someone opened the artifact.

These tests pin both halves of the fix: the prompt names the real target,
and the stage overwrites whatever the model returned.
"""

from __future__ import annotations

import threading
from pathlib import Path
from types import SimpleNamespace

import pytest

from ptv_orchestrator.events import EventBus, PipelineEvent
from ptv_orchestrator.pipeline import orchestrator, prompts, stage_build_spec
from ptv_orchestrator.pipeline.context import PipelineContext
from ptv_orchestrator.pipeline.stage_scene_spec import _default_composer_factory
from ptv_orchestrator.pipeline.target_device import sync_build_spec_device
from ptv_orchestrator.settings import Settings
from ptv_orchestrator.storage import ProjectStore
from ptv_schemas import (
    AssetEntry,
    BuildSpec,
    MechanicEntry,
    SceneEntry,
    TargetDevice,
    TestEntry,
)
from ptv_schemas.build_spec import GameType


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------


class _ListBus(EventBus):
    """Records events without needing a running asyncio loop."""

    def __init__(self) -> None:
        super().__init__()
        self.published: list[PipelineEvent] = []
        self._pub_lock = threading.Lock()

    def publish(self, event: PipelineEvent) -> None:
        with self._pub_lock:
            self.published.append(event)


def _settings(tmp_path: Path) -> Settings:
    return Settings(
        projects_dir=tmp_path / "projects",
        engine_dir=tmp_path / "engine",
        samples_dir=tmp_path / "samples",
        repo_root=tmp_path,
    )


def _build_spec(device: TargetDevice) -> BuildSpec:
    return BuildSpec(
        prompt_id="range-ready",
        title="Range Ready",
        pitch="A shooting range you can actually run around in.",
        game_type=GameType.shooter,
        target_device=device,
        scenes=[
            SceneEntry(
                id="shooting_range",
                name="Shooting Range",
                summary="An outdoor range with lanes and paper targets.",
                entry_point=True,
            )
        ],
        mechanics=[
            MechanicEntry(
                id="shoot",
                name="Shoot",
                summary="Right trigger fires the held weapon.",
            )
        ],
        assets=[
            AssetEntry(
                id="pistol", name="Pistol", category="prop", summary="A pistol."
            )
        ],
        acceptance_tests=[TestEntry(id="t1", description="the pistol fires")],
    )


def _seed(tmp_path: Path, device: TargetDevice) -> tuple[ProjectStore, str, Settings]:
    settings = _settings(tmp_path)
    store = ProjectStore(settings.projects_dir)
    state = store.create("Range Ready", "a shooting range game", target_device=device)
    return store, state.project_id, settings


def _ctx(store: ProjectStore, pid: str, settings: Settings):
    bus = _ListBus()
    return (
        PipelineContext(project_id=pid, settings=settings, store=store, bus=bus),
        bus,
    )


class _FakeLlm:
    """Captures the system / user prompt and replays a canned BuildSpec."""

    def __init__(self, returns: BuildSpec) -> None:
        self.returns = returns
        self.system = ""
        self.prompt = ""

    def generate_json(self, *, prompt: str, schema: dict, system: str):
        self.system = system
        self.prompt = prompt
        return SimpleNamespace(data=self.returns.model_dump(mode="json"))


# ---------------------------------------------------------------------------
# Prompt rendering
# ---------------------------------------------------------------------------


@pytest.mark.parametrize(
    "accessor",
    [
        prompts.build_spec_system,
        prompts.mechanic_plan_system,
        prompts.scene_spec_system,
        prompts.code_task_system,
        prompts.world_builder_system,
    ],
)
def test_every_prompt_names_the_actual_target(accessor) -> None:
    quest = accessor(TargetDevice.quest)
    pcvr = accessor(TargetDevice.pcvr)

    assert prompts.DEVICE_TOKEN not in quest
    assert prompts.DEVICE_TOKEN not in pcvr
    assert "QUEST STANDALONE" in quest and "Snapdragon" in quest
    assert "QUEST STANDALONE" not in pcvr
    assert "TARGETS PC VR" in pcvr


def test_module_constants_are_the_pcvr_rendering() -> None:
    """Kept so existing imports and prompt regressions keep working."""
    assert prompts.BUILD_SPEC_SYSTEM == prompts.build_spec_system(TargetDevice.pcvr)
    assert prompts.CODE_TASK_SYSTEM == prompts.code_task_system(TargetDevice.pcvr)
    assert prompts.SCENE_SPEC_SYSTEM == prompts.scene_spec_system(TargetDevice.pcvr)
    assert prompts.MECHANIC_PLAN_SYSTEM == prompts.mechanic_plan_system(
        TargetDevice.pcvr
    )
    assert prompts.WORLD_BUILDER_SYSTEM == prompts.world_builder_system(
        TargetDevice.pcvr
    )


# ---------------------------------------------------------------------------
# The stage overrides the model
# ---------------------------------------------------------------------------


def test_sync_reports_whether_it_changed_anything() -> None:
    spec = _build_spec(TargetDevice.pcvr)

    same, changed = sync_build_spec_device(spec, TargetDevice.pcvr)
    assert changed is False and same.target_device == TargetDevice.pcvr

    fixed, changed = sync_build_spec_device(spec, TargetDevice.quest)
    assert changed is True and fixed.target_device == TargetDevice.quest
    # The input is not mutated: callers rely on model_copy semantics.
    assert spec.target_device == TargetDevice.pcvr


def test_quest_project_overrides_the_models_pcvr_answer(
    tmp_path: Path, monkeypatch
) -> None:
    store, pid, settings = _seed(tmp_path, TargetDevice.quest)
    ctx, bus = _ctx(store, pid, settings)

    fake = _FakeLlm(returns=_build_spec(TargetDevice.pcvr))
    monkeypatch.setattr(stage_build_spec, "make_llm", lambda _s: fake)

    result = stage_build_spec.run(ctx)

    assert result.data.target_device == TargetDevice.quest
    assert store.load_build_spec(pid).target_device == TargetDevice.quest

    # The planner was told which device it is planning for, both ways.
    assert "QUEST STANDALONE" in fake.system
    assert "'quest'" in fake.prompt

    messages = [e.message for e in bus.published]
    assert any("target_device" in m and "quest" in m for m in messages), messages


def test_matching_device_is_left_alone(tmp_path: Path, monkeypatch) -> None:
    store, pid, settings = _seed(tmp_path, TargetDevice.pcvr)
    ctx, bus = _ctx(store, pid, settings)

    fake = _FakeLlm(returns=_build_spec(TargetDevice.pcvr))
    monkeypatch.setattr(stage_build_spec, "make_llm", lambda _s: fake)

    stage_build_spec.run(ctx)

    assert store.load_build_spec(pid).target_device == TargetDevice.pcvr
    assert "TARGETS PC VR" in fake.system
    assert not [
        e for e in bus.published if "replaced the model" in e.message
    ], "nothing to override, so no override event"


# ---------------------------------------------------------------------------
# Existing projects self-heal without a regeneration
# ---------------------------------------------------------------------------


def test_stale_build_spec_is_restamped_at_run_start(tmp_path: Path) -> None:
    store, pid, settings = _seed(tmp_path, TargetDevice.quest)
    # What a pre-fix run left on disk: the schema default.
    store.save_build_spec(pid, _build_spec(TargetDevice.pcvr))
    ctx, bus = _ctx(store, pid, settings)

    orchestrator._reconcile_build_spec_device(ctx)

    assert store.load_build_spec(pid).target_device == TargetDevice.quest
    assert ctx.build_spec is not None
    assert ctx.build_spec.target_device == TargetDevice.quest
    assert any("re-stamped" in e.message for e in bus.published)


def test_reconcile_is_a_no_op_without_a_build_spec(tmp_path: Path) -> None:
    store, pid, settings = _seed(tmp_path, TargetDevice.quest)
    ctx, _bus = _ctx(store, pid, settings)

    orchestrator._reconcile_build_spec_device(ctx)  # must not raise

    assert ctx.build_spec is None


# ---------------------------------------------------------------------------
# Wiring: the scene composer is built per device
# ---------------------------------------------------------------------------


def test_scene_composer_instructions_follow_the_device(tmp_path: Path) -> None:
    settings = _settings(tmp_path)

    quest = _default_composer_factory(settings, "sid", TargetDevice.quest)
    pcvr = _default_composer_factory(settings, "sid", TargetDevice.pcvr)

    assert "QUEST STANDALONE" in quest.instructions
    assert "QUEST STANDALONE" not in pcvr.instructions
