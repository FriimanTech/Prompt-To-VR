"""Stage 6b: Tripo rig-check + auto-rig for character assets.

Runs immediately after ``tripo_models`` and before ``asset_normalization``,
so the RIGGED mesh is what every downstream stage sees: normalization
validates it, ``asset_contract`` derives its sockets from it,
``asset_optimization`` decimates it while preserving the skin, and
``scene_compile`` marks it ``mesh.skinned`` in ``scene.json``. Rigging after
optimization would be far worse -- the LOD chain would all be un-rigged.

Two Tripo calls per asset, in this order and never the other way round:

1. ``/v3/animations/rig-check`` -- free. Answers "can this be rigged, and as
   what skeleton?". Every rig type it can return (``biped``, ``quadruped``,
   ``hexapod``, ``octopod``, ``avian``, ``serpentine``, ``aquatic``) describes
   a whole creature, so props, weapons and scenery come back
   ``riggable: false``.
2. ``/v3/animations/rig`` -- ~30 credits. Only for assets that passed the
   check. The ``model`` version is derived from the returned ``rig_type``,
   because the humanoid and creature checkpoints are not interchangeable and
   the wrong pairing produces a bad skeleton rather than an error.

Which assets are candidates comes from ``AssetItem.rigged``, set by the
asset-manifest agent. That is deliberately an explicit flag rather than
something inferred from ``category``: "is this thing a creature with a
skeleton" is a judgement about the asset, and getting it wrong costs 30
credits and a mangled mesh.

Idempotency: ``asset_rig.json`` records what happened per asset, so a Resume
never re-bills. Delete that file (and ``model_rigged.glb``) to force a re-rig.
"""

from __future__ import annotations

import json
import shutil
from pathlib import Path
from typing import Any

from ptv_providers import TripoClient, TripoConfig
from ptv_schemas import ProjectStage, ProjectStatus

from .context import PipelineContext, StageResult

_STAGE = ProjectStage.asset_rigging

# Tripo bills roughly 30 credits per rig. rig-check itself is free, so the
# preflight only has to cover the assets that could actually reach the rig
# call -- we cannot know which of those will pass the check, so budget for
# the pessimistic case where all of them do.
_CREDITS_PER_RIG = 30

_RIGGED_GLB_NAME = "model_rigged.glb"
_RIG_REPORT_NAME = "asset_rig.json"


