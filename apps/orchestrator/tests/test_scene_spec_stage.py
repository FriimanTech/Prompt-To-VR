"""Tests for the parallel SceneSpec stage.

These tests exercise ``stage_scene_spec.run_with_client_factory`` with
an injected fake LLM client so no network calls happen. They verify:

* one LLM call per BuildSpec scene
* per-scene user messages are COMPRESSED (no full BuildSpec / Manifest /
  MechanicPlan dumps) and include pre-computed glb_path values
* the stage runs scene calls in PARALLEL but returns ``ctx.scene_specs``
  in BuildSpec order
* per-scene progress events are emitted
* per-scene + canonical scene_spec.json files are saved
* a single failing scene propagates the error (the stage cannot quietly
  proceed with a missing SceneSpec)
"""

from __future__ import annotations

import json
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import pytest

from ptv_orchestrator.events import EventBus, PipelineEvent
from ptv_orchestrator.pipeline import stage_scene_spec
from ptv_orchestrator.pipeline.context import PipelineContext
from ptv_orchestrator.settings import Settings
from ptv_orchestrator.storage import ProjectStore
from ptv_schemas import (
    AssetEntry,
    AssetItem,
    AssetManifest,
    BuildSpec,
    MechanicClassification,
    MechanicEntry,
    MechanicPlan,
    MechanicPlanItem,
    ReferenceImageRequest,
    SceneEntry,
    SceneSpec,
    TestEntry,
    Transform,
)
from ptv_schemas.asset_manifest import AssetCategory
from ptv_schemas.build_spec import GameType
from ptv_schemas.code_task import CodeTask, CodeTaskKind
from ptv_schemas.common import Vec3
from ptv_schemas.scene_spec import EntitySpec, MeshRef, PhysicsProps


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------


def _settings(tmp_path: Path, workers: int = 3) -> Settings:
    return Settings(
        projects_dir=tmp_path / "projects",
        engine_dir=tmp_path / "engine",
        samples_dir=tmp_path / "samples",
        repo_root=tmp_path,
        scene_spec_max_parallel_workers=workers,
    )


def _seed(tmp_path: Path, *, n_scenes: int, workers: int = 3) -> tuple[ProjectStore, str, Settings]:
    settings = _settings(tmp_path, workers=workers)
    store = ProjectStore(settings.projects_dir)
    state = store.create("Demo", "prompt here")
    pid = state.project_id

    scenes = []
    for i in range(n_scenes):
        scenes.append(
            SceneEntry(
                id=f"scene_{i:02d}",
                name=f"Scene {i}",
                summary=(
                    f"Scene {i} summary. Player explores area {i} with "
                    f"gauntlets and constructs. " * 4
                ),
                entry_point=(i == 0),
            )
        )

    bs = BuildSpec(
        prompt_id="demo",
        title="Demo",
        pitch="A tiny demo for tests.",
        game_type=GameType.shooter,
        scenes=scenes,
        mechanics=[
            MechanicEntry(
                id="grab",
                name="Grab Objects",
                summary="Right grip held grabs the nearest interactable. " * 20,
            ),
            MechanicEntry(
                id="shooting_hub",
                name="Shooting Hub",
                summary="Routes trigger to selected weapon profile. " * 20,
            ),
        ],
        assets=[
            AssetEntry(id="city", name="City", category="environment", summary="A city block."),
            AssetEntry(id="gauntlet", name="Gauntlet", category="prop", summary="Iron gauntlet."),
        ],
        acceptance_tests=[TestEntry(id="t1", description="grab works")],
    )
    store.save_build_spec(pid, bs)

    manifest = AssetManifest(
        prompt_id="demo",
        items=[
            AssetItem(
                id="city",
                name="City",
                category=AssetCategory.environment,
                style_notes=(
                    "Cinematic comic-book sci-fi city block with monumental alien "
                    "architecture and emerald energy channels. " * 18
                ),
                reference=ReferenceImageRequest(prompt="city"),
                target_scale_meters=18.0,
                collision_shape="mesh",
            ),
            AssetItem(
                id="gauntlet",
                name="Gauntlet",
                category=AssetCategory.prop,
                style_notes="Gold and emerald wearable gauntlet, grabbable.",
                reference=ReferenceImageRequest(prompt="gauntlet"),
                target_scale_meters=0.3,
            ),
        ],
    )
    store.save_asset_manifest(pid, manifest)

    plan = MechanicPlan(
        prompt_id="demo",
        items=[
            MechanicPlanItem(
                id="grab",
                classification=MechanicClassification.builtin,
                builtin_id="builtin.grab",
            ),
            MechanicPlanItem(
                id="shooting_hub",
                classification=MechanicClassification.generate,
                code_task=CodeTask(
                    id="shooting_hub",
                    kind=CodeTaskKind.mechanic,
                    title="Shooting Hub",
                    # Long description that MUST be stripped from the
                    # per-scene user message -- the SceneSpec composer
                    # doesn't need it and paying 10K chars per scene 15x
                    # was the source of the hang.
                    description="long impl spec - " * 500,
                    interface="IMechanic",
                ),
            ),
        ],
    )
    store.save_mechanic_plan(pid, plan)
    return store, pid, settings


