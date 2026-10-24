"""Stage 4: BuildSpec + AssetManifest + MechanicPlan -> SceneSpec per scene.

Composes one SceneSpec per ``BuildSpec.scenes`` entry in **parallel**.

Why this stage used to appear hung
==================================

Two distinct bugs caused this stage to look hung on Resume:

A. **No prompt compression / no parallelism.** A long user prompt
   (e.g. the 945-line ``GREEN LANTERN: WILLPOWER VR`` brief) can
   produce a BuildSpec with 15 scenes, an AssetManifest with 48
   assets, and a MechanicPlan with 44 items (many ``generate``
   classifications carrying 10K-character ``code_task.description``
   strings). The original implementation looped over scenes serially
   AND dumped the full BuildSpec + AssetManifest + MechanicPlan
   (~155 KB ≈ 39K input tokens) as the user message of every call.
   For a ``gpt-5``-class reasoning model that meant 15 sequential
   calls of ~60-90 seconds each (15-22 minutes wall-clock) with no
   progress events between them.

B. **No idempotent resume.** Even after fixing (A), a long pipeline
   run that finished writing every ``scene_spec.<scene_id>.json``
   file but was killed before the stage could stamp
   ``ProjectStatus.succeeded`` on ``project_state.json`` would force
   every subsequent Resume to re-run all 15 LLM calls from scratch.
   The user saw "running" with nothing happening because the work
   had already been done -- the orchestrator just didn't know.
   Concretely: the Green Lantern project produced all 13 scene_spec
   files on 2026-06-03 within a 15-minute window, but the process
   was killed at the end before ``record_stage(..., succeeded)`` ran,
   so the stage stayed at status='running' forever.

What this module does
=====================

1. **Idempotent resume** (the new fix). Before any LLM call, check
   the project directory for a pre-existing ``scene_spec.<id>.json``
   that validates against the :class:`SceneSpec` schema. If valid,
   reuse it and skip the LLM call. If every scene is already on
   disk, the stage stamps ``succeeded`` and returns in well under a
   second. Force a re-generation by deleting the file from disk
   (or the entire scene_spec.*.json set for a full re-run).

2. **Compressed per-scene prompts.** The THIS-scene record stays
   full, but assets / mechanics / other scenes are summarised down
   to the fields the SceneSpec composer actually needs (id, name,
   kind, collision shape, target scale, GLB path, ~400-char style
   notes). Drops the per-call input from ~39K tokens to ~5-8K.

3. **Parallel scene calls.** Scenes run through a
   ``ThreadPoolExecutor`` capped by
   ``Settings.scene_spec_max_parallel_workers`` (default 3).
   Wall-clock drops from O(n) to O(n / workers).

4. **Granular progress events.** Per-scene ``running`` / ``info`` /
   ``succeeded`` events so the UI shows real progress
   (``"scene 3/15 composed: training_arena"``) plus a periodic
   heartbeat for any LLM call that runs longer than 30s so the user
   never wonders whether a worker is alive.

5. **Pre-computed canonical glb_path.** Each asset's
   ``assets/<asset_id>.glb`` path is computed once so the LLM cannot
   invent a wrong path that would later fail GLB normalization.

Each scene is composed by an OpenAI Agents SDK agent (``gpt-5.6-sol``, NO
tools) via :class:`SceneSpecComposer`, mirroring the mechanic_plan and
world stages. After each compose the stage runs a deterministic
:func:`_validate_scene_semantics` check (asset coverage, environment
presence, valid mechanic-binding ids, scene_id match); any problems are
fed back into the SAME agent session as a repair message so the agent
fixes its output in context, up to ``scene_spec_max_repair_rounds``.
This replaces the old single ``make_llm`` JSON call, whose gpt-5
fallback could exceed the request timeout and then be silently retried
by the OpenAI SDK, producing a ~15-minute apparent hang.
"""

from __future__ import annotations

import json
import os
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, Mapping, Optional

from ptv_schemas import (
    AssetManifest,
    BuildSpec,
    MechanicPlan,
    ProjectStage,
    ProjectStatus,
    SceneSpec,
    TargetDevice,
)

from ..settings import Settings
from .context import PipelineContext, StageResult
from .prompts import scene_spec_system
from .scene_spec_agent import (
    SceneSpecAgentError,
    SceneSpecComposer,
    SceneSpecOutputError,
    build_repair_message,
    build_truncation_message,
)


# ---------------------------------------------------------------------------
# Public entry point used by the orchestrator
# ---------------------------------------------------------------------------


