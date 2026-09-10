"""Tests for the parallel SceneSpec stage.

These tests exercise ``stage_scene_spec.run_with_composer_factory`` with
an injected fake SceneSpec composer so no network / Agents SDK calls
happen. They verify:

* one agent call per BuildSpec scene
* per-scene user messages are COMPRESSED (no full BuildSpec / Manifest /
  MechanicPlan dumps) and include pre-computed glb_path values
* the stage runs scene composes in PARALLEL but returns ``ctx.scene_specs``
  in BuildSpec order
* per-scene progress events are emitted
* per-scene + canonical scene_spec.json files are saved
* a single failing scene propagates the error (the stage cannot quietly
  proceed with a missing SceneSpec)
* the validate-and-repair loop fixes a semantically invalid first output
  and fails the stage when the repair budget is exhausted
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
from ptv_orchestrator.pipeline.scene_spec_agent import SceneSpecOutputError
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
    TargetDevice,
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
    scene_id: str


class _Registry:
    """Shared call log across all per-scene fake composers (each scene
    gets its own composer instance, so we aggregate here)."""

    def __init__(self) -> None:
        self.calls: list[_Call] = []
        self._lock = threading.Lock()

    def record(self, scene_id: str, prompt: str) -> None:
        with self._lock:
            self.calls.append(_Call(prompt=prompt, scene_id=scene_id))


# ``payload_fn(scene_id, attempt) -> SceneSpec | Exception-raiser``. ``attempt``
# is 0 for the initial compose, 1+ for repair turns on the SAME scene.
PayloadFn = Any


class _FakeComposer:
    """Fake :class:`SceneSpecComposer`. Records every ``compose`` call and
    returns a SceneSpec for its scene. The scene_id is captured from the
    FIRST (initial) message; repair turns reuse it since they carry no id.
    """

    def __init__(
        self,
        *,
        registry: _Registry,
        delay_seconds: float = 0.0,
        payload_fn: PayloadFn = None,
    ) -> None:
        self._registry = registry
        self._delay = delay_seconds
        self._payload_fn = payload_fn
        self._scene_id: str | None = None
        self._attempt = 0

    def compose(self, message: str) -> SceneSpec:
        if self._delay > 0:
            time.sleep(self._delay)
        if self._scene_id is None:
            self._scene_id = _scene_id_from_prompt(message)
        self._registry.record(self._scene_id, message)
        attempt = self._attempt
        self._attempt += 1
        if self._payload_fn is not None:
            return self._payload_fn(self._scene_id, attempt)
        return _make_valid_scene(self._scene_id)


def _scene_id_from_prompt(prompt: str) -> str:
    # The prompt starts with: ``Compose the SceneSpec for scene_id='scene_03', ...``
    marker = "scene_id="
    i = prompt.index(marker) + len(marker)
    raw = prompt[i:].split(",", 1)[0].strip()
    return raw.strip("'\"")


def _make_valid_scene(scene_id: str) -> SceneSpec:
    """A semantically valid SceneSpec for the seed manifest (city env +
    gauntlet prop): every asset appears, an environment entity exists."""
    return SceneSpec(
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
            EntitySpec(
                id="gauntlet",
                name="Gauntlet",
                transform=Transform(position=Vec3(x=0.5, y=1.0, z=-0.5)),
                mesh=MeshRef(asset_id="gauntlet", glb_path="assets/gauntlet.glb"),
                physics=PhysicsProps(kind="dynamic"),
                tags=["interactable", "grabbable"],
            ),
        ],
    )


def _make_scene_missing_asset(scene_id: str) -> SceneSpec:
    """An INVALID SceneSpec: the gauntlet asset never appears, so
    ``_validate_scene_semantics`` reports it as missing."""
    return SceneSpec(
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


def _composer_factory(
    registry: _Registry,
    *,
    delay_seconds: float = 0.0,
    payload_fn: PayloadFn = None,
):
    """Return a ComposerFactory ``(settings, session_id, device, log) ->
    _FakeComposer``."""

    def factory(
        _settings: Settings,
        _session_id: str,
        _target_device: TargetDevice = TargetDevice.pcvr,
        _log: Any = None,
    ) -> _FakeComposer:
        return _FakeComposer(
            registry=registry,
            delay_seconds=delay_seconds,
            payload_fn=payload_fn,
        )

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
    reg = _Registry()

    result = stage_scene_spec.run_with_composer_factory(
        ctx, _composer_factory(reg)
    )

    assert len(reg.calls) == 5, f"expected 5 agent calls, got {len(reg.calls)}"
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
    reg = _Registry()

    t0 = time.monotonic()
    stage_scene_spec.run_with_composer_factory(
        ctx, _composer_factory(reg, delay_seconds=0.2)
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
    reg = _Registry()

    stage_scene_spec.run_with_composer_factory(
        ctx, _composer_factory(reg)
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

    def payload(scene_id: str, attempt: int) -> SceneSpec:
        if scene_id == "scene_01":
            raise RuntimeError("simulated LLM blow-up on scene_01")
        return _make_valid_scene(scene_id)

    reg = _Registry()
    with pytest.raises(RuntimeError, match="scene_01"):
        stage_scene_spec.run_with_composer_factory(
            ctx, _composer_factory(reg, payload_fn=payload)
        )

    # Stage was not stamped 'succeeded'.
    states = [e.status for e in bus.published]
    assert "succeeded" not in states
    # We DID emit a FAILED-marker info event for the bad scene.
    assert any("FAILED scene 'scene_01'" in e.message for e in bus.published)


def test_run_default_factory_uses_composer(tmp_path, monkeypatch):
    """Smoke-test that the public ``run`` wires through the default
    composer factory, accepting a stub composer built by patching it."""
    store, pid, settings = _seed(tmp_path, n_scenes=2)
    ctx, _ = _ctx_with_bus(store, pid, settings)

    reg = _Registry()
    monkeypatch.setattr(
        stage_scene_spec, "_default_composer_factory", _composer_factory(reg)
    )

    stage_scene_spec.run(ctx)
    assert len(reg.calls) == 2


# ---------------------------------------------------------------------------
# Tests: validate-and-repair loop
# ---------------------------------------------------------------------------


def test_semantic_repair_then_succeeds(tmp_path):
    """A first compose that omits an asset (semantic failure) is repaired
    on the next turn; the scene succeeds and a repair event is emitted."""
    store, pid, settings = _seed(tmp_path, n_scenes=1)
    ctx, bus = _ctx_with_bus(store, pid, settings)

    def payload(scene_id: str, attempt: int) -> SceneSpec:
        # First attempt is missing the gauntlet asset -> semantic failure.
        if attempt == 0:
            return _make_scene_missing_asset(scene_id)
        return _make_valid_scene(scene_id)

    reg = _Registry()
    result = stage_scene_spec.run_with_composer_factory(
        ctx, _composer_factory(reg, payload_fn=payload)
    )

    # Composed twice for the one scene: initial (bad) + one repair (good).
    assert len(reg.calls) == 2
    assert [c.scene_id for c in reg.calls] == ["scene_00", "scene_00"]
    assert [s.scene_id for s in result.data] == ["scene_00"]

    statuses = [(e.status, e.message) for e in bus.published]
    assert any(s == "succeeded" for s, _ in statuses)
    info_msgs = [m for s, m in statuses if s == "info"]
    assert any("repairing scene 'scene_00'" in m for m in info_msgs), info_msgs

    # The persisted scene is the repaired (valid) one with both assets.
    saved = store.load_scene_spec(pid, filename="scene_spec.scene_00.json")
    asset_ids = {e.mesh.asset_id for e in saved.entities if e.mesh}
    assert {"city", "gauntlet"} <= asset_ids


def test_truncated_output_is_repaired_with_a_smaller_ask(tmp_path):
    """A compose cut off at the token cap must not be answered with "fix
    these problems" -- there is nothing wrong with the content, there is
    just too much of it, so the repair turn asks for fewer entities."""
    store, pid, settings = _seed(tmp_path, n_scenes=1)
    settings.scene_spec_max_entities = 42
    ctx, bus = _ctx_with_bus(store, pid, settings)

    def payload(scene_id: str, attempt: int) -> SceneSpec:
        if attempt == 0:
            raise SceneSpecOutputError(
                "model output was cut off after 60000 chars",
                issues=[],
                truncated=True,
            )
        return _make_valid_scene(scene_id)

    reg = _Registry()
    result = stage_scene_spec.run_with_composer_factory(
        ctx, _composer_factory(reg, payload_fn=payload)
    )

    assert [s.scene_id for s in result.data] == ["scene_00"]
    assert len(reg.calls) == 2
    repair_prompt = reg.calls[1].prompt
    assert "CUT OFF" in repair_prompt
    assert "at most 42 entities" in repair_prompt


def test_output_problems_are_surfaced_per_field(tmp_path):
    """The field-level issues reach the UI instead of one redacted line."""
    store, pid, settings = _seed(tmp_path, n_scenes=1)
    ctx, bus = _ctx_with_bus(store, pid, settings)

    def payload(scene_id: str, attempt: int) -> SceneSpec:
        if attempt == 0:
            raise SceneSpecOutputError(
                "model output failed schema validation (2 problem(s))",
                issues=["entities.0.name: Input should be a valid string"],
            )
        return _make_valid_scene(scene_id)

    reg = _Registry()
    stage_scene_spec.run_with_composer_factory(
        ctx, _composer_factory(reg, payload_fn=payload)
    )

    messages = [e.message for e in bus.published]
    assert any("entities.0.name" in m for m in messages), messages
    assert "entities.0.name" in reg.calls[1].prompt


def test_compose_diagnostics_are_written_to_the_project_log(tmp_path):
    """This stage used to write nothing to disk, so a failed compose left
    no trace of what the model actually said."""
    store, pid, settings = _seed(tmp_path, n_scenes=1)
    ctx, _bus = _ctx_with_bus(store, pid, settings)

    seen: list[str] = []

    def factory(_settings, _session_id, _device, log):
        log("hello from the composer")
        seen.append("built")
        return _FakeComposer(registry=_Registry())

    stage_scene_spec.run_with_composer_factory(ctx, factory)

    log_path = store.dir_for(pid) / "logs" / "scene_spec.log"
    assert log_path.exists(), "scene_spec.log was never created"
    assert "hello from the composer" in log_path.read_text(encoding="utf-8")
    assert seen == ["built"]


def test_repair_budget_exhausted_fails_stage(tmp_path):
    """If the scene never validates, the stage exhausts its repair budget,
    raises, and is not stamped succeeded."""
    store, pid, settings = _seed(tmp_path, n_scenes=1)
    settings.scene_spec_max_repair_rounds = 1
    ctx, bus = _ctx_with_bus(store, pid, settings)

    def payload(scene_id: str, attempt: int) -> SceneSpec:
        # Always invalid (gauntlet asset never present).
        return _make_scene_missing_asset(scene_id)

    reg = _Registry()
    with pytest.raises(Exception, match="semantic validation"):
        stage_scene_spec.run_with_composer_factory(
            ctx, _composer_factory(reg, payload_fn=payload)
        )

    # max_repair_rounds=1 -> initial compose + 1 repair = 2 attempts.
    assert len(reg.calls) == 2
    statuses = [e.status for e in bus.published]
    assert "succeeded" not in statuses


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
    reg = _Registry()

    # First run: composes all 4 scenes via the agent and writes them to disk.
    stage_scene_spec.run_with_composer_factory(ctx, _composer_factory(reg))
    assert len(reg.calls) == 4
    proj_dir = store.dir_for(pid)
    for i in range(4):
        assert (proj_dir / f"scene_spec.scene_{i:02d}.json").exists()

    # Second run on a fresh context: every scene should be reused from
    # disk; the fake composer must NOT be called again.
    ctx2, bus2 = _ctx_with_bus(store, pid, settings)
    reg2 = _Registry()
    result = stage_scene_spec.run_with_composer_factory(
        ctx2, _composer_factory(reg2)
    )
    assert len(reg2.calls) == 0, (
        "second run made agent calls; idempotent resume is broken. "
        f"calls={[c.scene_id for c in reg2.calls]}"
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
    reg = _Registry()

    # Compose everything once, then nuke 2 of the per-scene files.
    stage_scene_spec.run_with_composer_factory(ctx, _composer_factory(reg))
    proj_dir = store.dir_for(pid)
    (proj_dir / "scene_spec.scene_01.json").unlink()
    (proj_dir / "scene_spec.scene_03.json").unlink()

    # Fresh registry + ctx: only scenes 01 and 03 should fire an agent call.
    ctx2, _ = _ctx_with_bus(store, pid, settings)
    reg2 = _Registry()
    stage_scene_spec.run_with_composer_factory(ctx2, _composer_factory(reg2))
    assert len(reg2.calls) == 2
    scene_ids_called = {c.scene_id for c in reg2.calls}
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
    reg = _Registry()
    stage_scene_spec.run_with_composer_factory(ctx, _composer_factory(reg))
    assert len(reg.calls) == 3

    proj_dir = store.dir_for(pid)
    # Corrupt scene_01's file. Invalid JSON triggers the "missing" path.
    (proj_dir / "scene_spec.scene_01.json").write_text(
        "{ not valid json at all", encoding="utf-8"
    )

    ctx2, _ = _ctx_with_bus(store, pid, settings)
    reg2 = _Registry()
    stage_scene_spec.run_with_composer_factory(ctx2, _composer_factory(reg2))
    # Exactly one re-composed scene (the corrupted one).
    assert len(reg2.calls) == 1
    assert reg2.calls[0].scene_id == "scene_01"
    # And the file is now valid JSON again.
    spec = store.load_scene_spec(pid, filename="scene_spec.scene_01.json")
    assert spec.scene_id == "scene_01"


def test_resume_regenerates_file_with_mismatched_scene_id(tmp_path):
    """A scene_spec_X.json that holds a scene with a different id (e.g.
    stale after a rename) is considered invalid and re-generated."""
    store, pid, settings = _seed(tmp_path, n_scenes=2)
    ctx, _ = _ctx_with_bus(store, pid, settings)
    reg = _Registry()
    stage_scene_spec.run_with_composer_factory(ctx, _composer_factory(reg))

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
    reg2 = _Registry()
    stage_scene_spec.run_with_composer_factory(ctx2, _composer_factory(reg2))
    # Exactly the mismatched scene is re-composed.
    assert len(reg2.calls) == 1
    assert reg2.calls[0].scene_id == "scene_00"


def test_fast_finish_records_succeeded_without_llm_call(tmp_path):
    """The "all on disk" fast path must stamp ProjectStatus.succeeded
    on the state file. Without that stamp the next Resume would re-run
    the stage entirely -- which is the exact bug we're fixing."""
    from ptv_schemas import ProjectStage as _ProjectStage, ProjectStatus as _PS

    store, pid, settings = _seed(tmp_path, n_scenes=3)
    ctx, _ = _ctx_with_bus(store, pid, settings)
    reg = _Registry()
    stage_scene_spec.run_with_composer_factory(ctx, _composer_factory(reg))

    ctx2, _ = _ctx_with_bus(store, pid, settings)
    reg2 = _Registry()
    stage_scene_spec.run_with_composer_factory(ctx2, _composer_factory(reg2))
    assert len(reg2.calls) == 0

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
    reg = _Registry()

    # Reach into the module to lower the heartbeat interval for the test.
    real_heartbeat_cls = stage_scene_spec._Heartbeat

    class _FastHeartbeat(real_heartbeat_cls):
        def __init__(self, *, ctx, total, interval_s: float = 1.0):
            super().__init__(ctx=ctx, total=total, interval_s=interval_s)

    import unittest.mock as _mock

    with _mock.patch.object(stage_scene_spec, "_Heartbeat", _FastHeartbeat):
        stage_scene_spec.run_with_composer_factory(
            ctx, _composer_factory(reg, delay_seconds=2.2)
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
    reg = _Registry()  # no delay

    stage_scene_spec.run_with_composer_factory(ctx, _composer_factory(reg))

    info_msgs = [e.message for e in bus.published if e.status == "info"]
    assert not any("still composing" in m for m in info_msgs), info_msgs