def _ctx(
    store: ProjectStore, project_id: str, settings: Settings
) -> PipelineContext:
    return PipelineContext(
        project_id=project_id,
        settings=settings,
        store=store,
        bus=EventBus(),
    )


# ---------------------------------------------------------------------------
# Fake LLM client
# ---------------------------------------------------------------------------


@dataclass
class _Call:
    prompt: str
    schema: dict
    system: str


@dataclass
class _Resp:
    data: dict


class _FakeClient:
    """Records every ``generate_json`` call and returns a valid SceneSpec
    payload derived from the prompt's ``scene_id``."""

    def __init__(self, *, delay_seconds: float = 0.0):
        self._delay = delay_seconds
        self.calls: list[_Call] = []
        self._lock = threading.Lock()

    def generate_json(self, *, prompt: str, schema: Any, system: str) -> _Resp:
        if self._delay > 0:
            time.sleep(self._delay)
        with self._lock:
            self.calls.append(_Call(prompt=prompt, schema=dict(schema), system=system))
        scene_id = _scene_id_from_prompt(prompt)
        return _Resp(data=_make_scene_payload(scene_id))


def _scene_id_from_prompt(prompt: str) -> str:
    # The prompt starts with: ``Compose the SceneSpec for scene_id='scene_03', ...``
    marker = "scene_id="
    i = prompt.index(marker) + len(marker)
    raw = prompt[i:].split(",", 1)[0].strip()
    return raw.strip("'\"")


def _make_scene_payload(scene_id: str) -> dict:
    spec = SceneSpec(
        prompt_id="demo",
        scene_id=scene_id,
        name=f"Composed {scene_id}",
        entities=[
            EntitySpec(
                id="city",
                name="City",
                transform=Transform(position=Vec3(x=0.0, y=0.0, z=0.0)),
                mesh=MeshRef(asset_id="city", glb_path="assets/city.glb"),
                physics=PhysicsProps(kind="static", collision_shape="mesh"),
                tags=["environment"],
            ),
        ],
    )
    return spec.model_dump(mode="json")


def _factory_returning(client: _FakeClient):
    def factory(_settings: Settings):
        return client

    return factory


# ---------------------------------------------------------------------------
# Event capture
# ---------------------------------------------------------------------------


def _drain(bus: EventBus, project_id: str) -> list[PipelineEvent]:
    queue = bus._queues.get(project_id, [])  # noqa: SLF001
    if not queue:
        return []
    captured: list[PipelineEvent] = []
    q = queue[0]
    try:
        while True:
            captured.append(q.get_nowait())
    except Exception:
        pass
    return captured


class _ListBus(EventBus):
    """Synchronous bus that records every event without needing an
    asyncio loop (the production EventBus uses ``asyncio.Queue`` whose
    ``put_nowait`` requires a running loop on Windows). We only need the
    publication side here -- subscribers aren't exercised in tests."""

    def __init__(self) -> None:
        super().__init__()
        self.published: list[PipelineEvent] = []
        self._pub_lock = threading.Lock()

    def publish(self, event: PipelineEvent) -> None:
        with self._pub_lock:
            self.published.append(event)


def _ctx_with_bus(
    store: ProjectStore, project_id: str, settings: Settings
) -> tuple[PipelineContext, _ListBus]:
    bus = _ListBus()
    ctx = PipelineContext(
        project_id=project_id,
        settings=settings,
        store=store,
        bus=bus,
    )
    return ctx, bus


# ---------------------------------------------------------------------------
# Tests: prompt compression
# ---------------------------------------------------------------------------


