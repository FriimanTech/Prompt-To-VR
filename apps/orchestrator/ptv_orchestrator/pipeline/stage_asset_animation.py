"""Stage 6c: Tripo animation-retarget -- give rigged NPCs real movement.

A rigged enemy that cannot animate is just a statue that slides across the
floor. This stage is what turns it into something that walks, runs and
attacks: it retargets Tripo's preset animation library onto the skeleton the
previous stage produced, and the clips ride inside the same GLB the runtime
already loads.

Runs after ``asset_rigging`` and before ``asset_normalization``, so the
animated GLB becomes the primary asset every later stage sees -- validated,
socketed, decimated (``ptv_glb_optimize`` copies clips through) and finally
compiled into ``scene.json``.

Two things make this stage more than a single API call:

1. **Preset ids depend on the rig.** v1.0 biped has 90+, v2.5 biped has 11,
   and a quadruped rig has exactly one. The manifest therefore declares
   semantic INTENTS ("walk", "attack") and ``resolve_animations`` maps them
   onto real presets using the ``rig_type`` and ``model`` recorded in
   ``asset_rig.json``. Intents a rig cannot express are dropped loudly rather
   than substituted with something that would animate wrongly.

2. **Tripo names the clips, not us.** ``ISkeleton::play_clip`` matches on the
   glTF animation name, so this stage opens the returned GLB, reads the names
   that are ACTUALLY in it, and records intent -> real clip name. The scene
   compiler forwards that map so generated C++ can say ``play_clip(self,
   "walk")`` without knowing what Tripo called it.

Billing is per animation (~20 credits each), so ``asset_animation.json``
records every outcome and a Resume never re-pays. Delete that file and
``model_animated.glb`` to force a re-run.
"""

from __future__ import annotations

import json
import shutil
import struct
from pathlib import Path
from typing import Any

from ptv_providers import TripoClient, TripoConfig, resolve_animations
from ptv_schemas import ProjectStage, ProjectStatus

from .context import PipelineContext, StageResult

_STAGE = ProjectStage.asset_animation

# Tripo bills roughly 20 credits per retargeted animation.
_CREDITS_PER_ANIMATION = 20

# The API accepts at most 5 animations in one batch call.
_MAX_ANIMATIONS = 5

_ANIMATED_GLB_NAME = "model_animated.glb"
_ANIMATION_REPORT_NAME = "asset_animation.json"
_RIG_REPORT_NAME = "asset_rig.json"

_GLB_MAGIC = 0x46546C67
_CHUNK_JSON = 0x4E4F534A


