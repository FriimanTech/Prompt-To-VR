"""Stage 5: per-asset reference images via Google GenAI (Imagen + Gemini).

Idempotency: if an asset's ``images/<asset_id>/`` directory already
contains at least one ``.png``/``.jpg``/``.jpeg``/``.webp`` file, the
asset is treated as already-imaged and skipped. This means a Resume
after a partial run does not re-pay for Imagen calls that were already
made. Delete an asset's image directory to force regeneration.
"""

from __future__ import annotations

from pathlib import Path

from ptv_providers import GoogleImageClient, GoogleImageConfig
from ptv_schemas import ProjectStage, ProjectStatus

from .context import PipelineContext, StageResult


_IMAGE_EXTS = (".png", ".jpg", ".jpeg", ".webp")


def run(ctx: PipelineContext) -> StageResult:
    ctx.emit(ProjectStage.reference_images, "running", "Generating reference images")
    state = ctx.store.load_state(ctx.project_id)
    ctx.store.record_stage(state, ProjectStage.reference_images, ProjectStatus.running)

    manifest = ctx.asset_manifest or ctx.store.load_asset_manifest(ctx.project_id)

    out_root = ctx.project_dir / "images"
    out_root.mkdir(parents=True, exist_ok=True)
    produced: list[str] = []
    skipped: list[str] = []

    # Build the API client lazily. If every asset's images already exist
    # we never touch Google credentials at all, which means the stage is
    # safe to "Resume" on a machine that lost its GOOGLE_API_KEY.
    client: GoogleImageClient | None = None

    def _client() -> GoogleImageClient:
        nonlocal client
        if client is None:
            client = GoogleImageClient(
                GoogleImageConfig(
                    api_key=ctx.settings.google_api_key,
                    gemini_image_model=ctx.settings.gemini_image_model,
                )
            )
        return client

    for item in manifest.items:
        if item.strategy.value != "image_to_3d":
            # No reference image needed for text-to-3D.
            continue
        item_dir = out_root / item.id
        item_dir.mkdir(parents=True, exist_ok=True)

        existing = _existing_images(item_dir)
        if existing:
            for p in existing:
                produced.append(
                    str(p.relative_to(ctx.settings.repo_root)).replace("\\", "/")
                )
            skipped.append(item.id)
            ctx.emit(
                ProjectStage.reference_images,
                "info",
                f"{item.id}: reusing {len(existing)} image(s) already on disk",
            )
            continue

        try:
            results = _client().generate(
                prompt=_compose_prompt(item),
                out_dir=item_dir,
                number_of_images=item.reference.number_of_images,
                aspect_ratio=item.reference.aspect_ratio,
                negative_prompt=item.reference.negative_prompt,
                filename_prefix=f"{item.id}_ref",
                prefer="gemini",
            )
            for r in results:
                produced.append(
                    str(r.path.relative_to(ctx.settings.repo_root)).replace("\\", "/")
                )
            ctx.emit(
                ProjectStage.reference_images,
                "info",
                f"{item.id}: {len(results)} image(s)",
            )
        except Exception as e:
            ctx.store.append_log(
                ctx.project_id,
                "reference_images",
                f"{item.id}: {e}",
            )
            ctx.emit(ProjectStage.reference_images, "warning", f"{item.id}: {e}")

    message = f"{len(produced)} reference images"
    if skipped:
        message += f" ({len(skipped)} asset(s) reused from disk)"

    ctx.store.record_stage(
        state,
        ProjectStage.reference_images,
        ProjectStatus.succeeded,
        message=message,
        artifacts=produced,
    )
    ctx.emit(ProjectStage.reference_images, "succeeded", message)
    return StageResult(stage=ProjectStage.reference_images, artifacts=produced)


def _compose_prompt(item) -> str:
    base = item.reference.prompt.strip()
    if item.style_notes:
        return f"{base}\nStyle: {item.style_notes.strip()}"
    return base


def _existing_images(item_dir: Path) -> list[Path]:
    """Return any pre-existing reference image files for an asset.

    A non-empty list means we can skip the API call for this asset.
    """

    if not item_dir.exists():
        return []
    return sorted(
        p
        for p in item_dir.iterdir()
        if p.is_file() and p.suffix.lower() in _IMAGE_EXTS
    )