# A ``ComposerFactory`` takes the orchestrator ``Settings``, a unique
# per-scene ``session_id`` and the project's target device, and returns an
# object exposing ``.compose(message) -> SceneSpec`` (a
# :class:`SceneSpecComposer`). ``_default_composer_factory`` is the
# production wiring; tests inject a fake. We accept a *factory* (not a
# pre-built composer) because each scene needs its OWN agent session, and
# some tests want to count constructions and be sure no agent is built at
# import-time.
ComposerFactory = Callable[[Settings, str, TargetDevice, Callable[[str], None]], Any]


def _scene_log(ctx: PipelineContext, scene_id: str) -> Callable[[str], None]:
    """Per-scene sink for ``logs/scene_spec.log``.

    This stage used to write nothing to disk, so when a compose failed the
    only trace left anywhere was one redacted sentence in
    ``project_state.json`` -- the raw model output was gone. The world
    stage has always logged; now this one does too.
    """

    def _log(text: str) -> None:
        ctx.store.append_log(ctx.project_id, "scene_spec", f"[{scene_id}] {text}")

    return _log


def _default_composer_factory(
    settings: Settings,
    session_id: str,
    target_device: TargetDevice,
    log: Optional[Callable[[str], None]] = None,
) -> SceneSpecComposer:
    """Build a real Agents-SDK scene composer from settings."""
    return SceneSpecComposer(
        instructions=scene_spec_system(
            target_device, max_entities=int(settings.scene_spec_max_entities)
        ),
        model=settings.scene_spec_model,
        reasoning_effort=settings.scene_spec_reasoning_effort,
        verbosity=settings.scene_spec_verbosity,
        max_output_tokens=int(settings.scene_spec_max_output_tokens),
        max_turns=settings.scene_spec_agent_max_turns,
        session_id=session_id,
        log=log,
    )


def _remove_stale_scene_specs(ctx: PipelineContext, build_spec: Any) -> None:
    """Delete SceneSpecs of scenes the BuildSpec no longer has; later stages
    glob ``scene_spec*.json`` and would read them as part of this game."""
    from .run_reset import remove_stale_scene_files

    removed = remove_stale_scene_files(
        ctx.project_dir, "scene_spec", [s.id for s in build_spec.scenes])
    if removed:
        ctx.emit(ProjectStage.scene_spec, "info",
                 f"removed SceneSpecs of scenes no longer planned: {', '.join(removed)}")


def run(ctx: PipelineContext) -> StageResult:
    return run_with_composer_factory(ctx, _default_composer_factory)