def test_compress_asset_truncates_style_notes_and_sets_glb_path(tmp_path):
    store, pid, settings = _seed(tmp_path, n_scenes=2)
    manifest = store.load_asset_manifest(pid)
    city = next(a for a in manifest.items if a.id == "city")

    compact = stage_scene_spec._compress_asset(city)

    assert compact["id"] == "city"
    assert compact["glb_path"] == "assets/city.glb"
    assert len(compact["style_notes"]) <= 410, (
        f"style_notes was {len(compact['style_notes'])} chars; "
        "the compression layer is supposed to truncate to ~400"
    )
    # Truncation marker
    assert compact["style_notes"].endswith("...")


def test_compress_mechanics_emits_binding_ids(tmp_path):
    store, pid, _ = _seed(tmp_path, n_scenes=1)
    bs = store.load_build_spec(pid)
    plan = store.load_mechanic_plan(pid)

    compact = stage_scene_spec._compress_mechanics(bs, plan)

    by_id = {m["id"]: m for m in compact}
    # Built-in: binding_id is the builtin id directly.
    assert by_id["grab"]["binding_id"] == "builtin.grab"
    assert by_id["grab"]["classification"] == "builtin"
    # Generate: binding_id is generated.<code_task_id>.
    assert by_id["shooting_hub"]["binding_id"] == "generated.shooting_hub"
    assert by_id["shooting_hub"]["classification"] == "generate"
    # The huge code_task.description must NOT appear anywhere on the
    # compressed entry -- that's the key savings.
    for entry in compact:
        for v in entry.values():
            if isinstance(v, str):
                assert "long impl spec -" not in v, (
                    "compressed mechanics must not leak the long "
                    "code_task.description into the per-scene prompt"
                )


def test_user_message_omits_full_code_task_description(tmp_path):
    """Regression guard: the per-scene LLM prompt MUST NOT contain the
    full 10K-char ``code_task.description`` blocks, or we're back to the
    39K-token-per-scene problem that made the stage appear hung."""
    store, pid, _ = _seed(tmp_path, n_scenes=2)
    bs = store.load_build_spec(pid)
    manifest = store.load_asset_manifest(pid)
    plan = store.load_mechanic_plan(pid)

    static = stage_scene_spec._StaticContext.build(bs, manifest, plan)
    msg = stage_scene_spec._build_user_message(bs.scenes[0], static)

    assert "long impl spec -" not in msg
    # The full BuildSpec mechanic.summary (which is "Right grip held..."
    # repeated 20 times) must not be present verbatim either.
    huge_grab_summary = "Right grip held grabs the nearest interactable. " * 20
    assert huge_grab_summary not in msg
    # The pre-computed glb_path is present so the LLM doesn't invent one.
    assert "assets/city.glb" in msg
    assert "assets/gauntlet.glb" in msg


def test_user_message_focuses_on_this_scene(tmp_path):
    store, pid, _ = _seed(tmp_path, n_scenes=3)
    bs = store.load_build_spec(pid)
    manifest = store.load_asset_manifest(pid)
    plan = store.load_mechanic_plan(pid)

    static = stage_scene_spec._StaticContext.build(bs, manifest, plan)
    msg = stage_scene_spec._build_user_message(bs.scenes[1], static)

    # THIS_SCENE carries the full target summary.
    assert "scene_01" in msg
    assert "THIS_SCENE" in msg
    # Other scenes appear only as id+name (used by the composer for
    # nav-tag wiring) -- NOT with their full summaries.
    assert "OTHER_SCENES" in msg
    # Find the OTHER_SCENES JSON block and assert the OTHER scene
    # summaries are absent from it.
    other_summary_fragment = (
        "Player explores area 2 with gauntlets and constructs."
    )
    # That summary belongs to scene_02 (not scene_01), so it must be
    # absent from the user message for scene_01.
    assert other_summary_fragment not in msg


# ---------------------------------------------------------------------------
# Tests: end-to-end orchestration
# ---------------------------------------------------------------------------