def run(ctx: PipelineContext) -> StageResult:
    ctx.emit(_STAGE, "running", "Rigging character assets")
    state = ctx.store.load_state(ctx.project_id)
    ctx.store.record_stage(state, _STAGE, ProjectStatus.running)

    manifest = ctx.asset_manifest or ctx.store.load_asset_manifest(ctx.project_id)
    assets_dir = ctx.project_dir / "assets"
    report_path = ctx.project_dir / _RIG_REPORT_NAME
    report: dict[str, Any] = _load_report(report_path)

    if not ctx.settings.tripo_rigging_enabled:
        message = "Rigging disabled (tripo_rigging_enabled=false)"
        ctx.store.record_stage(state, _STAGE, ProjectStatus.succeeded, message=message)
        ctx.emit(_STAGE, "succeeded", message)
        return StageResult(stage=_STAGE, artifacts=[])

    # ------------------------------------------------------------------
    # Work out what still needs rigging.
    # ------------------------------------------------------------------
    produced: list[str] = []
    pending = []
    reused = 0
    for item in manifest.items:
        if not item.rigged:
            continue
        item_dir = assets_dir / item.id
        rigged_glb = item_dir / _RIGGED_GLB_NAME

        prior = report.get(item.id)
        if isinstance(prior, dict) and prior.get("status") in ("rigged", "not_riggable"):
            # A previous run already paid for the answer. Trust it unless the
            # file it points at has since disappeared.
            if prior.get("status") == "not_riggable":
                reused += 1
                continue
            if rigged_glb.is_file():
                produced.append(_rel(ctx, rigged_glb))
                reused += 1
                continue

        source = _source_glb(item_dir)
        if source is None:
            ctx.emit(_STAGE, "warning", f"{item.id}: no source GLB to rig")
            report[item.id] = {"status": "missing_source"}
            continue
        pending.append((item, item_dir, source))

    if reused:
        ctx.emit(
            _STAGE,
            "info",
            f"reusing {reused} rig result(s) already recorded; "
            f"{len(pending)} asset(s) still need Tripo",
        )

    if not pending:
        _save_report(report_path, report)
        message = (
            f"{len(produced)} rigged asset(s) already on disk; no Tripo calls made"
            if produced
            else "No assets marked rigged; nothing to do"
        )
        ctx.store.record_stage(
            state, _STAGE, ProjectStatus.succeeded, message=message,
            artifacts=produced,
        )
        ctx.emit(_STAGE, "succeeded", message)
        return StageResult(stage=_STAGE, artifacts=produced)

    client = _make_client(ctx)

    # ------------------------------------------------------------------
    # Balance preflight, mirroring stage_tripo_models: fail before firing N
    # rigs rather than after burning most of them.
    # ------------------------------------------------------------------
    required = len(pending) * _CREDITS_PER_RIG
    try:
        balance_info = client.get_balance()
    except Exception as exc:  # noqa: BLE001 - surfaced verbatim to the user
        msg = (
            f"Tripo preflight balance check failed: {type(exc).__name__}: {exc}. "
            "Verify TRIPO_API_KEY in .env and that the account is active."
        )
        ctx.store.append_log(ctx.project_id, "asset_rigging", msg)
        raise RuntimeError(msg) from exc

    available = float(balance_info.get("balance", 0.0))
    ctx.store.append_log(
        ctx.project_id,
        "asset_rigging",
        f"Tripo balance preflight: available={available:.1f} credits, "
        f"required_estimate={required} (for {len(pending)} rig candidate(s)).",
    )
    ctx.emit(
        _STAGE,
        "info",
        f"Tripo balance: {available:.0f} credits "
        f"(need ~{required} for {len(pending)} rig candidate(s))",
    )
    if available < required:
        msg = (
            f"Insufficient Tripo credits for rigging: have {available:.1f}, "
            f"need ~{required} for {len(pending)} asset(s) "
            f"(~{_CREDITS_PER_RIG} credits per rig). "
            "Top up at https://developers.tripo3d.ai then press Resume."
        )
        ctx.store.append_log(ctx.project_id, "asset_rigging", msg)
        raise RuntimeError(msg)

    # ------------------------------------------------------------------
    # rig-check, then rig.
    # ------------------------------------------------------------------
    rigged = 0
    not_riggable = 0
    errors: list[str] = []

    for item, item_dir, source in pending:
        ctx.raise_if_cancelled()
        task_id = _read_task_id(item_dir)
        try:
            check = client.rig_check(task_id=task_id, glb_path=None if task_id else source)
            if not check.riggable:
                not_riggable += 1
                report[item.id] = {
                    "status": "not_riggable",
                    "rig_check_task_id": check.task_id,
                    "source_glb": _rel(ctx, source),
                }
                ctx.emit(
                    _STAGE,
                    "info",
                    f"{item.id}: not riggable (Tripo only rigs whole creatures); "
                    "left un-rigged",
                )
                ctx.store.append_log(
                    ctx.project_id,
                    "asset_rigging",
                    f"{item.id}: rig-check says riggable=false; skipping the "
                    f"{_CREDITS_PER_RIG}-credit rig call.",
                )
                continue

            rig_model = check.rig_model
            ctx.emit(
                _STAGE,
                "info",
                f"{item.id}: riggable as {check.rig_type} -> rigging with {rig_model}",
            )
            # Download into a scratch dir, NOT straight into the asset dir.
            # The client always names its download ``model.glb``, which is
            # exactly the un-rigged source sitting there -- writing in place
            # would destroy the original and a re-run would then rig an
            # already-rigged mesh.
            staging = item_dir / ".rig_download"
            shutil.rmtree(staging, ignore_errors=True)
            result = client.rig(
                out_dir=staging,
                task_id=task_id,
                glb_path=None if task_id else source,
                rig_type=check.rig_type,
                # tripo (not mixamo): Mixamo-spec skeletons make every
                # subsequent /animations/retarget call fail with error 1004.
                spec="tripo",
                out_format="glb",
                model=rig_model,
            )
            downloaded = result.primary_glb
            if downloaded is None:
                raise RuntimeError("rig task returned no GLB")

            final = item_dir / _RIGGED_GLB_NAME
            final.unlink(missing_ok=True)
            shutil.move(str(downloaded), str(final))
            shutil.rmtree(staging, ignore_errors=True)

            produced.append(_rel(ctx, final))
            report[item.id] = {
                "status": "rigged",
                "rig_check_task_id": check.task_id,
                "rig_task_id": result.task_id,
                "rig_type": check.rig_type,
                "model": rig_model,
                "spec": "tripo",
                "out_format": "glb",
                "source_glb": _rel(ctx, source),
                "rigged_glb": _rel(ctx, final),
                "credits_estimate": _CREDITS_PER_RIG,
            }
            rigged += 1
            ctx.emit(_STAGE, "info", f"{item.id}: rigged ({check.rig_type})")
        except Exception as exc:  # noqa: BLE001 - one bad asset must not stop the rest
            ctx.store.append_log(ctx.project_id, "asset_rigging", f"{item.id}: {exc}")
            ctx.emit(_STAGE, "warning", f"{item.id}: {exc}")
            errors.append(f"{item.id}: {exc}")
            report[item.id] = {"status": "failed", "error": str(exc)}

    _save_report(report_path, report)

    # A failed rig is recoverable -- the un-rigged GLB is still on disk and the
    # asset renders as static geometry, it just cannot be animated. That is a
    # far better outcome than failing the whole build, so unlike
    # stage_tripo_models this stage does not hard-fail on a barren pass.
    parts = [f"{rigged} rigged"]
    if not_riggable:
        parts.append(f"{not_riggable} not riggable")
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
    # ``model_version`` deliberately not forwarded: it selects the
    # image-to-3D checkpoint, while the rig call carries its own ``model``
    # chosen from the rig type.
    return TripoClient(TripoConfig(**kwargs))


