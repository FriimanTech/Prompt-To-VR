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
import struct
from pathlib import Path
from typing import Any, Iterable

from ptv_providers import TripoClient, TripoConfig, resolve_tripo_motions
from ptv_schemas import ProjectStage, ProjectStatus

from ..fsutil import atomic_write_text, move_with_retry, rmtree_with_retry
from .character_artifacts import load_routing
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
    manifest_ids = [it.id for it in manifest.items]

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

    routing = load_routing(ctx.project_dir)
    for item in manifest.items:
        intents = [a.value for a in getattr(item, "animations", [])]
        route = routing.route_for(item.id) if routing else None
        if route is not None and route.effective_animation_provider().value != "tripo":
            # Animated by Uthana (or not at all) -- see uthana_animation.
            continue
        # Exact presets character_routing chose from the full v1.0 library.
        # A character that FELL BACK from Uthana keeps its semantic intents:
        # its routed motions are Uthana gaits Tripo cannot read.
        requests: list[tuple[str, str | None]] = [(i, None) for i in intents]
        if route is not None and route.fallback is None:
            tripo_motions = [m for m in route.required_motions
                             if m.source.value == "tripo_preset"]
            if tripo_motions:
                requests = [(m.key, m.tripo_preset) for m in tripo_motions]
        if not item.rigged or not requests:
            continue

        item_dir = assets_dir / item.id
        animated_glb = item_dir / _ANIMATED_GLB_NAME

        prior = report.get(item.id)
        if isinstance(prior, dict) and prior.get("status") == "downloaded":
            # Paid for and downloaded; only the move to its final name failed.
            staged = item_dir / ".anim_download"
            glbs = sorted(staged.glob("*.glb")) if staged.is_dir() else []
            try:
                if glbs:
                    prior = _finish_download(ctx, item.id, item_dir, glbs[0], prior)
                    report[item.id] = prior
            except Exception as exc:  # noqa: BLE001 - retried next run
                ctx.emit(_STAGE, "warning",
                         f"{item.id}: could not finish the animated download: {exc}")
        rig_now = rig_report.get(item.id)
        rigged_now = isinstance(rig_now, dict) and rig_now.get("status") == "rigged"
        if (isinstance(prior, dict) and prior.get("status") == "not_rigged"
                and rigged_now):
            # It has been rigged since (a repaired or re-run rig): animate it.
            prior = None
        if isinstance(prior, dict) and prior.get("status") in (
            "animated",
            "no_presets",
            "not_rigged",
        ):
            if prior.get("status") != "animated":
                reused += 1
                continue
            if animated_glb.is_file():
                _repair_clip_map(ctx, item.id, prior, requests, cap,
                                 rig_report.get(item.id), animated_glb)
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
        presets, mapping, dropped = resolve_tripo_motions(
            requests[:cap], rig_type, rig_model
        )
        intents = [k for k, _ in requests]
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
        _save_json(report_path, report, manifest_ids)
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
            rmtree_with_retry(staging)
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

            # Record the paid retarget BEFORE touching files, so a move that
            # fails (another process holding the download) is finished on the
            # next run instead of being paid for again.
            report[item.id] = {
                "status": "downloaded",
                "provider": "tripo",
                "retarget_task_id": result.task_id,
                "rig_task_id": job["rig_task_id"],
                "rig_type": job["rig_type"],
                "model": job["model"],
                "requested_intents": job["requested"],
                "dropped_intents": job["dropped"],
                "presets": presets,
                "mapping": job["mapping"],
                "staged_glb": _rel(ctx, downloaded),
            }
            _save_json(report_path, report, manifest_ids)

            entry = _finish_download(ctx, item.id, item_dir, downloaded,
                                     report[item.id])
            report[item.id] = entry
            produced.append(entry["animated_glb"])
            animated += 1
            ctx.emit(
                _STAGE,
                "info",
                f"{item.id}: animated with {len(entry['clip_names'])} clip(s)",
            )
        except Exception as exc:  # noqa: BLE001 - one bad asset must not stop the rest
            ctx.store.append_log(ctx.project_id, "asset_animation", f"{item.id}: {exc}")
            ctx.emit(_STAGE, "warning", f"{item.id}: {exc}")
            errors.append(f"{item.id}: {exc}")
            prior = report.get(item.id)
            if isinstance(prior, dict) and prior.get("status") == "downloaded":
                report[item.id] = dict(prior, error=str(exc))
            else:
                report[item.id] = {"status": "failed", "error": str(exc)}

    _save_json(report_path, report, manifest_ids)

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


