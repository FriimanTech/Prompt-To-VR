"""Stage 8a: Uthana auto-rig (with finger joints) for routed characters.

Rigs the player's avatar body and every humanoid NPC that
``character_routing`` sent to Uthana, BEFORE Tripo's rigging stage -- so a
character whose Uthana rig fails or is not trusted falls back to Tripo in
the same run.

Per character:

1. Upload the un-rigged Tripo ``model.glb`` with the five auto-rig flags
   (``auto_rig``, ``front_facing``, ``include_fingers``,
   ``auto_rig_strip_namespace``, ``auto_rig_feet_on_floor``).
2. Download the rigged GLB into the asset's own folder as
   ``model_uthana_rigged.glb`` (Uthana keeps rigged characters server-side;
   nothing is on disk until it is downloaded).
3. Record the ``character_id`` (Part 3 retargets every motion onto it) and
   ``auto_rig_confidence`` in ``uthana_rig.json``.
4. Validate the skeleton (humanoid joints, finger chains, no namespace).
5. Normalise the origin to the character's vertical middle -- the engine's
   contract. Uthana returns the FIRST character for a repeated upload, with
   that upload's flags, so ``feet_on_floor`` may or may not have taken effect
   and is never assumed.

Fallback: confidence at or below the threshold (0.20 by default) or a failed
upload/validation sends the avatar to Tripo (rig + walk/run, no finger curls)
with a loud warning, and an NPC to Tripo when Tripo's rig-check said it is
riggable, otherwise to no rig.
"""

from __future__ import annotations

from pathlib import Path
from typing import Any, Optional

from ptv_providers import AutoRigFlags, UthanaClient, UthanaClientError
from ptv_schemas import CharacterRoute, ProjectStage, ProjectStatus, RigProvider

from ..fsutil import move_with_retry
from .character_artifacts import (
    UTHANA_RIG_NAME,
    UTHANA_RIGGED_GLB,
    load_json,
    load_routing,
    manifest_asset_ids,
    prune_to_assets,
    rel,
    save_json,
    save_routing,
    sha256_file,
)
from .context import PipelineContext, StageResult
from .stage_asset_rigging import _source_glb
from .stage_character_routing import uthana_available
from .uthana.glb_measure import measure
from .uthana.gltf_io import Glb, GltfError
from .uthana.rig_edit import shift_hips_world

_STAGE = ProjectStage.uthana_rigging

#: Joints a humanoid needs for the avatar body and locomotion.
REQUIRED_JOINTS = (
    "hips", "spine", "head",
    "leftshoulder", "rightshoulder", "leftarm", "rightarm",
    "leftforearm", "rightforearm", "lefthand", "righthand",
    "leftupleg", "rightupleg", "leftleg", "rightleg", "leftfoot", "rightfoot",
)
FINGER_PREFIXES = ("handindex", "handmiddle", "handring", "handpinky", "handthumb")

#: Origin is "at the middle" within this tolerance (metres, GLB units).
ORIGIN_TOLERANCE_M = 0.03


def run(ctx: PipelineContext) -> StageResult:
    return run_with(ctx)


