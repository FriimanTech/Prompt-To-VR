"""Stage 5: per-asset reference images via Google GenAI (Imagen + Gemini)."""

from __future__ import annotations

from ptv_providers import GoogleImageClient, GoogleImageConfig
from ptv_schemas import ProjectStage, ProjectStatus

from .context import PipelineContext, StageResult


def run(ctx: PipelineContext) -> StageResult:
    ctx.emit(ProjectStage.reference_images, "running", "Generating reference images")
    state = ctx.store.load_state(ctx.project_id)
    ctx.store.record_stage(state, ProjectStage.reference_images, ProjectStatus.running)

    manifest = ctx.asset_manifest or ctx.store.load_asset_manifest(ctx.project_id)
    client = GoogleImageClient(
        GoogleImageConfig(
            api_key=ctx.settings.google_api_key,
            imagen_model=ctx.settings.imagen_model,
            gemini_image_model=ctx.settings.gemini_image_model,
        )
    )

    out_root = ctx.project_dir / "images"
    out_root.mkdir(parents=True, exist_ok=True)
    produced: list[str] = []

    for item in manifest.items:
        if item.strategy.value != "image_to_3d":
            # No reference image needed for text-to-3D.
            continue
        item_dir = out_root / item.id
        item_dir.mkdir(parents=True, exist_ok=True)
        try:
            results = client.generate(
                prompt=_compose_prompt(item),
                out_dir=item_dir,
                number_of_images=item.reference.number_of_images,
                aspect_ratio=item.reference.aspect_ratio,
                negative_prompt=item.reference.negative_prompt,
                filename_prefix=f"{item.id}_ref",
                prefer="imagen",
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

    ctx.store.record_stage(
        state,
        ProjectStage.reference_images,
        ProjectStatus.succeeded,
        message=f"{len(produced)} reference images",
        artifacts=produced,
    )
    ctx.emit(ProjectStage.reference_images, "succeeded", f"{len(produced)} images")
    return StageResult(stage=ProjectStage.reference_images, artifacts=produced)


def _compose_prompt(item) -> str:
    base = item.reference.prompt.strip()
    if item.style_notes:
        return f"{base}\nStyle: {item.style_notes.strip()}"
    return base