def _repair_clip_map(
    ctx: PipelineContext,
    asset_id: str,
    prior: dict[str, Any],
    requests: list[tuple[str, str | None]],
    cap: int,
    rig: Any,
    animated_glb: Path,
) -> None:
    """Re-derive intent -> clip for an asset animated earlier.

    Older runs paired clips by position and could map ``run`` onto the
    ``fall`` clip. The presets and the GLB are already paid for and on disk,
    so this costs nothing: the same preset resolution is repeated and paired
    against the clip names actually in the file. ``prior`` is updated in
    place.
    """

    clip_names = _glb_animation_names(animated_glb)
    if not clip_names:
        return
    rig_type = str((rig or {}).get("rig_type") or prior.get("rig_type") or "biped")
    rig_model = (rig or {}).get("model") or prior.get("model")
    _presets, mapping, _dropped = resolve_tripo_motions(
        requests[:cap], rig_type, rig_model
    )
    presets = list(prior.get("presets") or _presets)
    fixed = _pair_intents_to_clips(mapping, presets, clip_names)
    if fixed and fixed != prior.get("animations"):
        msg = (
            f"{asset_id}: corrected intent -> clip map "
            f"{prior.get('animations')} -> {fixed}"
        )
        ctx.emit(_STAGE, "info", msg)
        ctx.store.append_log(ctx.project_id, "asset_animation", msg)
        prior["animations"] = fixed
        prior["clip_names"] = clip_names


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


def _name_key(name: str) -> str:
    """Comparable form of a preset or clip name.

    ``preset:biped:slash``, ``Armature|preset_slash``, ``slash`` and
    ``Slash`` all reduce to ``slash``.
    """

    tail = name.replace("|", ":").rsplit(":", 1)[-1].lower()
    for prefix in ("preset_", "biped_", "quadruped_", "animation_"):
        if tail.startswith(prefix):
            tail = tail[len(prefix):]
    return "".join(ch for ch in tail if ch.isalnum())


def _pair_intents_to_clips(
    mapping: dict[str, str], presets: list[str], clip_names: list[str]
) -> dict[str, str]:
    """Map each requested intent onto the clip that fulfils it.

    By NAME: Tripo does not return clips in the order the presets were sent
    (a request for run, slash, hurt, fall came back fall, hurt, run, slash),
    so pairing by position sent a running goblin into its fall animation.
    Each preset takes the clip whose name matches its own; only presets no
    name accounts for fall back to the leftover clips in order. Anything
    beyond what actually came back is left out entirely -- an intent pointing
    at a clip that does not exist would make ``play_clip`` fail at runtime
    for no visible reason.
    """

    preset_to_clip: dict[str, str] = {}
    used: set[str] = set()
    by_key: dict[str, list[str]] = {}
    for clip in clip_names:
        by_key.setdefault(_name_key(clip), []).append(clip)

    unmatched: list[str] = []
    for preset in presets:
        candidates = [c for c in by_key.get(_name_key(preset), []) if c not in used]
        if candidates:
            preset_to_clip[preset] = candidates[0]
            used.add(candidates[0])
        else:
            unmatched.append(preset)

    leftovers = [c for c in clip_names if c not in used]
    for preset, clip in zip(unmatched, leftovers):
        preset_to_clip[preset] = clip

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


def _save_json(path: Path, payload: dict[str, Any],
               manifest_ids: Iterable[str] = ()) -> None:
    from .character_artifacts import prune_to_assets

    prune_to_assets(payload, manifest_ids)
    atomic_write_text(path, json.dumps(payload, indent=2, sort_keys=True))


def _finish_download(ctx: PipelineContext, item_id: str, item_dir: Path,
                     downloaded: Path, pending: dict[str, Any]) -> dict[str, Any]:
    """Move a retargeted download into place and read its real clip names.

    ``pending`` is the ``downloaded`` report entry, which carries everything
    the final entry needs."""
    final = item_dir / _ANIMATED_GLB_NAME
    move_with_retry(downloaded, final)
    rmtree_with_retry(item_dir / ".anim_download")

    # Read the clip names that are ACTUALLY in the file rather than assuming
    # the batch call honoured every preset. Tripo names the clips, and the
    # whole downstream contract depends on those names.
    presets = list(pending.get("presets") or [])
    clip_names = _glb_animation_names(final)
    intent_to_clip = _pair_intents_to_clips(pending.get("mapping") or {}, presets,
                                            clip_names)
    if len(clip_names) < len(presets):
        warn = (
            f"{item_id}: asked for {len(presets)} animation(s) but the "
            f"returned GLB contains {len(clip_names)} "
            f"({', '.join(clip_names) or 'none'}). Only the clips "
            "present are exposed to gameplay."
        )
        ctx.emit(_STAGE, "warning", warn)
        ctx.store.append_log(ctx.project_id, "asset_animation", warn)

    entry = {k: v for k, v in pending.items()
             if k not in ("status", "mapping", "staged_glb", "error")}
    entry.update({
        "status": "animated",
        "clip_names": clip_names,
        "animations": intent_to_clip,
        "animated_glb": _rel(ctx, final),
        "animate_in_place": True,
        "credits_estimate": len(presets) * _CREDITS_PER_ANIMATION,
    })
    return entry