def run_with(ctx: PipelineContext, *, client: Any = None) -> StageResult:
    ctx.emit(_STAGE, "running", "Uthana auto-rig")
    state = ctx.store.load_state(ctx.project_id)
    ctx.store.record_stage(state, _STAGE, ProjectStatus.running)

    routing = load_routing(ctx.project_dir)
    targets = [
        r for r in (routing.routes if routing else [])
        if r.rig_provider == RigProvider.uthana
    ]
    if not targets:
        message = "No characters routed to Uthana"
        ctx.store.record_stage(state, _STAGE, ProjectStatus.succeeded, message=message)
        ctx.emit(_STAGE, "succeeded", message)
        return StageResult(stage=_STAGE)

    on, reason = uthana_available(ctx)
    if not on and client is None:
        for r in targets:
            _fall_back(ctx, r, f"Uthana unavailable ({reason})")
        save_routing(ctx.project_dir, routing)  # type: ignore[arg-type]
        message = f"Uthana disabled; {len(targets)} character(s) fell back"
        ctx.store.record_stage(state, _STAGE, ProjectStatus.succeeded, message=message)
        ctx.emit(_STAGE, "succeeded", message)
        return StageResult(stage=_STAGE)

    ledger_path = ctx.project_dir / UTHANA_RIG_NAME
    ledger = load_json(ledger_path)
    produced: list[str] = []
    rigged = reused = fell_back = 0

    for route in targets:
        ctx.raise_if_cancelled()
        item_dir = ctx.project_dir / "assets" / route.asset_id
        final = item_dir / UTHANA_RIGGED_GLB
        source = _source_glb(item_dir)
        if source is None:
            _fall_back(ctx, route, "no Tripo mesh on disk to upload")
            fell_back += 1
            continue
        digest = sha256_file(source)
        prior = ledger.get(route.asset_id)
        if (
            isinstance(prior, dict)
            and prior.get("status") == "rigged"
            and prior.get("source_sha256") == digest
            and final.is_file()
        ):
            route.fallback = None
            route.fallback_reason = ""
            supersede_tripo_outputs(ctx, route.asset_id)
            produced.append(rel(ctx.settings.repo_root, final))
            reused += 1
            continue

        try:
            if client is None:
                client = UthanaClient(ctx.settings.uthana_api_key)
            entry = _rig_one(ctx, client, route, source, final)
        except (UthanaClientError, GltfError, OSError) as exc:
            final.unlink(missing_ok=True)
            ledger[route.asset_id] = {"status": "failed", "error": str(exc)[:600],
                                      "source_sha256": digest}
            _fall_back(ctx, route, f"Uthana rig failed: {exc}")
            fell_back += 1
            continue

        entry["source_sha256"] = digest
        entry["source_glb"] = rel(ctx.settings.repo_root, source)
        ledger[route.asset_id] = entry
        if entry["status"] != "rigged":
            final.unlink(missing_ok=True)
            _fall_back(ctx, route, entry.get("reason", "Uthana rig not trusted"))
            fell_back += 1
            continue
        route.fallback = None
        route.fallback_reason = ""
        supersede_tripo_outputs(ctx, route.asset_id)
        produced.append(rel(ctx.settings.repo_root, final))
        rigged += 1
        ctx.emit(_STAGE, "info",
                 f"{route.asset_id}: rigged by Uthana (confidence "
                 f"{entry.get('auto_rig_confidence')}, character_id {entry['character_id']})")

    prune_to_assets(ledger, manifest_asset_ids(ctx))
    save_json(ledger_path, ledger)
    save_routing(ctx.project_dir, routing)  # type: ignore[arg-type]
    _sync_manifest(ctx, routing)

    parts = [f"{rigged} rigged"]
    if reused:
        parts.append(f"{reused} reused")
    if fell_back:
        parts.append(f"{fell_back} fell back")
    message = ", ".join(parts)
    ctx.store.record_stage(state, _STAGE, ProjectStatus.succeeded, message=message,
                           artifacts=produced)
    ctx.emit(_STAGE, "succeeded", message)
    return StageResult(stage=_STAGE, artifacts=produced)


# ---------------------------------------------------------------------------


def _rig_one(
    ctx: PipelineContext, client: Any, route: CharacterRoute, source: Path, final: Path
) -> dict[str, Any]:
    name = f"{ctx.project_id}__{route.asset_id}"[:120]
    ctx.emit(_STAGE, "info", f"{route.asset_id}: uploading to Uthana for auto-rig")
    created = client.create_character(source, name, AutoRigFlags())
    entry: dict[str, Any] = {
        "character_id": created.character_id,
        "auto_rig_confidence": created.auto_rig_confidence,
        "params": {
            "auto_rig": True,
            "auto_rig_front_facing": True,
            "include_fingers": True,
            "auto_rig_strip_namespace": True,
            "auto_rig_feet_on_floor": True,
        },
        "character_role": route.character_role,
    }
    threshold = (
        ctx.settings.uthana_avatar_min_confidence
        if route.character_role == "player_avatar"
        else ctx.settings.uthana_npc_min_confidence
    )
    conf = created.auto_rig_confidence
    if conf is not None and conf <= threshold:
        entry.update(status="low_confidence",
                     reason=f"auto_rig_confidence {conf:.2f} is at or below {threshold:.2f}")
        return entry

    staging = final.with_suffix(".download")
    staging.write_bytes(client.download_character(created.character_id))
    move_with_retry(staging, final)

    glb = Glb.load(final)
    check = validate_rig(glb)
    entry["joints"] = check["joints"]
    entry["finger_joints"] = check["fingers"]
    entry["warnings"] = check["warnings"]
    if check["missing"]:
        entry.update(status="invalid_rig",
                     reason="missing humanoid joints: " + ", ".join(check["missing"]))
        return entry
    for w in check["warnings"]:
        ctx.emit(_STAGE, "warning", f"{route.asset_id}: {w}")

    entry["origin_shift_m"] = normalise_origin(final)
    entry["rigged_glb"] = rel(ctx.settings.repo_root, final)
    entry["status"] = "rigged"
    return entry