def run_with_composer_factory(
    ctx: PipelineContext, composer_factory: ComposerFactory
) -> StageResult:
    """Compose one SceneSpec per BuildSpec scene, in parallel.

    Each scene is composed by its own OpenAI Agents SDK agent
    (``gpt-5.6-sol``, no tools) with a validate-and-repair loop.
    ``composer_factory`` lets tests inject a fake composer without
    touching the SDK or the network.

    On Resume / partial completion, scenes whose ``scene_spec.<id>.json``
    already exists and validates against the :class:`SceneSpec` schema
    are reused without an LLM call. Delete the file (or the entire
    ``scene_spec.*.json`` set) to force re-generation.
    """

    ctx.emit(ProjectStage.scene_spec, "running", "Composing SceneSpec(s)")
    state = ctx.store.load_state(ctx.project_id)
    ctx.store.record_stage(state, ProjectStage.scene_spec, ProjectStatus.running)

    build_spec: BuildSpec = ctx.build_spec or ctx.store.load_build_spec(
        ctx.project_id
    )

    total = len(build_spec.scenes)

    # ------------------------------------------------------------------
    # Fast path: every scene already on disk -> mark succeeded immediately.
    # This is the fix for the "Resume just sits at running" symptom: a
    # previous run wrote every scene_spec.<id>.json but the orchestrator
    # was killed before the stage could stamp ProjectStatus.succeeded.
    # Without this branch every Resume re-runs N expensive LLM calls
    # from scratch, which can take 15-25 minutes on a large BuildSpec.
    # ------------------------------------------------------------------
    project_dir = ctx.store.dir_for(ctx.project_id)
    manifest: AssetManifest = (
        ctx.asset_manifest or ctx.store.load_asset_manifest(ctx.project_id)
    )
    # Reuse is only safe against the CURRENT manifest. A scene composed for an
    # earlier, differently-named asset set validates against the schema
    # perfectly and is still worthless: its meshes point at GLBs that were
    # never generated, and the assets that DO exist end up referenced by
    # nothing. That is exactly what happened to the shooting-range project,
    # where a reused 11-asset scene named eight assets the 14-asset manifest
    # had never heard of.
    known_asset_ids = {a.id for a in manifest.items}
    # The same goes for mechanics: a re-planned game that turned a generated
    # teleport copy back into the built-in must not keep a scene that binds
    # the copy.
    plan_for_reuse: MechanicPlan = (
        ctx.mechanic_plan or ctx.store.load_mechanic_plan(ctx.project_id)
    )
    known_generated_ids = {
        f"generated.{it.code_task.id if it.code_task else it.id}"
        for it in plan_for_reuse.items
        if it.classification.value in ("generate", "builtin+config_code_edit")
    }

    existing: dict[int, SceneSpec] = {}
    for i, scene_entry in enumerate(build_spec.scenes):
        loaded, rejected = _try_load_existing_scene_spec(
            project_dir, scene_entry.id, known_asset_ids, known_generated_ids
        )
        if loaded is not None:
            existing[i] = loaded
        elif rejected is not None:
            ctx.emit(
                ProjectStage.scene_spec,
                "info",
                f"{scene_entry.id}: recomposing, {rejected}",
            )
    if len(existing) == total:
        return _finalize_all_existing(ctx, state, build_spec, existing)

    if existing:
        ctx.emit(
            ProjectStage.scene_spec,
            "info",
            f"reusing {len(existing)}/{total} scene(s) already on disk; "
            f"composing {total - len(existing)} via LLM",
        )

    # ------------------------------------------------------------------
    # Slow path: at least one scene needs to be composed via LLM.
    # ------------------------------------------------------------------
    plan: MechanicPlan = (
        ctx.mechanic_plan or ctx.store.load_mechanic_plan(ctx.project_id)
    )

    # Bridge OPENAI_API_KEY into os.environ for the Agents SDK (BaseSettings
    # does not export it automatically), mirroring the mechanic_plan / world
    # stages.
    if ctx.settings.openai_api_key and not os.environ.get("OPENAI_API_KEY"):
        os.environ["OPENAI_API_KEY"] = ctx.settings.openai_api_key

    npc_plan = ctx.npc_plan or ctx.store.load_npc_plan(ctx.project_id)
    static = _StaticContext.build(build_spec, manifest, plan, npc_plan)
    max_repair_rounds = int(ctx.settings.scene_spec_max_repair_rounds)

    max_workers = max(
        1, min(int(ctx.settings.scene_spec_max_parallel_workers), total)
    )
    ctx.emit(
        ProjectStage.scene_spec,
        "info",
        f"composing {total} scene(s) with {max_workers} parallel worker(s)",
    )

    results: dict[int, _SceneJob] = {}
    save_lock = threading.Lock()
    heartbeat = _Heartbeat(ctx=ctx, total=total)

    def _do_one(index: int, scene_entry: Any) -> _SceneJob:
        ctx.raise_if_cancelled()
        scene_id = scene_entry.id

        # ----------------------------------------------------------
        # Per-scene idempotency: if this scene was already composed
        # by a prior run (and survived validation), reuse it.
        # ----------------------------------------------------------
        if index in existing:
            scene_spec = existing[index]
            with save_lock:
                artifacts_local = _ensure_entrypoint_canonical(
                    ctx, scene_entry, scene_spec, total
                )
            ctx.emit(
                ProjectStage.scene_spec,
                "info",
                f"[{index + 1}/{total}] reused scene '{scene_id}' "
                f"({len(scene_spec.entities)} entities, no LLM call)",
            )
            return _SceneJob(
                index=index,
                scene_spec=scene_spec,
                artifacts=artifacts_local,
            )

        ctx.emit(
            ProjectStage.scene_spec,
            "info",
            f"[{index + 1}/{total}] starting scene '{scene_id}'",
        )
        heartbeat.start(scene_id, index)
        try:
            composer = composer_factory(
                ctx.settings,
                f"ptv-scene-{ctx.project_id}-{scene_id}",
                state.target_device,
                _scene_log(ctx, scene_id),
            )
            scene_spec = _compose_one_scene(
                ctx=ctx,
                composer=composer,
                scene_entry=scene_entry,
                static=static,
                max_repair_rounds=max_repair_rounds,
                index=index,
                total=total,
            )
        except Exception as e:
            heartbeat.stop(scene_id)
            ctx.emit(
                ProjectStage.scene_spec,
                "info",
                f"[{index + 1}/{total}] FAILED scene '{scene_id}': {e}",
            )
            raise
        heartbeat.stop(scene_id)

        # Persist immediately so partial progress survives a worker crash.
        with save_lock:
            artifacts_local = []
            filename = f"scene_spec.{scene_id}.json"
            art = ctx.store.save_scene_spec(
                ctx.project_id, scene_spec, filename=filename
            )
            artifacts_local.append(art.path)
            artifacts_local.extend(
                _ensure_entrypoint_canonical(ctx, scene_entry, scene_spec, total)
            )

        ctx.emit(
            ProjectStage.scene_spec,
            "info",
            f"[{index + 1}/{total}] done scene '{scene_id}'"
            f" ({len(scene_spec.entities)} entities)",
        )
        return _SceneJob(
            index=index,
            scene_spec=scene_spec,
            artifacts=artifacts_local,
        )

    try:
        with ThreadPoolExecutor(max_workers=max_workers) as pool:
            futures = [
                pool.submit(_do_one, i, scene)
                for i, scene in enumerate(build_spec.scenes)
            ]
            # ``.result()`` reraises any worker exception; the pool's
            # context manager waits for outstanding futures to finish.
            for fut in futures:
                job = fut.result()
                results[job.index] = job
    finally:
        heartbeat.shutdown()

    # Preserve BuildSpec scene order in ctx.scene_specs (and artifacts).
    artifacts: list[str] = []
    scenes: list[SceneSpec] = []
    for i in range(total):
        job = results[i]
        scenes.append(job.scene_spec)
        artifacts.extend(job.artifacts)

    _remove_stale_scene_specs(ctx, build_spec)
    ctx.scene_specs = scenes
    ctx.store.record_stage(
        state,
        ProjectStage.scene_spec,
        ProjectStatus.succeeded,
        message=f"Composed {len(scenes)} scenes",
        artifacts=artifacts,
    )
    ctx.emit(ProjectStage.scene_spec, "succeeded", f"{len(scenes)} scenes")
    return StageResult(stage=ProjectStage.scene_spec, artifacts=artifacts, data=scenes)