def run(ctx: PipelineContext) -> StageResult:
    ctx.emit(_STAGE, "running", "Animating rigged assets")
    state = ctx.store.load_state(ctx.project_id)
    ctx.store.record_stage(state, _STAGE, ProjectStatus.running)

    manifest = ctx.asset_manifest or ctx.store.load_asset_manifest(ctx.project_id)
    assets_dir = ctx.project_dir / "assets"
    report_path = ctx.project_dir / _ANIMATION_REPORT_NAME
    report: dict[str, Any] = _load_json(report_path)
    rig_report: dict[str, Any] = _load_json(ctx.project_dir / _RIG_REPORT_NAME)

    if not ctx.settings.tripo_animation_enabled:
        message = "Animation disabled (tripo_animation_enabled=false)"
        ctx.store.record_stage(state, _STAGE, ProjectStatus.succeeded, message=message)
        ctx.emit(_STAGE, "succeeded", message)
        return StageResult(stage=_STAGE, artifacts=[])

    cap = max(1, min(_MAX_ANIMATIONS, ctx.settings.tripo_animation_max_per_asset))

    # ------------------------------------------------------------------
    # Work out what still needs animating.
    # ------------------------------------------------------------------
    produced: list[str] = []
    pending: list[dict[str, Any]] = []
    reused = 0
    billable = 0

    for item in manifest.items:
        intents = [a.value for a in getattr(item, "animations", [])]
        if not item.rigged or not intents:
            continue

        item_dir = assets_dir / item.id
        animated_glb = item_dir / _ANIMATED_GLB_NAME

        prior = report.get(item.id)
        if isinstance(prior, dict) and prior.get("status") in (
            "animated",
            "no_presets",
            "not_rigged",
        ):
            if prior.get("status") != "animated":
                reused += 1
                continue
            if animated_glb.is_file():
                produced.append(_rel(ctx, animated_glb))
                reused += 1
                continue

        rig = rig_report.get(item.id)
        if not isinstance(rig, dict) or rig.get("status") != "rigged":
            # Not an error: rig-check may have said the asset is not a
            # creature. It simply stays static.
            report[item.id] = {
                "status": "not_rigged",
                "reason": "no successful rig recorded in asset_rig.json",
            }
            continue

        rig_task_id = rig.get("rig_task_id")
        if not rig_task_id:
            # A rigged GLB exists but its task lineage was never recorded
            # (rigged before this pipeline saved it, or by hand). Retarget
            # cannot take a file, only a rig task, so say so plainly rather
            # than half-animating.
            msg = (
                f"{item.id}: rigged GLB has no recorded rig_task_id; "
                "re-run asset_rigging for this asset to enable animation"
            )
            ctx.emit(_STAGE, "warning", msg)
            ctx.store.append_log(ctx.project_id, "asset_animation", msg)
            report[item.id] = {"status": "missing_rig_task"}
            continue

        rig_type = str(rig.get("rig_type") or "biped")
        rig_model = rig.get("model")
        presets, mapping, dropped = resolve_animations(
            intents[:cap], rig_type, rig_model
        )
        if dropped:
            ctx.emit(
                _STAGE,
                "info",
                f"{item.id}: {rig_type} rig has no preset for "
                f"{', '.join(dropped)}; skipping those",
            )
        if not presets:
            report[item.id] = {
                "status": "no_presets",
                "rig_type": rig_type,
                "requested": intents,
                "dropped": dropped,
            }
            continue

        billable += len(presets)
        pending.append(
            {
                "item": item,
                "item_dir": item_dir,
                "rig_task_id": str(rig_task_id),
                "rig_type": rig_type,
                "model": rig_model,
                "presets": presets,
                "mapping": mapping,
                "dropped": dropped,
                "requested": intents,
            }
        )

    if reused:
        ctx.emit(
            _STAGE,
            "info",
            f"reusing {reused} animation result(s) already recorded; "
            f"{len(pending)} asset(s) still need Tripo",
        )

    if not pending:
        _save_json(report_path, report)
        message = (
            f"{len(produced)} animated asset(s) already on disk; no Tripo calls made"
            if produced
            else "No rigged assets requested animations; nothing to do"
        )
        ctx.store.record_stage(
            state, _STAGE, ProjectStatus.succeeded, message=message,
            artifacts=produced,
        )
        ctx.emit(_STAGE, "succeeded", message)
        return StageResult(stage=_STAGE, artifacts=produced)

    client = _make_client(ctx)

    # ------------------------------------------------------------------
    # Balance preflight -- per ANIMATION, not per asset.
    # ------------------------------------------------------------------
    required = billable * _CREDITS_PER_ANIMATION
    try:
        balance_info = client.get_balance()
    except Exception as exc:  # noqa: BLE001 - surfaced verbatim
        msg = (
            f"Tripo preflight balance check failed: {type(exc).__name__}: {exc}. "
            "Verify TRIPO_API_KEY in .env and that the account is active."
        )
        ctx.store.append_log(ctx.project_id, "asset_animation", msg)
        raise RuntimeError(msg) from exc

    available = float(balance_info.get("balance", 0.0))
    ctx.store.append_log(
        ctx.project_id,
        "asset_animation",
        f"Tripo balance preflight: available={available:.1f} credits, "
        f"required_estimate={required} ({billable} animation(s) across "
        f"{len(pending)} asset(s)).",
    )
    ctx.emit(
        _STAGE,
        "info",
        f"Tripo balance: {available:.0f} credits "
        f"(need ~{required} for {billable} animation(s))",
    )
    if available < required:
        msg = (
            f"Insufficient Tripo credits for animation: have {available:.1f}, "
            f"need ~{required} for {billable} animation(s) "
            f"(~{_CREDITS_PER_ANIMATION} credits each). "
            "Top up at https://developers.tripo3d.ai then press Resume."
        )
        ctx.store.append_log(ctx.project_id, "asset_animation", msg)
        raise RuntimeError(msg)

    # ------------------------------------------------------------------
    # Retarget.
    # ------------------------------------------------------------------
    animated = 0
    errors: list[str] = []

    for job in pending:
        ctx.raise_if_cancelled()
        item = job["item"]
        item_dir: Path = job["item_dir"]
        presets: list[str] = job["presets"]
        try:
            ctx.emit(
                _STAGE,
                "info",
                f"{item.id}: retargeting {len(presets)} animation(s) onto its "
                f"{job['rig_type']} rig",
            )
            staging = item_dir / ".anim_download"
            shutil.rmtree(staging, ignore_errors=True)
            result = client.retarget(
                out_dir=staging,
                rig_task_id=job["rig_task_id"],
                animations=presets,
                out_format="glb",
                bake_animation=True,
                export_with_geometry=True,
                # Always in place. A clip carrying baked root motion would
                # translate the mesh on its own ON TOP of the engine moving
                # the entity, so the character covers double the ground and
                # snaps back to the origin on every loop.
                animate_in_place=True,
            )
            downloaded = result.primary_glb
            if downloaded is None:
                raise RuntimeError("retarget task returned no GLB")

            final = item_dir / _ANIMATED_GLB_NAME
            final.unlink(missing_ok=True)
            shutil.move(str(downloaded), str(final))
            shutil.rmtree(staging, ignore_errors=True)

            # Read the clip names that are ACTUALLY in the file rather than
            # assuming the batch call honoured every preset. Tripo names the
            # clips, and the whole downstream contract depends on those names.
            clip_names = _glb_animation_names(final)
            intent_to_clip = _pair_intents_to_clips(job["mapping"], presets,
                                                    clip_names)

            if len(clip_names) < len(presets):
                warn = (
                    f"{item.id}: asked for {len(presets)} animation(s) but the "
                    f"returned GLB contains {len(clip_names)} "
                    f"({', '.join(clip_names) or 'none'}). Only the clips "
                    "present are exposed to gameplay."
                )
                ctx.emit(_STAGE, "warning", warn)
                ctx.store.append_log(ctx.project_id, "asset_animation", warn)

            produced.append(_rel(ctx, final))
            report[item.id] = {
                "status": "animated",
                "retarget_task_id": result.task_id,
                "rig_task_id": job["rig_task_id"],
                "rig_type": job["rig_type"],
                "model": job["model"],
                "requested_intents": job["requested"],
                "dropped_intents": job["dropped"],
                "presets": presets,
                "clip_names": clip_names,
                "animations": intent_to_clip,
                "animated_glb": _rel(ctx, final),
                "animate_in_place": True,
                "credits_estimate": len(presets) * _CREDITS_PER_ANIMATION,
            }
            animated += 1
            ctx.emit(
                _STAGE,
                "info",
                f"{item.id}: animated with {len(clip_names)} clip(s)",
            )
        except Exception as exc:  # noqa: BLE001 - one bad asset must not stop the rest
            ctx.store.append_log(ctx.project_id, "asset_animation", f"{item.id}: {exc}")
            ctx.emit(_STAGE, "warning", f"{item.id}: {exc}")
            errors.append(f"{item.id}: {exc}")
            report[item.id] = {"status": "failed", "error": str(exc)}

    _save_json(report_path, report)

    # A failed retarget is recoverable: the rigged GLB is still on disk and
    # the asset renders as a static skinned mesh. Far better than failing the
    # whole build, so this stage does not hard-fail on a barren pass.
    parts = [f"{animated} animated"]
    if reused:
        parts.append(f"{reused} reused")
    if errors:
        parts.append(f"{len(errors)} failed")
    message = ", ".join(parts)

    ctx.store.record_stage(
        state, _STAGE, ProjectStatus.succeeded, message=message, artifacts=produced
    )
    ctx.emit(_STAGE, "succeeded", message)
    return StageResult(stage=_STAGE, artifacts=produced)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _make_client(ctx: PipelineContext) -> TripoClient:
    kwargs: dict[str, Any] = {"api_key": ctx.settings.tripo_api_key}
    if ctx.settings.tripo_base_url:
        kwargs["base_url"] = ctx.settings.tripo_base_url
    return TripoClient(TripoConfig(**kwargs))