def test_one_llm_call_per_scene_and_order_preserved(tmp_path):
    store, pid, settings = _seed(tmp_path, n_scenes=5)
    ctx, _ = _ctx_with_bus(store, pid, settings)
    client = _FakeClient()

    result = stage_scene_spec.run_with_client_factory(
        ctx, _factory_returning(client)
    )

    assert len(client.calls) == 5, f"expected 5 LLM calls, got {len(client.calls)}"
    # ctx.scene_specs preserves BuildSpec order regardless of which
    # worker finished first.
    assert [s.scene_id for s in ctx.scene_specs] == [
        "scene_00",
        "scene_01",
        "scene_02",
        "scene_03",
        "scene_04",
    ]
    # The composed StageResult mirrors the same ordering.
    assert [s.scene_id for s in result.data] == [
        "scene_00",
        "scene_01",
        "scene_02",
        "scene_03",
        "scene_04",
    ]
    # Both the per-scene file AND the canonical scene_spec.json (for
    # the entry-point scene_00) were written.
    proj_dir = store.dir_for(pid)
    for i in range(5):
        assert (proj_dir / f"scene_spec.scene_{i:02d}.json").exists()
    assert (proj_dir / "scene_spec.json").exists()


def test_scenes_run_in_parallel_when_workers_gt_one(tmp_path):
    """When concurrency > 1, total wall-clock should be roughly
    ``ceil(n / workers) * delay`` instead of ``n * delay``. We use a
    deliberately slow fake client to make the ratio observable."""
    store, pid, settings = _seed(tmp_path, n_scenes=6, workers=3)
    ctx, _ = _ctx_with_bus(store, pid, settings)
    # 200ms per call. Serial = 1.2s. With 3 workers should be ~0.4s.
    client = _FakeClient(delay_seconds=0.2)

    t0 = time.monotonic()
    stage_scene_spec.run_with_client_factory(
        ctx, _factory_returning(client)
    )
    elapsed = time.monotonic() - t0

    # Generous bound so test isn't flaky on slow CI. Serial would take
    # at LEAST 1.2s; we expect comfortably under 1.0s.
    assert elapsed < 0.95, (
        f"SceneSpec stage took {elapsed:.2f}s for 6 scenes x 0.2s with "
        f"3 workers; parallel execution looks broken (would be ~1.2s "
        f"serially)"
    )


def test_emits_per_scene_progress_events(tmp_path):
    store, pid, settings = _seed(tmp_path, n_scenes=3)
    ctx, bus = _ctx_with_bus(store, pid, settings)
    client = _FakeClient()

    stage_scene_spec.run_with_client_factory(
        ctx, _factory_returning(client)
    )

    # Drain everything published during the run.
    msgs = [(e.status, e.message) for e in bus.published]
    info_msgs = [m for s, m in msgs if s == "info"]
    assert any("composing 3 scene(s)" in m for m in info_msgs), info_msgs
    # Per-scene start + done markers for each scene id.
    for sid in ("scene_00", "scene_01", "scene_02"):
        assert any(
            f"starting scene '{sid}'" in m for m in info_msgs
        ), f"no 'starting' event for {sid}"
        assert any(
            f"done scene '{sid}'" in m for m in info_msgs
        ), f"no 'done' event for {sid}"
    # Final stage status was 'succeeded'.
    assert any(s == "succeeded" for s, _ in msgs)


def test_failing_scene_propagates_and_does_not_succeed_stage(tmp_path):
    store, pid, settings = _seed(tmp_path, n_scenes=3)
    ctx, bus = _ctx_with_bus(store, pid, settings)

    class _BoomClient(_FakeClient):
        def generate_json(self, *, prompt, schema, system):  # type: ignore[override]
            scene_id = _scene_id_from_prompt(prompt)
            if scene_id == "scene_01":
                raise RuntimeError("simulated LLM blow-up on scene_01")
            return super().generate_json(prompt=prompt, schema=schema, system=system)

    client = _BoomClient()
    with pytest.raises(RuntimeError, match="scene_01"):
        stage_scene_spec.run_with_client_factory(
            ctx, _factory_returning(client)
        )

    # Stage was not stamped 'succeeded'.
    states = [e.status for e in bus.published]
    assert "succeeded" not in states
    # We DID emit a FAILED-marker info event for the bad scene.
    assert any("FAILED scene 'scene_01'" in e.message for e in bus.published)


def test_run_default_factory_constructs_make_llm(tmp_path, monkeypatch):
    """Smoke-test that the public ``run`` wires through ``make_llm`` and
    accepts a stub client built by patching it."""
    store, pid, settings = _seed(tmp_path, n_scenes=2)
    ctx, _ = _ctx_with_bus(store, pid, settings)

    client = _FakeClient()

    def fake_make_llm(_settings):
        return client

    monkeypatch.setattr(stage_scene_spec, "make_llm", fake_make_llm)

    stage_scene_spec.run(ctx)
    assert len(client.calls) == 2