# ---------------------------------------------------------------------------
# Idempotency helpers
# ---------------------------------------------------------------------------


def _try_load_existing_scene_spec(
    project_dir: Path,
    scene_id: str,
    known_asset_ids: set[str] | None = None,
    known_generated_ids: set[str] | None = None,
) -> tuple[Optional[SceneSpec], Optional[str]]:
    """``(scene, rejection_reason)`` for ``scene_spec.<scene_id>.json``.

    The scene is ``None`` if the file is missing, malformed, fails Pydantic
    validation, or belongs to a different asset set -- in which case the
    caller should re-generate that ONE scene via the LLM. We never raise
    here: a half-written file from a crashed prior run is recoverable.

    ``rejection_reason`` is set only when a file was actually present and
    rejected, so the caller can say why it is spending an LLM call. A missing
    file needs no explanation.

    Schema validity is NOT enough to reuse a scene. Asset ids change whenever
    the manifest is re-planned, and a scene composed against the old set is
    still perfectly valid JSON -- it just describes a different game. Passing
    ``known_asset_ids`` is what turns that into a re-compose instead of a
    build full of dangling GLB paths.
    """

    path = project_dir / f"scene_spec.{scene_id}.json"
    if not path.exists():
        return None, None
    try:
        raw = path.read_text(encoding="utf-8")
        data = json.loads(raw)
        scene = SceneSpec.model_validate(data)
    except Exception:
        return None, "the file on disk is malformed or fails validation"
    # Sanity check: the embedded scene_id must match. A stale file from
    # before a scene id rename should be regenerated.
    if scene.scene_id != scene_id:
        return None, f"it declares scene_id '{scene.scene_id}'"
    if known_asset_ids is not None:
        referenced = {
            e.mesh.asset_id for e in scene.entities if e.mesh is not None
        }
        unknown = sorted(referenced - known_asset_ids)
        if unknown:
            return None, (
                f"it references {len(unknown)} asset(s) absent from the"
                f" current AssetManifest: {unknown}"
            )
    if known_generated_ids is not None:
        stale = sorted({
            b.mechanic_id
            for e in scene.entities
            for b in e.mechanic_bindings
            if b.mechanic_id.startswith("generated.")
            and b.mechanic_id not in known_generated_ids
        })
        if stale:
            return None, (
                f"it binds {len(stale)} generated mechanic(s) the current"
                f" MechanicPlan no longer generates: {stale}"
            )
    return scene, None


def _ensure_entrypoint_canonical(
    ctx: PipelineContext,
    scene_entry: Any,
    scene_spec: SceneSpec,
    total: int,
) -> list[str]:
    """Write ``scene_spec.json`` (the canonical entry-point file) when
    this scene is the entry-point (or the only scene). Returns the list
    of artifact paths written (possibly empty)."""

    if not (scene_entry.entry_point or total == 1):
        return []
    art = ctx.store.save_scene_spec(
        ctx.project_id, scene_spec, filename="scene_spec.json"
    )
    return [art.path]