def _rel(ctx: PipelineContext, path: Path) -> str:
    return str(path.relative_to(ctx.settings.repo_root)).replace("\\", "/")


def _glb_animation_names(path: Path) -> list[str]:
    """Clip names actually present in a GLB, in file order.

    Reads the JSON chunk only -- the BIN chunk of an animated character is
    tens of megabytes and none of it is needed to answer this.
    """

    try:
        data = path.read_bytes()
    except OSError:
        return []
    if len(data) < 20:
        return []
    magic, = struct.unpack_from("<I", data, 0)
    if magic != _GLB_MAGIC:
        return []
    chunk_len, chunk_type = struct.unpack_from("<II", data, 12)
    if chunk_type != _CHUNK_JSON or 20 + chunk_len > len(data):
        return []
    try:
        doc = json.loads(data[20 : 20 + chunk_len])
    except json.JSONDecodeError:
        return []

    names: list[str] = []
    for i, anim in enumerate(doc.get("animations") or []):
        if not isinstance(anim, dict):
            continue
        # glTF animation names are optional; fall back to the index so a
        # nameless clip is still addressable.
        names.append(str(anim.get("name") or f"animation_{i}"))
    return names


def _pair_intents_to_clips(
    mapping: dict[str, str], presets: list[str], clip_names: list[str]
) -> dict[str, str]:
    """Map each requested intent onto the clip that fulfils it.

    Tripo returns clips in the order the presets were sent, so preset i pairs
    with clip i. Anything beyond what actually came back is left out entirely
    -- an intent pointing at a clip that does not exist would make
    ``play_clip`` fail at runtime for no visible reason.
    """

    preset_to_clip: dict[str, str] = {}
    for i, preset in enumerate(presets):
        if i < len(clip_names):
            preset_to_clip[preset] = clip_names[i]

    out: dict[str, str] = {}
    for intent, preset in mapping.items():
        clip = preset_to_clip.get(preset)
        if clip:
            out[intent] = clip
    return out


def _load_json(path: Path) -> dict[str, Any]:
    if not path.is_file():
        return {}
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}
    return data if isinstance(data, dict) else {}


def _save_json(path: Path, payload: dict[str, Any]) -> None:
    path.write_text(
        json.dumps(payload, indent=2, sort_keys=True), encoding="utf-8"
    )