# ---------------------------------------------------------------------------
# Tests: idempotent resume (the "Resume just sits at running" fix)
# ---------------------------------------------------------------------------


def test_resume_skips_scenes_already_on_disk(tmp_path):
    """The killer case: a prior run produced every scene_spec file but
    was killed before the stage could stamp success. A subsequent Resume
    must NOT re-run any LLM calls -- it should reuse the disk files and
    mark the stage succeeded in well under a second."""
    store, pid, settings = _seed(tmp_path, n_scenes=4)
    ctx, bus = _ctx_with_bus(store, pid, settings)
    client = _FakeClient()

    # First run: composes all 4 scenes via LLM and writes them to disk.
    stage_scene_spec.run_with_client_factory(ctx, _factory_returning(client))
    assert len(client.calls) == 4
    proj_dir = store.dir_for(pid)
    for i in range(4):
        assert (proj_dir / f"scene_spec.scene_{i:02d}.json").exists()

    # Second run on a fresh context: every scene should be reused from
    # disk; the fake client must NOT be called again.
    ctx2, bus2 = _ctx_with_bus(store, pid, settings)
    client2 = _FakeClient()
    result = stage_scene_spec.run_with_client_factory(
        ctx2, _factory_returning(client2)
    )
    assert len(client2.calls) == 0, (
        "second run made LLM calls; idempotent resume is broken. "
        f"calls={[c.prompt[:80] for c in client2.calls]}"
    )
    # Outputs still preserve BuildSpec order.
    assert [s.scene_id for s in result.data] == [
        "scene_00",
        "scene_01",
        "scene_02",
        "scene_03",
    ]
    # Stage was stamped 'succeeded' on this resume too.
    statuses = [e.status for e in bus2.published]
    assert "succeeded" in statuses
    # And the UI saw an explicit "all on disk" message.
    info_msgs = [e.message for e in bus2.published if e.status == "info"]
    assert any("already on disk" in m for m in info_msgs), info_msgs


def test_resume_recomposes_only_missing_scenes(tmp_path):
    """Mixed case: 3 of 5 scenes already exist; only the missing 2 hit
    the LLM. Tests the partial-resume code path."""
    store, pid, settings = _seed(tmp_path, n_scenes=5)
    ctx, _ = _ctx_with_bus(store, pid, settings)
    client = _FakeClient()

    # Compose everything once, then nuke 2 of the per-scene files.
    stage_scene_spec.run_with_client_factory(ctx, _factory_returning(client))
    proj_dir = store.dir_for(pid)
    (proj_dir / "scene_spec.scene_01.json").unlink()
    (proj_dir / "scene_spec.scene_03.json").unlink()

    # Fresh client + ctx: only scenes 01 and 03 should fire an LLM call.
    ctx2, _ = _ctx_with_bus(store, pid, settings)
    client2 = _FakeClient()
    stage_scene_spec.run_with_client_factory(ctx2, _factory_returning(client2))
    assert len(client2.calls) == 2
    scene_ids_called = {_scene_id_from_prompt(c.prompt) for c in client2.calls}
    assert scene_ids_called == {"scene_01", "scene_03"}
    # All five files are present once we're done.
    for i in range(5):
        assert (proj_dir / f"scene_spec.scene_{i:02d}.json").exists()


def test_resume_regenerates_corrupt_scene_file(tmp_path):
    """A half-written file from a crashed prior run must not be silently
    reused. The stage should treat invalid JSON / failed Pydantic
    validation as "missing" and re-compose just that scene."""
    store, pid, settings = _seed(tmp_path, n_scenes=3)
    ctx, _ = _ctx_with_bus(store, pid, settings)
    client = _FakeClient()
    stage_scene_spec.run_with_client_factory(ctx, _factory_returning(client))
    assert len(client.calls) == 3

    proj_dir = store.dir_for(pid)
    # Corrupt scene_01's file. Invalid JSON triggers the "missing" path.
    (proj_dir / "scene_spec.scene_01.json").write_text(
        "{ not valid json at all", encoding="utf-8"
    )

    ctx2, _ = _ctx_with_bus(store, pid, settings)
    client2 = _FakeClient()
    stage_scene_spec.run_with_client_factory(ctx2, _factory_returning(client2))
    # Exactly one re-composed scene (the corrupted one).
    assert len(client2.calls) == 1
    assert _scene_id_from_prompt(client2.calls[0].prompt) == "scene_01"
    # And the file is now valid JSON again.
    spec = store.load_scene_spec(pid, filename="scene_spec.scene_01.json")
    assert spec.scene_id == "scene_01"