def _finalize_all_existing(
    ctx: PipelineContext,
    state: Any,
    build_spec: BuildSpec,
    existing: Mapping[int, SceneSpec],
) -> StageResult:
    """Fast-finish the stage when every scene already exists on disk.

    Stamps ``ProjectStatus.succeeded`` and emits per-scene reuse events
    so the UI shows what happened. Also re-stamps the canonical
    ``scene_spec.json`` (entry-point) in case a prior run died before
    that file was synced.
    """

    total = len(build_spec.scenes)
    ctx.emit(
        ProjectStage.scene_spec,
        "info",
        f"all {total} scene(s) already on disk; reusing without LLM calls",
    )

    artifacts: list[str] = []
    scenes: list[SceneSpec] = []
    for i, scene_entry in enumerate(build_spec.scenes):
        scene_spec = existing[i]
        scenes.append(scene_spec)
        # Per-scene file is already on disk; report its canonical
        # project-relative path so the UI's artifact list stays
        # consistent with the "freshly composed" code path.
        rel = (
            ctx.store.dir_for(ctx.project_id)
            / f"scene_spec.{scene_entry.id}.json"
        )
        try:
            artifacts.append(
                str(rel.relative_to(ctx.settings.repo_root)).replace("\\", "/")
            )
        except ValueError:
            # repo_root not on the same drive as project_dir (unusual);
            # fall back to absolute path so the UI still has something
            # to display.
            artifacts.append(str(rel).replace("\\", "/"))
        artifacts.extend(
            _ensure_entrypoint_canonical(ctx, scene_entry, scene_spec, total)
        )
        ctx.emit(
            ProjectStage.scene_spec,
            "info",
            f"[{i + 1}/{total}] reused scene '{scene_entry.id}' "
            f"({len(scene_spec.entities)} entities, no LLM call)",
        )

    _remove_stale_scene_specs(ctx, build_spec)
    ctx.scene_specs = scenes
    ctx.store.record_stage(
        state,
        ProjectStage.scene_spec,
        ProjectStatus.succeeded,
        message=f"Reused {len(scenes)} scenes already on disk",
        artifacts=artifacts,
    )
    ctx.emit(
        ProjectStage.scene_spec,
        "succeeded",
        f"{len(scenes)} scenes (reused from disk)",
    )
    return StageResult(
        stage=ProjectStage.scene_spec, artifacts=artifacts, data=scenes
    )


# ---------------------------------------------------------------------------
# Heartbeat -- periodic "still composing" emits for in-flight LLM calls
# ---------------------------------------------------------------------------


class _Heartbeat:
    """Periodically emits an ``info`` event for any scene that has been
    composing for longer than ``interval_s`` (default 30s).

    Without this, a slow scene (e.g. a 90-second gpt-5 reasoning call)
    looks identical to a hung worker in the UI: a "starting scene X"
    event followed by silence. The heartbeat fires
    ``[i/N] still composing 'X' (Ys elapsed)`` every interval so the
    user can see the worker is alive.

    Cheap to run -- a single daemon thread that sleeps when nothing is
    in flight. Safe to call from any number of worker threads
    concurrently; lock-guarded.
    """

    def __init__(
        self, *, ctx: PipelineContext, total: int, interval_s: float = 30.0
    ) -> None:
        self._ctx = ctx
        self._total = total
        self._interval_s = interval_s
        self._lock = threading.Lock()
        self._in_flight: dict[str, tuple[int, float]] = {}
        self._stop = threading.Event()
        self._thread = threading.Thread(
            target=self._run, name="scene_spec-heartbeat", daemon=True
        )
        self._thread.start()

    def start(self, scene_id: str, index: int) -> None:
        with self._lock:
            self._in_flight[scene_id] = (index, time.monotonic())

    def stop(self, scene_id: str) -> None:
        with self._lock:
            self._in_flight.pop(scene_id, None)

    def shutdown(self) -> None:
        self._stop.set()
        # Daemon thread; do not block on join in case the orchestrator
        # was killed mid-sleep. ``join`` with a short timeout gives a
        # clean shutdown when possible.
        self._thread.join(timeout=1.0)

    def _run(self) -> None:
        # Re-check every second so shutdown is responsive, but only
        # emit at ``interval_s`` cadence per in-flight scene.
        last_emit: dict[str, float] = {}
        while not self._stop.is_set():
            self._stop.wait(timeout=1.0)
            if self._stop.is_set():
                return
            now = time.monotonic()
            with self._lock:
                snapshot = dict(self._in_flight)
            for scene_id, (index, started) in snapshot.items():
                elapsed = now - started
                if elapsed < self._interval_s:
                    continue
                if now - last_emit.get(scene_id, 0.0) < self._interval_s:
                    continue
                last_emit[scene_id] = now
                self._ctx.emit(
                    ProjectStage.scene_spec,
                    "info",
                    f"[{index + 1}/{self._total}] still composing "
                    f"'{scene_id}' ({elapsed:.0f}s elapsed)",
                )


# ---------------------------------------------------------------------------
# Per-scene LLM call
# ---------------------------------------------------------------------------


