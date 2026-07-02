"""Stage 6: Tripo image-to-3D (or text-to-3D fallback) per asset.

Idempotency: if an asset already has a valid ``.glb`` under
``assets/<asset_id>/``, the Tripo call for that asset is skipped. This
matters a lot here -- each image-to-3D call costs ~20 Tripo credits and
takes 30-90s to poll. A Resume after a partial run must not re-bill the
user for work that already landed on disk. Delete the asset's
sub-directory to force re-generation.

Preflight balance check is scoped to assets that ACTUALLY still need a
GLB, so a Resume where most assets are already on disk doesn't error
out on "insufficient credits" when the remaining work would fit.
"""

from __future__ import annotations

from pathlib import Path

from ptv_providers import TripoClient, TripoConfig
from ptv_schemas import ProjectStage, ProjectStatus

from .context import PipelineContext, StageResult


def run(ctx: PipelineContext) -> StageResult:
    ctx.emit(ProjectStage.tripo_models, "running", "Generating 3D assets")
    state = ctx.store.load_state(ctx.project_id)
    ctx.store.record_stage(state, ProjectStage.tripo_models, ProjectStatus.running)

    manifest = ctx.asset_manifest or ctx.store.load_asset_manifest(ctx.project_id)

    # ------------------------------------------------------------------
    # Idempotency pass: figure out which assets still need Tripo. An
    # asset is considered "done" when its sub-directory under
    # ``assets/`` already contains a usable GLB. We surface those as
    # ``produced`` so the artifacts list and downstream stages still
    # see them.
    # ------------------------------------------------------------------
    assets_dir = ctx.project_dir / "assets"
    produced: list[str] = []
    skipped: list[str] = []
    pending = []  # assets that still need a Tripo call
    for item in manifest.items:
        if item.strategy.value not in ("image_to_3d", "text_to_3d"):
            continue
        item_dir = assets_dir / item.id
        existing = _existing_glb(item_dir)
        if existing is not None:
            produced.append(
                str(existing.relative_to(ctx.settings.repo_root)).replace("\\", "/")
            )
            skipped.append(item.id)
        else:
            pending.append(item)

    if skipped:
        ctx.emit(
            ProjectStage.tripo_models,
            "info",
            f"reusing {len(skipped)} GLB(s) already on disk; "
            f"{len(pending)} asset(s) still need Tripo",
        )

    if not pending:
        # Nothing to do. Stamp success and exit; we never touch the
        # Tripo client or burn a balance preflight call.
        ctx.store.record_stage(
            state,
            ProjectStage.tripo_models,
            ProjectStatus.succeeded,
            message=(
                f"All {len(produced)} GLB(s) already on disk; no Tripo calls made"
            ),
            artifacts=produced,
        )
        ctx.emit(
            ProjectStage.tripo_models,
            "succeeded",
            f"{len(produced)} models (reused from disk)",
        )
        return StageResult(stage=ProjectStage.tripo_models, artifacts=produced)

    client = TripoClient(
        TripoConfig(
            api_key=ctx.settings.tripo_api_key,
            model_version=ctx.settings.tripo_model_version or None,
        )
    )

    # Preflight: check Tripo account balance BEFORE firing N image-to-3D
    # requests. Without this, a user whose account is out of credits would
    # watch every single asset attempt, fail, and wait the full polling
    # timeout. Each image-to-3D run on v2.5 costs ~20 credits, so we need
    # at least roughly ``len(pending) * 20`` credits to complete the
    # REMAINING work (not the whole manifest -- already-on-disk assets
    # don't need re-billing).
    required_credits = max(20, len(pending) * 20)
    try:
        balance_info = client.get_balance()
    except Exception as exc:
        msg = (
            "Tripo preflight balance check failed: "
            f"{type(exc).__name__}: {exc}. "
            "Verify TRIPO_API_KEY in .env and that the account is active."
        )
        ctx.store.append_log(ctx.project_id, "tripo_models", msg)
        raise RuntimeError(msg) from exc

    available = balance_info.get("balance", 0.0)
    ctx.store.append_log(
        ctx.project_id,
        "tripo_models",
        f"Tripo balance preflight: available={available:.1f} credits, "
        f"frozen={balance_info.get('frozen', 0.0):.1f}, "
        f"required_estimate={required_credits} (for {len(pending)} pending assets, "
        f"{len(skipped)} already on disk).",
    )
    ctx.emit(
        ProjectStage.tripo_models,
        "info",
        f"Tripo balance: {available:.0f} credits "
        f"(need ~{required_credits} for {len(pending)} pending assets)",
    )
    if available < required_credits:
        msg = (
            f"Insufficient Tripo credits: have {available:.1f}, "
            f"need ~{required_credits} for {len(pending)} assets "
            "(approx 20 credits per image-to-3D job on v2.5). "
            "Top up at https://platform.tripo3d.ai then press Resume."
        )
        ctx.store.append_log(ctx.project_id, "tripo_models", msg)
        raise RuntimeError(msg)

    attempted = 0
    per_item_errors: list[str] = []
    for item in pending:
        item_dir = assets_dir / item.id
        item_dir.mkdir(parents=True, exist_ok=True)
        attempted += 1
        try:
            if item.strategy.value == "image_to_3d":
                img = _pick_reference_image(ctx.project_dir / "images" / item.id)
                if img is None:
                    ctx.store.append_log(
                        ctx.project_id,
                        "tripo_models",
                        f"{item.id}: no reference image found; falling back to text-to-3D",
                    )
                    result = client.text_to_model(
                        prompt=item.reference.prompt,
                        out_dir=item_dir,
                        texture_quality=item.tripo.texture_quality.value,
                        face_limit=item.tripo.face_limit,
                    )
                else:
                    result = client.image_to_model(
                        image_path=img,
                        out_dir=item_dir,
                        texture_quality=item.tripo.texture_quality.value,
                        face_limit=item.tripo.face_limit,
                    )
            else:
                result = client.text_to_model(
                    prompt=item.reference.prompt,
                    out_dir=item_dir,
                    texture_quality=item.tripo.texture_quality.value,
                    face_limit=item.tripo.face_limit,
                )
            if result.primary_glb is None:
                ctx.emit(ProjectStage.tripo_models, "warning", f"{item.id}: no GLB")
                per_item_errors.append(f"{item.id}: no GLB returned")
                continue
            produced.append(
                str(result.primary_glb.relative_to(ctx.settings.repo_root)).replace("\\", "/")
            )
            ctx.emit(ProjectStage.tripo_models, "info", f"{item.id}: {result.status}")
        except Exception as e:
            ctx.store.append_log(
                ctx.project_id, "tripo_models", f"{item.id}: {e}"
            )
            ctx.emit(ProjectStage.tripo_models, "warning", f"{item.id}: {e}")
            per_item_errors.append(f"{item.id}: {e}")

    # Hard-fail if the pending list wanted Tripo work but none materialised.
    # Without this the pipeline would silently steamroll past the Tripo
    # stage, normalize 0 new GLBs, and the C++ build would compile against
    # missing entities. Fail loudly so the UI shows the error and the user
    # can fix credits / API keys and resume from this point.
    if attempted > 0 and len(produced) == len(skipped):
        # produced grew only by the reused-from-disk count; no new GLB
        # was generated this pass.
        detail = "; ".join(per_item_errors[:6]) if per_item_errors else "unknown"
        msg = (
            f"Tripo produced 0 NEW models from {attempted} pending asset(s). "
            f"Check TRIPO_API_KEY and account credits. First errors: {detail}"
        )
        raise RuntimeError(msg)

    message = f"{len(produced)} models produced"
    if skipped:
        message += f" ({len(skipped)} reused from disk, {attempted} via Tripo)"

    ctx.store.record_stage(
        state,
        ProjectStage.tripo_models,
        ProjectStatus.succeeded,
        message=message,
        artifacts=produced,
    )
    ctx.emit(ProjectStage.tripo_models, "succeeded", message)
    return StageResult(stage=ProjectStage.tripo_models, artifacts=produced)


