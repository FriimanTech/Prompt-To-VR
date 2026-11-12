"""Stage 5: per-asset reference images via Google GenAI (Imagen + Gemini).

Idempotency: if an asset's ``images/<asset_id>/`` directory already
contains at least one ``.png``/``.jpg``/``.jpeg``/``.webp`` file, the
asset is treated as already-imaged and skipped. This means a Resume
after a partial run does not re-pay for Imagen calls that were already
made. Delete an asset's image directory to force regeneration.
"""

from __future__ import annotations

import json
import shutil
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

    vision = None
    vision_checked = False

    def _vision():
        """Gemini vision for the hand check, or None (no key / disabled)."""
        nonlocal vision, vision_checked
        if not vision_checked:
            vision_checked = True
            if ctx.settings.hand_qa_enabled and ctx.settings.google_api_key:
                from ptv_providers.gemini_vision_client import (
                    GeminiVisionClient,
                    GeminiVisionConfig,
                )

                candidate = GeminiVisionClient(GeminiVisionConfig(
                    api_key=ctx.settings.google_api_key,
                    model=ctx.settings.socket_vision_model))
                vision = candidate if candidate.is_available() else None
            if vision is None:
                ctx.emit(ProjectStage.reference_images, "info",
                         "hand check unavailable (no Google key or disabled); "
                         "character images are not checked for five fingers")
        return vision

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
        size = image_size_for(item, pose, ctx.settings)
        try:
            if pose == ReferencePose.t_pose and _vision() is not None:
                results = _generate_with_hand_qa(
                    ctx, item, item_dir, prompt, compose_negative_prompt(item, pose),
                    size, _client(), _vision())
            else:
                results = _client().generate(
                    prompt=prompt,
                    out_dir=item_dir,
                    number_of_images=item.reference.number_of_images,
                    aspect_ratio=item.reference.aspect_ratio,
                    negative_prompt=compose_negative_prompt(item, pose),
                    filename_prefix=f"{item.id}_ref",
                    prefer="gemini",
                    image_size=size,
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


def image_size_for(item, pose: ReferencePose, settings) -> str | None:
    """2K for a posed character, 4K for the player's own avatar (its hands
    are inches from the player's eyes), the model default for the rest."""
    if pose == ReferencePose.natural:
        return None
    if item.character_role == CharacterRole.player_avatar:
        return settings.reference_avatar_image_size or None
    return settings.reference_character_image_size or None


def _generate_with_hand_qa(ctx: PipelineContext, item, item_dir: Path, prompt: str,
                           negative: str | None, size: str | None, client, vision) -> list:
    """Generate, check the hands, regenerate with the critique up to
    ``hand_qa_max_retries`` times; keep the first image that passes, else the
    best one with a loud warning. Rejected attempts stay under ``attempts/``
    (``_pick_reference_image`` only reads the top level)."""
    from . import hand_qa

    tries = 1 + max(0, int(ctx.settings.hand_qa_max_retries))
    attempts: list[tuple[hand_qa.HandReport, Path]] = []
    critique = ""
    for n in range(tries):
        ctx.raise_if_cancelled()
        text = prompt if n == 0 else prompt + "\n" + hand_qa.retry_clause(critique)
        out = client.generate(
            prompt=text, out_dir=item_dir / "attempts" / f"try{n}", number_of_images=1,
            aspect_ratio=item.reference.aspect_ratio, negative_prompt=negative,
            filename_prefix=f"{item.id}_ref", prefer="gemini", image_size=size)
        report = hand_qa.check_image(vision, out[0].path)
        attempts.append((report, out[0].path))
        ctx.emit(ProjectStage.reference_images, "info",
                 f"{item.id}: hand check {n + 1}/{tries}: "
                 + ("pass" if report.passed else f"score {report.score:.2f}")
                 + (f" ({report.error})" if report.error else "")
                 + (f" -- {report.critique[:120]}" if not report.passed and report.critique else ""))
        if report.passed:
            break
        if report.error and not report.hands:
            break  # the check itself failed; another image will not fix that
        critique = report.critique
    pick = hand_qa.best([a for a, _ in attempts])
    chosen_report, chosen = attempts[pick]
    final = item_dir / f"{item.id}_ref_00{chosen.suffix}"
    shutil.copyfile(chosen, final)
    (item_dir / "hand_qa.json").write_text(json.dumps({
        "asset_id": item.id, "image_size": size, "chosen": pick,
        "passed": chosen_report.passed,
        "attempts": [dict(a.as_dict(), image=str(p.relative_to(item_dir)).replace("\\", "/"))
                     for a, p in attempts],
    }, indent=2), encoding="utf-8")
    if not chosen_report.passed:
        msg = (f"HANDS: {item.id}: no reference image passed the hand check after "
               f"{len(attempts)} attempt(s); keeping the best (score "
               f"{chosen_report.score:.2f}). {chosen_report.critique[:200]}")
        ctx.emit(ProjectStage.reference_images, "warning", msg[:300])
        ctx.store.append_log(ctx.project_id, "reference_images", msg)
    from types import SimpleNamespace

    return [SimpleNamespace(path=final)]


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
    "full body in frame, isolated on a clean solid neutral background.\n"
    "Hands: both hands fully in frame with a clear margin around them, open "
    "flat with the palms facing the camera, all five fingers (thumb included) "
    "extended and spread apart with clearly visible gaps between every "
    "finger. Bare hands or full-finger gloves only -- every finger covered to "
    "its tip and individually shaped."
)
_T_POSE_NEGATIVES = (
    "sitting, crouching, walking, running, flying, action pose, A-pose, "
    "arms down, crossed arms, hands on hips, side view, three-quarter view, "
    "perspective distortion, cropped limbs, multiple characters, "
    "fingerless gloves, mittens, fists, clenched hands, fingers pressed "
    "together, fused fingers, hidden hands, cropped hands, hands out of frame"
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