def _compose_one_scene(
    *,
    ctx: PipelineContext,
    composer: Any,
    scene_entry: Any,
    static: "_StaticContext",
    max_repair_rounds: int,
    index: int,
    total: int,
) -> SceneSpec:
    """Compose one scene via the agent, then run deterministic semantic
    validation and feed any problems back to the SAME agent session as a
    repair message, up to ``max_repair_rounds`` times.

    Raises :class:`SceneSpecAgentError` if the scene still fails schema or
    semantic validation after the repair budget is exhausted (the worker
    stamps the failure and the stage propagates it).
    """

    message = _build_user_message(scene_entry, static)
    issues: list[str] = []
    repair_message: Optional[str] = None

    for attempt in range(max_repair_rounds + 1):
        ctx.raise_if_cancelled()
        if attempt > 0:
            ctx.emit(
                ProjectStage.scene_spec,
                "info",
                f"[{index + 1}/{total}] repairing scene '{scene_entry.id}' "
                f"(round {attempt}/{max_repair_rounds}): {len(issues)} issue(s)",
            )
        msg = message if attempt == 0 else (repair_message or build_repair_message(issues))

        try:
            scene = composer.compose(msg)
        except SceneSpecOutputError as e:
            # The model produced something, it just did not validate. That
            # IS repairable in-context -- but only if the next turn is told
            # what was actually wrong, which is why the error carries
            # field-level issues instead of a redacted one-liner.
            if e.truncated:
                issues = [str(e)]
                repair_message = build_truncation_message(
                    int(ctx.settings.scene_spec_max_entities)
                )
            else:
                issues = e.issues or [str(e)]
                repair_message = build_repair_message(issues)
            for issue in issues:
                ctx.emit(
                    ProjectStage.scene_spec,
                    "info",
                    f"[{index + 1}/{total}] '{scene_entry.id}' output problem: "
                    f"{issue[:200]}",
                )
            if attempt >= max_repair_rounds:
                raise
            continue
        except SceneSpecAgentError as e:
            # Transport / turn-budget / API failure: there is no model output
            # to repair, so re-sending the original brief is the best we can
            # do. Asking it to "fix" a network error would be nonsense.
            issues = [f"agent run failed: {e}"]
            repair_message = message
            if attempt >= max_repair_rounds:
                raise
            continue

        issues = _validate_scene_semantics(scene, scene_entry, static)
        if not issues:
            return scene
        repair_message = build_repair_message(issues)
        if attempt >= max_repair_rounds:
            raise SceneSpecAgentError(
                f"scene '{scene_entry.id}' failed semantic validation after "
                f"{max_repair_rounds} repair round(s): " + "; ".join(issues)
            )

    # Unreachable (the loop either returns or raises), but keeps the type
    # checker satisfied.
    raise SceneSpecAgentError(f"scene '{scene_entry.id}' could not be composed")


def _validate_scene_semantics(
    scene: SceneSpec, scene_entry: Any, static: "_StaticContext"
) -> list[str]:
    """Deterministic post-checks the schema alone cannot enforce.

    Returns a list of human-readable problems (empty == valid) that are
    fed back to the agent as a repair message.
    """

    issues: list[str] = []

    if scene.scene_id != scene_entry.id:
        issues.append(
            f"scene_id mismatch: got '{scene.scene_id}', "
            f"expected '{scene_entry.id}'"
        )

    if not scene.entities:
        issues.append(
            "entities[] is empty; a real scene needs at least the player "
            "rig and one entity per AssetItem"
        )

    # Asset coverage: every AssetItem must appear at least once as a mesh.
    mesh_asset_ids = {
        e.mesh.asset_id for e in scene.entities if e.mesh is not None
    }
    missing = [
        a["id"] for a in static.assets_compact if a["id"] not in mesh_asset_ids
    ]
    if missing:
        shown = ", ".join(missing[:20]) + (" ..." if len(missing) > 20 else "")
        issues.append(
            "these AssetItem ids never appear in entities[] with a mesh "
            f"(each must appear at least once): {shown}"
        )

    # There is deliberately no "the scene must contain an environment"
    # check any more: the environment is authored in C++ by the world
    # architect and merged in by the scene compiler, so requiring the
    # composer to invent one was demanding an entity it should not own.

    # Mechanic-binding validity: every mechanic_id must be a known binding
    # id from the plan (builtin.* or generated.<task_id>).
    known_binding_ids = {
        m["binding_id"]
        for m in static.mechanics_compact
        if m.get("binding_id")
    }
    unknown: list[str] = []
    for e in scene.entities:
        for b in e.mechanic_bindings:
            mid = b.mechanic_id
            if mid in known_binding_ids or mid.startswith("builtin."):
                continue
            unknown.append(f"{e.id}:{mid}")
    if unknown:
        shown = ", ".join(unknown[:20]) + (" ..." if len(unknown) > 20 else "")
        issues.append(
            "mechanic_bindings reference unknown mechanic ids (use a "
            "binding_id from the MECHANICS block or a 'builtin.*' id): "
            f"{shown}"
        )

    return issues