def test_resume_regenerates_file_with_mismatched_scene_id(tmp_path):
    """A scene_spec_X.json that holds a scene with a different id (e.g.
    stale after a rename) is considered invalid and re-generated."""
    store, pid, settings = _seed(tmp_path, n_scenes=2)
    ctx, _ = _ctx_with_bus(store, pid, settings)
    client = _FakeClient()
    stage_scene_spec.run_with_client_factory(ctx, _factory_returning(client))

    proj_dir = store.dir_for(pid)
    # Replace scene_00's file with content whose scene_id says something else.
    bogus = SceneSpec(
        prompt_id="demo",
        scene_id="something_else",
        name="Stale",
        entities=[],
    )
    (proj_dir / "scene_spec.scene_00.json").write_text(
        bogus.model_dump_json(indent=2, by_alias=True), encoding="utf-8"
    )

    ctx2, _ = _ctx_with_bus(store, pid, settings)
    client2 = _FakeClient()
    stage_scene_spec.run_with_client_factory(ctx2, _factory_returning(client2))
    # Exactly the mismatched scene is re-composed.
    assert len(client2.calls) == 1
    assert _scene_id_from_prompt(client2.calls[0].prompt) == "scene_00"


def test_fast_finish_records_succeeded_without_llm_call(tmp_path):
    """The "all on disk" fast path must stamp ProjectStatus.succeeded
    on the state file. Without that stamp the next Resume would re-run
    the stage entirely -- which is the exact bug we're fixing."""
    from ptv_schemas import ProjectStage as _ProjectStage, ProjectStatus as _PS

    store, pid, settings = _seed(tmp_path, n_scenes=3)
    ctx, _ = _ctx_with_bus(store, pid, settings)
    client = _FakeClient()
    stage_scene_spec.run_with_client_factory(ctx, _factory_returning(client))

    ctx2, _ = _ctx_with_bus(store, pid, settings)
    client2 = _FakeClient()
    stage_scene_spec.run_with_client_factory(ctx2, _factory_returning(client2))
    assert len(client2.calls) == 0

    state = store.load_state(pid)
    scene_stages = [
        s for s in state.stages if s.stage == _ProjectStage.scene_spec
    ]
    # The latest record for scene_spec is succeeded (this is what
    # resume_pipeline looks at).
    assert scene_stages[-1].status == _PS.succeeded


def test_heartbeat_emits_for_slow_scenes(tmp_path):
    """A scene that takes longer than the heartbeat interval should
    receive at least one "still composing" info event. Without this the
    user can't tell whether a long LLM call is alive or hung."""
    store, pid, settings = _seed(tmp_path, n_scenes=1)
    ctx, bus = _ctx_with_bus(store, pid, settings)
    # 2s scene, 1s heartbeat -> at least one heartbeat fires.
    client = _FakeClient(delay_seconds=2.2)

    # Reach into the module to lower the heartbeat interval for the test.
    real_heartbeat_cls = stage_scene_spec._Heartbeat

    class _FastHeartbeat(real_heartbeat_cls):
        def __init__(self, *, ctx, total, interval_s: float = 1.0):
            super().__init__(ctx=ctx, total=total, interval_s=interval_s)

    import unittest.mock as _mock

    with _mock.patch.object(stage_scene_spec, "_Heartbeat", _FastHeartbeat):
        stage_scene_spec.run_with_client_factory(
            ctx, _factory_returning(client)
        )

    info_msgs = [e.message for e in bus.published if e.status == "info"]
    assert any("still composing" in m for m in info_msgs), (
        f"no heartbeat event was emitted for a 2.2s scene. "
        f"info messages were: {info_msgs}"
    )


def test_no_heartbeat_for_fast_scenes(tmp_path):
    """The heartbeat must not spam fast scenes. A scene that finishes
    well within the interval should produce zero heartbeat events."""
    store, pid, settings = _seed(tmp_path, n_scenes=3)
    ctx, bus = _ctx_with_bus(store, pid, settings)
    client = _FakeClient()  # no delay

    stage_scene_spec.run_with_client_factory(ctx, _factory_returning(client))

    info_msgs = [e.message for e in bus.published if e.status == "info"]
    assert not any("still composing" in m for m in info_msgs), info_msgs