def _pick_reference_image(images_dir: Path) -> Path | None:
    if not images_dir.exists():
        return None
    candidates = sorted(
        [
            p
            for p in images_dir.iterdir()
            if p.is_file() and p.suffix.lower() in (".png", ".jpg", ".jpeg", ".webp")
        ]
    )
    return candidates[0] if candidates else None


def _existing_glb(item_dir: Path) -> Path | None:
    """Return a usable GLB already on disk for this asset, or ``None``.

    We require a real-looking GLB (header magic) so a 0-byte truncated
    file from a crashed prior run gets RE-GENERATED instead of being
    treated as "done". Matches the same primary-GLB picking rule as
    :mod:`stage_asset_normalize`: prefer ``*pbr*`` / ``*model*`` files.
    """

    if not item_dir.exists():
        return None
    glbs = [p for p in item_dir.rglob("*.glb") if p.is_file()]
    if not glbs:
        return None
    glbs.sort(
        key=lambda p: (
            0 if "pbr" in p.name.lower() else 1,
            0 if "model" in p.name.lower() else 1,
            p.name.lower(),
        )
    )
    for candidate in glbs:
        try:
            with candidate.open("rb") as f:
                header = f.read(4)
        except OSError:
            continue
        # 'glTF' little-endian magic.
        if header == b"glTF":
            return candidate
    return None