def _build_user_message(scene_entry: Any, static: "_StaticContext") -> str:
    """Assemble a focused user message for a single scene.

    The composer needs:

      * the FULL ``scene_entry`` (the 4000-char authoring summary),
      * ids/names of the other scenes (for navigation tags),
      * COMPRESSED asset list (id, name, category, scale, collision,
        glb path, ~400-char style notes),
      * COMPRESSED mechanic list (binding id ready to drop into
        ``MechanicBinding.mechanic_id``, plus config and a short brief),
      * top-level BuildSpec context (player height, locomotion, comfort,
        constraints).

    Everything else (long mechanic summaries, 10K-char code-task
    descriptions, image-prompt blocks, Tripo settings, acceptance test
    list) is dropped -- the SceneSpec composer does not use it and
    paying for those input tokens 15x is what made this stage appear
    to hang.
    """

    target = {
        "id": scene_entry.id,
        "name": scene_entry.name,
        "summary": scene_entry.summary,
        "entry_point": scene_entry.entry_point,
        "expected_playtime_minutes": scene_entry.expected_playtime_minutes,
    }

    other_scenes = [
        {"id": s["id"], "name": s["name"]}
        for s in static.scenes_min
        if s["id"] != scene_entry.id
    ]

    return (
        f"Compose the SceneSpec for scene_id={scene_entry.id!r}, "
        f"name={scene_entry.name!r}.\n\n"
        "Use ONLY ids that appear in the ASSETS and MECHANICS blocks "
        "below. Pre-computed canonical glb_path values are provided -- "
        "do NOT invent your own.\n\n"
        "BUILD_SPEC_META (top-level context):\n"
        + _pretty(static.build_spec_meta)
        + "\n\nTHIS_SCENE (compose for this entry):\n"
        + _pretty(target)
        + "\n\nOTHER_SCENES (id+name only, for nav tags):\n"
        + _pretty(other_scenes)
        + "\n\nASSETS (every AssetItem must appear at least once in "
          "entities[]):\n"
        + _pretty(static.assets_compact)
        + "\n\nMECHANICS (use 'binding_id' as the MechanicBinding "
          "mechanic_id, forward 'config' into params):\n"
        + _pretty(static.mechanics_compact)
        + (
            "\n\nNPCS (already decided; place them accordingly):\n"
            + _pretty(static.npcs_compact)
            + "\n"
            "For each NPC asset you place:\n"
            "  * set 'health' from its max_health and 'faction' from its "
            "faction + hostile_to, or nothing can damage it and no one will "
            "target it;\n"
            "  * bind 'builtin.character_locomotion' with params from its "
            "locomotion block, and 'animated': false when movement_style is "
            "slide_no_animation;\n"
            "  * use physics.kind 'kinematic' and collision_shape 'capsule';\n"
            "  * when spawn.waves is true or initial_count exceeds what "
            "should exist at load, place ONE hidden copy tagged 'template' "
            "at a far-off position and let a spawner mechanic clone it, "
            "instead of placing every instance up front.\n"
            if static.npcs_compact
            else ""
        )
        + "\n\nOutput obeys the supplied JSON Schema exactly. No "
          "prose, no markdown."
    )


def _pretty(obj: Any) -> str:
    return json.dumps(obj, indent=2, ensure_ascii=False, sort_keys=False)


# ---------------------------------------------------------------------------
# Static per-run context (shared across all scenes)
# ---------------------------------------------------------------------------


@dataclass
class _StaticContext:
    build_spec_meta: dict[str, Any]
    scenes_min: list[dict[str, Any]]
    assets_compact: list[dict[str, Any]]
    mechanics_compact: list[dict[str, Any]]
    npcs_compact: list[dict[str, Any]] = field(default_factory=list)

    @classmethod
    def build(
        cls,
        build_spec: BuildSpec,
        manifest: AssetManifest,
        plan: MechanicPlan,
        npc_plan: Any = None,
    ) -> "_StaticContext":
        return cls(
            build_spec_meta=_compress_build_spec_meta(build_spec),
            scenes_min=[
                {"id": s.id, "name": s.name} for s in build_spec.scenes
            ],
            assets_compact=[_compress_asset(a) for a in manifest.items],
            mechanics_compact=_compress_mechanics(build_spec, plan),
            npcs_compact=_compress_npcs(npc_plan),
        )


