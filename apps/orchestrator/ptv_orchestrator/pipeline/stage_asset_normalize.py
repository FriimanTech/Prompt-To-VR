"""Stage 7: deterministic GLB validation and normalization metadata."""

from __future__ import annotations

import json
import struct
from pathlib import Path
from typing import Any

from ptv_schemas import ProjectStage, ProjectStatus

from .context import PipelineContext, StageResult


GLB_MAGIC = 0x46546C67  # 'glTF'


def run(ctx: PipelineContext) -> StageResult:
    ctx.emit(ProjectStage.asset_normalization, "running", "Validating GLB outputs")
    state = ctx.store.load_state(ctx.project_id)
    ctx.store.record_stage(
        state, ProjectStage.asset_normalization, ProjectStatus.running
    )

    manifest = ctx.asset_manifest or ctx.store.load_asset_manifest(ctx.project_id)
    assets_dir = ctx.project_dir / "assets"
    report: dict[str, dict[str, Any]] = {}
    animation_map = _animation_clip_names(ctx.project_dir)
    ok = 0
    rejected = 0

    for item in manifest.items:
        item_dir = assets_dir / item.id
        glb = _pick_primary_glb(item_dir)
        if glb is None:
            report[item.id] = {
                "status": "missing",
                "reason": "no GLB file found",
            }
            rejected += 1
            continue
        info = _inspect_glb(glb)
        info["target_scale_meters"] = item.target_scale_meters
        info["collision_shape"] = item.collision_shape
        info["glb_path"] = str(glb.relative_to(ctx.settings.repo_root)).replace("\\", "/")
        # Semantic intent -> real glTF clip name, from the animation stage.
        # Rides on this report so it reaches the scene compiler the same way
        # ``skinned`` and ``lod_paths`` do, with no extra plumbing.
        clips = animation_map.get(item.id)
        if clips:
            info["animations"] = dict(clips)
        if info["valid"] and info["has_any_material"]:
            info["status"] = "ok"
            ok += 1
        elif info["valid"]:
            info["status"] = "warn_no_material"
            ok += 1
        else:
            info["status"] = "invalid"
            rejected += 1
        report[item.id] = info

    out = ctx.project_dir / "asset_normalization.json"
    out.write_text(json.dumps(report, indent=2, sort_keys=True), encoding="utf-8")
    ctx.store.record_stage(
        state,
        ProjectStage.asset_normalization,
        ProjectStatus.succeeded,
        message=f"{ok} ok, {rejected} rejected",
        artifacts=[
            str(out.relative_to(ctx.settings.repo_root)).replace("\\", "/"),
        ],
    )
    ctx.emit(
        ProjectStage.asset_normalization,
        "succeeded",
        f"ok={ok} rejected={rejected}",
    )
    return StageResult(
        stage=ProjectStage.asset_normalization,
        artifacts=[str(out.relative_to(ctx.settings.repo_root)).replace("\\", "/")],
        data=report,
    )


def _animation_clip_names(project_dir: Path) -> dict[str, dict[str, str]]:
    """Per-asset intent -> clip-name maps recorded by ``asset_animation``.

    Empty for projects with no animated assets, and for anything generated
    before that stage existed.
    """

    path = project_dir / "asset_animation.json"
    if not path.is_file():
        return {}
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}
    if not isinstance(data, dict):
        return {}

    out: dict[str, dict[str, str]] = {}
    for asset_id, entry in data.items():
        if not isinstance(entry, dict) or entry.get("status") != "animated":
            continue
        clips = entry.get("animations")
        if isinstance(clips, dict) and clips:
            out[str(asset_id)] = {str(k): str(v) for k, v in clips.items()}
    return out


def _pick_primary_glb(d: Path) -> Path | None:
    if not d.exists():
        return None
    # Prefer *animated*, then *rigged*, then *pbr* / *model*; fall back to the
    # first .glb.
    #
    # Order matters and is not alphabetical by accident. Each stage writes its
    # output BESIDE the input rather than over it, so a re-run can start from
    # the original: ``asset_rigging`` adds ``model_rigged.glb`` next to
    # ``model.glb``, and ``asset_animation`` adds ``model_animated.glb`` next
    # to that. On the *model* key alone plain "model.glb" sorts first, and the
    # skeleton and clips would silently never reach the scene.
    glbs = [p for p in d.rglob("*.glb") if p.is_file()]
    if not glbs:
        return None
    glbs.sort(
        key=lambda p: (
            0 if "animated" in p.name.lower() else 1,
            0 if "rigged" in p.name.lower() else 1,
            0 if "pbr" in p.name.lower() else 1,
            0 if "model" in p.name.lower() else 1,
            p.name.lower(),
        )
    )
    return glbs[0]


def _inspect_glb(path: Path) -> dict[str, Any]:
    """Light GLB validation: parse header + the first JSON chunk.

    We deliberately do NOT depend on tinygltf/pygltflib here; this runs often.
    """
    try:
        data = path.read_bytes()
    except OSError as e:
        return {"valid": False, "reason": f"read-error: {e}"}
    if len(data) < 20:
        return {"valid": False, "reason": "file too small"}
    magic, version, length = struct.unpack_from("<III", data, 0)
    if magic != GLB_MAGIC:
        return {"valid": False, "reason": "bad magic"}
    if length > len(data):
        return {"valid": False, "reason": "length mismatch"}

    # First chunk header.
    chunk_len, chunk_type = struct.unpack_from("<II", data, 12)
    if chunk_type != 0x4E4F534A:  # 'JSON'
        return {"valid": False, "reason": "first chunk not JSON"}
    json_bytes = data[20 : 20 + chunk_len]
    try:
        doc = json.loads(json_bytes)
    except Exception as e:
        return {"valid": False, "reason": f"json parse: {e}"}

    materials = doc.get("materials") or []
    textures = doc.get("textures") or []
    meshes = doc.get("meshes") or []
    primitives_total = sum(len(m.get("primitives") or []) for m in meshes)

    return {
        "valid": True,
        "gltf_version": doc.get("asset", {}).get("version"),
        "generator": doc.get("asset", {}).get("generator"),
        "num_meshes": len(meshes),
        "num_primitives": primitives_total,
        "num_materials": len(materials),
        "num_textures": len(textures),
        "has_any_material": len(materials) > 0,
        "has_any_texture": len(textures) > 0,
        "byte_length": len(data),
    }