def validate_rig(glb: Glb) -> dict[str, Any]:
    names = [str(glb.nodes[i].get("name") or "") for i in sorted(glb.skin_joint_set())]
    bare = {n.rsplit(":", 1)[-1].lower() for n in names}
    missing = [j for j in REQUIRED_JOINTS if j not in bare]
    fingers = sorted(n for n in names if any(p in n.lower() for p in FINGER_PREFIXES))
    warnings: list[str] = []
    if any(":" in n for n in names):
        warnings.append("joint names still carry a namespace prefix "
                        "(a de-duplicated upload kept its first flags); the engine strips it")
    if not fingers:
        warnings.append("no finger joints; the hands will not curl")
    if not any("toe" in b for b in bare):
        warnings.append("no toe joints; foot placement checks use the foot joints")
    return {"joints": names, "missing": missing, "fingers": fingers, "warnings": warnings}


def normalise_origin(path: Path) -> float:
    """Shift the rest pose so the bind mesh is vertically centred on the origin.

    Returns the world shift applied (0.0 when it was already centred).
    """
    report = measure(path)
    centre = report.world_center_y_m
    if abs(centre) <= ORIGIN_TOLERANCE_M:
        return 0.0
    glb = Glb.load(path)
    shift_hips_world(glb, -centre)
    glb.save(path)
    return -centre


def supersede_tripo_outputs(ctx: PipelineContext, asset_id: str) -> list[str]:
    """One provider per asset: retire an earlier run's Tripo rig/animation.

    ``pick_primary_glb`` prefers "animated" then "rigged" files, so a stale
    Tripo ``model_animated.glb`` beside the new Uthana rig would win and mix
    two skeletons. The files are moved (not deleted) to ``.superseded/`` and
    their ledger entries dropped, so a fallback later can still recover them.
    """
    item_dir = ctx.project_dir / "assets" / asset_id
    moved: list[str] = []
    for name in ("model_rigged.glb", "model_animated.glb"):
        p = item_dir / name
        if not p.is_file():
            continue
        dest_dir = item_dir / ".superseded"
        dest_dir.mkdir(exist_ok=True)
        move_with_retry(p, dest_dir / name)
        moved.append(name)
    for report_name in ("asset_rig.json", "asset_animation.json"):
        path = ctx.project_dir / report_name
        report = load_json(path)
        entry = report.get(asset_id)
        if isinstance(entry, dict) and entry.get("provider", "tripo") == "tripo":
            report.pop(asset_id, None)
            save_json(path, report)
    if moved:
        ctx.emit(_STAGE, "info", f"{asset_id}: retired earlier Tripo output ({', '.join(moved)})")
    return moved


def _fall_back(ctx: PipelineContext, route: CharacterRoute, reason: str) -> None:
    riggable = bool(route.tripo_rig_check and route.tripo_rig_check.riggable)
    if route.character_role == "player_avatar" or riggable:
        route.fallback = RigProvider.tripo
    else:
        route.fallback = RigProvider.none
    route.fallback_reason = reason[:600]
    final = ctx.project_dir / "assets" / route.asset_id / UTHANA_RIGGED_GLB
    final.unlink(missing_ok=True)
    loud = (
        f"{route.asset_id}: {reason}. Falling back to "
        f"{route.fallback.value}"
        + (" -- the avatar's hands will NOT curl" if route.character_role == "player_avatar" else "")
    )
    ctx.emit(_STAGE, "warning", loud[:400])
    ctx.store.append_log(ctx.project_id, "uthana_rigging", loud)


def _sync_manifest(ctx: PipelineContext, routing: Optional[Any]) -> None:
    if routing is None:
        return
    manifest = ctx.asset_manifest or ctx.store.load_asset_manifest(ctx.project_id)
    changed = False
    for item in manifest.items:
        route = routing.route_for(item.id)
        if route is None or route.fallback != RigProvider.none:
            continue
        if item.rigged:
            item.rigged = False
            changed = True
    if changed:
        ctx.store.save_asset_manifest(ctx.project_id, manifest)
        ctx.asset_manifest = manifest