def _compress_npcs(npc_plan: Any) -> list[dict[str, Any]]:
    """Just the fields that change how an NPC is PLACED.

    Behaviours and perception matter to the codegen agent, not to scene
    composition, and paying for those tokens once per scene is what made this
    stage expensive before.
    """
    if npc_plan is None or not getattr(npc_plan, "npcs", None):
        return []
    out: list[dict[str, Any]] = []
    for npc in npc_plan.npcs:
        out.append(
            {
                "asset_id": npc.asset_id,
                "role": npc.role.value,
                "faction": npc.faction,
                "hostile_to": list(npc.hostile_to),
                "movement_style": npc.movement_style.value,
                "max_health": npc.combat.max_health,
                "walk_speed_mps": npc.locomotion.walk_speed_mps,
                "run_speed_mps": npc.locomotion.run_speed_mps,
                "turn_rate_deg_s": npc.locomotion.turn_rate_deg_s,
                "initial_count": npc.spawn.initial_count,
                "max_concurrent": npc.spawn.max_concurrent,
                "waves": npc.spawn.waves,
            }
        )
    return out


def _compress_build_spec_meta(build_spec: BuildSpec) -> dict[str, Any]:
    return {
        "prompt_id": build_spec.prompt_id,
        "title": build_spec.title,
        "pitch": _truncate(build_spec.pitch, 600),
        "game_type": build_spec.game_type.value,
        "locomotion": build_spec.locomotion.value,
        "comfort": build_spec.comfort.value,
        "player_height_meters": build_spec.player_height_meters,
        "constraints": list(build_spec.constraints),
    }


def _compress_asset(asset: Any) -> dict[str, Any]:
    role = getattr(asset, "character_role", None)
    return {
        "id": asset.id,
        "name": asset.name,
        "category": asset.category.value if hasattr(asset.category, "value") else str(asset.category),
        # The manifest states outright whether a character is the player's own
        # body, an NPC or set dressing. Omitting it left the composer guessing
        # from ``style_notes`` prose, which is how a player avatar ends up
        # placed as an NPC standing next to the player.
        "character_role": getattr(role, "value", None) or str(role or "none"),
        "style_notes": _truncate(asset.style_notes, 400),
        "target_scale_meters": asset.target_scale_meters,
        "collision_shape": asset.collision_shape,
        "rigged": asset.rigged,
        "glb_path": _glb_path_for_asset(asset.id),
    }


def _glb_path_for_asset(asset_id: str) -> str:
    """Canonical project-relative GLB path the asset_normalization stage
    will produce. Hard-coding this here (instead of asking the LLM to
    guess) eliminates a class of validation failures downstream."""
    return f"assets/{asset_id}.glb"


def _compress_mechanics(
    build_spec: BuildSpec, plan: MechanicPlan
) -> list[dict[str, Any]]:
    # Index BuildSpec.mechanics by id so we can show the short brief next
    # to the planner's classification without leaking the full 4K summary.
    bs_by_id = {m.id: m for m in build_spec.mechanics}

    out: list[dict[str, Any]] = []
    for item in plan.items:
        cls = item.classification.value
        binding_id: Optional[str]
        if cls in ("generate", "builtin+config_code_edit"):
            # The SceneSpec must bind to the generated module the codegen
            # stage will produce (both 'generate' and 'code_edit' emit a NEW
            # mechanic). Convention is generated.<code_task_id>.
            task_id = item.code_task.id if item.code_task else item.id
            binding_id = f"generated.{task_id}"
        else:
            binding_id = item.builtin_id or None

        bs_entry = bs_by_id.get(item.id)
        out.append(
            {
                "id": item.id,
                "name": bs_entry.name if bs_entry else item.id,
                "type": bs_entry.type.value if bs_entry else "regular",
                "classification": cls,
                "binding_id": binding_id,
                # Tight brief so the composer knows which entity should
                # own the binding without paying for the full BuildSpec
                # summary in tokens.
                "brief": _truncate(bs_entry.summary, 400) if bs_entry else "",
                "config": item.config or {},
                "depends_on_assets": list(bs_entry.depends_on_assets)
                if bs_entry
                else [],
                "required_in_scenes": list(bs_entry.required_in_scenes)
                if bs_entry
                else [],
            }
        )
    return out


def _truncate(text: Optional[str], limit: int) -> str:
    if text is None:
        return ""
    t = str(text)
    if len(t) <= limit:
        return t
    # Trim on a word boundary when possible so the truncation isn't
    # visually jarring to the LLM.
    cut = t[:limit].rsplit(" ", 1)[0] or t[:limit]
    return cut + " ..."


# ---------------------------------------------------------------------------
# Internal types
# ---------------------------------------------------------------------------


@dataclass
class _SceneJob:
    index: int
    scene_spec: SceneSpec
    artifacts: list[str]
