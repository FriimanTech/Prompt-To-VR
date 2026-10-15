"""Stage 5: per-asset reference images via Google GenAI (Imagen + Gemini).

Idempotency: if an asset's ``images/<asset_id>/`` directory already
contains at least one ``.png``/``.jpg``/``.jpeg``/``.webp`` file, the
asset is treated as already-imaged and skipped. This means a Resume
after a partial run does not re-pay for Imagen calls that were already
made. Delete an asset's image directory to force regeneration.
"""

from __future__ import annotations

import json
from pathlib import Path

from ptv_providers import GoogleImageClient, GoogleImageConfig
from ptv_schemas import ProjectStage, ProjectStatus
from ptv_schemas.asset_manifest import (
    CharacterRole,
    ReferenceCoverage,
    ReferencePose,
)

from .context import PipelineContext, StageResult


_IMAGE_EXTS = (".png", ".jpg", ".jpeg", ".webp")


def run(ctx: PipelineContext) -> StageResult:
    ctx.emit(ProjectStage.reference_images, "running", "Generating reference images")
    state = ctx.store.load_state(ctx.project_id)
    ctx.store.record_stage(state, ProjectStage.reference_images, ProjectStatus.running)

    manifest = ctx.asset_manifest or ctx.store.load_asset_manifest(ctx.project_id)
    # npc_plan runs at stage 3, well before this one, so the actor decision is
    # already on disk and does not have to be guessed from prose.
    npc_asset_ids = _npc_asset_ids(ctx)

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
        ctx.raise_if_cancelled()
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

        pose = resolve_pose(item, npc_asset_ids)
        if pose != ReferencePose.natural:
            ctx.emit(
                ProjectStage.reference_images,
                "info",
                f"{item.id}: {pose.value} "
                f"({item.reference.coverage.value})",
            )

        prompt = compose_prompt(item, pose)
        # Kept beside the image: character routing reads the exact wording
        # Gemini was given when it judges a character's body shape.
        (item_dir / "reference_prompt.txt").write_text(prompt, encoding="utf-8")
        try:
            results = _client().generate(
                prompt=prompt,
                out_dir=item_dir,
                number_of_images=item.reference.number_of_images,
                aspect_ratio=item.reference.aspect_ratio,
                negative_prompt=compose_negative_prompt(item, pose),
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


def resolve_pose(item, npc_asset_ids: set[str]) -> ReferencePose:
    """Decide the pose from facts, not from what an agent remembered.

    The agent's own ``pose`` is treated as a SHAPE hint only -- it is good at
    "this creature is a quadruped" and unreliable at "and therefore the rule
    says T-pose". The decision itself is made here, from two things already on
    disk by the time this stage runs: the asset's ``character_role`` and the
    NpcPlan (stage 3, well before reference images at stage 6).

    Order matters:

    * A player avatar gets a character pose. NpcPlan deliberately ignores it,
      since it plans NON-player characters, so nothing else would catch it.
    * Anything the NPC director planned gets a character pose, INCLUDING a
      ``slide_no_animation`` NPC. The stated goal is reusing these GLBs in
      other 3D software, and that wants a T-pose whether or not this
      particular game animates the asset.
    * Everything else stays natural -- including a ``category: character``
      duck that the game only uses as a grabbable prop.
    """

    is_character_actor = (
        item.character_role == CharacterRole.player_avatar
        or item.character_role == CharacterRole.npc
        or item.id in npc_asset_ids
    )
    if not is_character_actor:
        return ReferencePose.natural

    # Keep the agent's shape hint when it identified a non-humanoid, since a
    # T-pose is meaningless for something with four legs.
    if item.reference.pose == ReferencePose.neutral_quadruped:
        return ReferencePose.neutral_quadruped
    return ReferencePose.t_pose


def _npc_asset_ids(ctx: PipelineContext) -> set[str]:
    """Asset ids the NPC director planned as actors, or empty."""
    path = ctx.project_dir / "npc_plan.json"
    if not path.exists():
        return set()
    try:
        doc = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        # Best-effort enrichment: a missing or malformed plan must not fail
        # the image stage, it just means fewer assets are known to be actors.
        return set()
    out: set[str] = set()
    for npc in doc.get("npcs") or []:
        asset_id = npc.get("asset_id")
        if isinstance(asset_id, str):
            out.add(asset_id)
    return out


# Wording kept close to what actually produced usable T-pose art in practice,
# rather than invented fresh: the phrasing below is what the duck and Red
# Lantern reference run used successfully.
_T_POSE_CLAUSE = (
    "Pose: strict T-pose. Both arms stretched straight out to the sides at "
    "shoulder height, elbows straight, legs straight and slightly apart, "
    "standing upright, facing the camera directly, orthographic front view, "
    "full body in frame, isolated on a clean solid neutral background."
)
_T_POSE_NEGATIVES = (
    "sitting, crouching, walking, running, flying, action pose, A-pose, "
    "arms down, crossed arms, hands on hips, side view, three-quarter view, "
    "perspective distortion, cropped limbs, multiple characters"
)

_QUADRUPED_CLAUSE = (
    "Pose: neutral standing reference. All four legs straight and clearly "
    "separated, body level, head forward and level, full body in frame, "
    "strict side-on orthographic view, isolated on a clean solid neutral "
    "background."
)
_QUADRUPED_NEGATIVES = (
    "sitting, lying down, rearing, jumping, running, curled up, legs "
    "overlapping, front view, perspective distortion, cropped limbs, "
    "multiple animals"
)

_NECK_DOWN_CLAUSE = (
    "Show the outfit and body from the feet up to the neck ONLY. No head, no "
    "face, no hair, no helmet. The neck ends in a clean flat opening."
)
_NECK_DOWN_NEGATIVES = "head, face, hair, helmet, mask, neck stump detail"


def compose_prompt(item, pose: ReferencePose) -> str:
    """Base prompt plus the deterministic pose and coverage clauses.

    The clause is appended rather than substituted so the agent's own art
    direction survives; it reinforces the base prompt instead of replacing it.
    """

    parts = [item.reference.prompt.strip()]
    if item.style_notes:
        parts.append(f"Style: {item.style_notes.strip()}")

    if pose == ReferencePose.t_pose:
        parts.append(_T_POSE_CLAUSE)
    elif pose == ReferencePose.neutral_quadruped:
        parts.append(_QUADRUPED_CLAUSE)

    # Coverage only means anything for a character being posed; a prop has no
    # neck to cut at.
    if (
        pose != ReferencePose.natural
        and item.reference.coverage == ReferenceCoverage.neck_down
    ):
        parts.append(_NECK_DOWN_CLAUSE)

    return "\n".join(parts)


def compose_negative_prompt(item, pose: ReferencePose) -> str | None:
    """Fold the pose's own avoid-list into the asset's negative prompt.

    Gemini image models have no separate negative-prompt field, so the
    provider client folds this into the text; keeping it as one string here
    means it is folded exactly once.
    """

    parts: list[str] = []
    if item.reference.negative_prompt:
        parts.append(item.reference.negative_prompt.strip())
    if pose == ReferencePose.t_pose:
        parts.append(_T_POSE_NEGATIVES)
    elif pose == ReferencePose.neutral_quadruped:
        parts.append(_QUADRUPED_NEGATIVES)
    if (
        pose != ReferencePose.natural
        and item.reference.coverage == ReferenceCoverage.neck_down
    ):
        parts.append(_NECK_DOWN_NEGATIVES)
    return ", ".join(p for p in parts if p) or None


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
