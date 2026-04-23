"""Stage 6: Tripo image-to-3D (or text-to-3D fallback) per asset."""

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
    client = TripoClient(
        TripoConfig(
            api_key=ctx.settings.tripo_api_key,
            model_version=ctx.settings.tripo_model_version or None,
        )
    )

    produced: list[str] = []
    attempted = 0
    per_item_errors: list[str] = []
    for item in manifest.items:
        item_dir = ctx.project_dir / "assets" / item.id
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

    # Hard-fail if the manifest wanted 3D assets but none materialised. Without
    # this the pipeline would silently steamroll past the Tripo stage,
    # normalize 0 GLBs, and the C++ build would then compile against an empty
    # scene. Fail loudly so the UI can show the error and the user can fix
    # credits / API keys and resume the pipeline from this point.
    if attempted > 0 and not produced:
        detail = "; ".join(per_item_errors[:6]) if per_item_errors else "unknown"
        msg = (
            f"Tripo produced 0 models from {attempted} asset(s). "
            f"Check TRIPO_API_KEY and account credits. First errors: {detail}"
        )
        raise RuntimeError(msg)

    ctx.store.record_stage(
        state,
        ProjectStage.tripo_models,
        ProjectStatus.succeeded,
        message=f"{len(produced)} models produced",
        artifacts=produced,
    )
    ctx.emit(ProjectStage.tripo_models, "succeeded", f"{len(produced)} models")
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