def _rel(ctx: PipelineContext, path: Path) -> str:
    return str(path.relative_to(ctx.settings.repo_root)).replace("\\", "/")


def _source_glb(item_dir: Path) -> Path | None:
    """The UN-rigged Tripo output for this asset.

    Deliberately not the shared "primary GLB" picker: that one prefers
    animated then rigged files, which would make a second run rig a mesh that
    already has a skeleton (and possibly clips) on it.
    """

    if not item_dir.is_dir():
        return None
    skip = ("rigged", "animated", "_lod")
    candidates = [
        p
        for p in item_dir.glob("*.glb")
        if p.is_file() and not any(s in p.name.lower() for s in skip)
    ]
    if not candidates:
        return None
    candidates.sort(
        key=lambda p: (
            0 if "pbr" in p.name.lower() else 1,
            0 if "model" in p.name.lower() else 1,
            p.name.lower(),
        )
    )
    return candidates[0]


def _read_task_id(item_dir: Path) -> str | None:
    """The generation task id recorded by ``stage_tripo_models``, if any."""

    record = item_dir / "tripo_task.json"
    if not record.is_file():
        return None
    try:
        data = json.loads(record.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None
    task_id = data.get("task_id")
    return str(task_id) if task_id else None


def _load_report(path: Path) -> dict[str, Any]:
    if not path.is_file():
        return {}
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}
    return data if isinstance(data, dict) else {}


def _save_report(path: Path, report: dict[str, Any]) -> None:
    path.write_text(
        json.dumps(report, indent=2, sort_keys=True), encoding="utf-8"
    )
