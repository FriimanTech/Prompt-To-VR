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
    animation_extras = _animation_extras(ctx.project_dir)
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
        extras = animation_extras.get(item.id) or {}
        if extras.get("speeds"):
            # Measured/requested clip speeds already in engine metres per
            # second (Uthana path); the scene compiler prefers them over the
            # height-derived estimate.
            info["animation_speed_overrides"] = dict(extras["speeds"])
        if extras.get("provider"):
            info["animation_provider"] = extras["provider"]
        # ``skinned`` is only set later by the optimizer, so ask the file.
        if glb_rig_summary(glb)["num_skins"] > 0:
            sole = _bind_sole(glb)
            if sole is not None:
                info["bind_sole_y"] = sole
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


def _animation_extras(project_dir: Path) -> dict[str, dict[str, Any]]:
    """Per-asset provider and engine-space clip speeds from asset_animation.json."""
    path = project_dir / "asset_animation.json"
    if not path.is_file():
        return {}
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}
    out: dict[str, dict[str, Any]] = {}
    for asset_id, entry in (data.items() if isinstance(data, dict) else []):
        if not isinstance(entry, dict) or entry.get("status") != "animated":
            continue
        speeds = {}
        for key, rec in (entry.get("clip_speeds") or {}).items():
            if isinstance(rec, dict) and float(rec.get("engine_mps") or 0) > 0:
                speeds[str(key)] = float(rec["engine_mps"])
        out[str(asset_id)] = {"provider": entry.get("provider", "tripo"), "speeds": speeds}
    return out


def _bind_sole(glb: Path) -> float | None:
    """Lowest rest-pose vertex height of a rigged GLB, in GLB units.

    Taken through the skin (joint matrices x inverse bind), because the
    rendered rest pose -- not the raw POSITION data -- is where the feet are.
    """
    try:
        from .uthana.gltf_io import Glb, aabb, skinned_bind_positions

        lo, _ = aabb(skinned_bind_positions(Glb.load(glb), max_vertices=20000))
    except Exception:  # noqa: BLE001 - best effort; the engine falls back to h/2
        return None
    return float(lo[1])


def is_lod_output(path: Path) -> bool:
    """True for a GLB this pipeline generated by decimating another one.

    ``asset_optimization`` writes ``<asset_id>_lod0.glb`` .. ``_lod3.glb``
    beside the authored mesh, so anything matching that shape is an OUTPUT and
    must never be treated as a source. Feeding one back in re-decimates an
    already-decimated mesh, and because the decimator picks its skinned code
    path by probing the input for a skeleton, a single round trip through an
    un-rigged LOD strips the rig permanently.
    """
    stem = path.stem.lower()
    return "_lod" in stem


def pick_primary_glb(d: Path, *, exclude_lods: bool = False) -> Path | None:
    """The mesh that represents this asset, by stage precedence.

    Prefer *animated*, then *rigged*, then *pbr* / *model*; fall back to the
    first .glb.

    Order matters and is not alphabetical by accident. Each stage writes its
    output BESIDE the input rather than over it, so a re-run can start from
    the original: ``asset_rigging`` adds ``model_rigged.glb`` next to
    ``model.glb``, and ``asset_animation`` adds ``model_animated.glb`` next
    to that. On the *model* key alone plain "model.glb" sorts first, and the
    skeleton and clips would silently never reach the scene.

    ``exclude_lods`` skips this pipeline's own decimation output, which is what
    a stage wants when it is recovering the AUTHORED mesh rather than choosing
    what to ship.
    """
    if not d.exists():
        return None
    glbs = [p for p in d.rglob("*.glb") if p.is_file()]
    if exclude_lods:
        glbs = [p for p in glbs if not is_lod_output(p)]
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


def _pick_primary_glb(d: Path) -> Path | None:
    return pick_primary_glb(d)


def read_glb_json(path: Path) -> dict[str, Any] | None:
    """The glTF JSON chunk of a .glb, or ``None`` if it is not readable.

    Deliberately does not depend on tinygltf/pygltflib: this runs on every
    asset of every project, and answering "is this rigged" should not cost a
    parse of a 40 MB binary chunk. Shared with ``asset_optimization``, which
    has to know whether the mesh it is about to decimate carries a skeleton.
    """
    try:
        data = path.read_bytes()
    except OSError:
        return None
    if len(data) < 20:
        return None
    magic, _version, length = struct.unpack_from("<III", data, 0)
    if magic != GLB_MAGIC or length > len(data):
        return None
    chunk_len, chunk_type = struct.unpack_from("<II", data, 12)
    if chunk_type != 0x4E4F534A:  # 'JSON'
        return None
    try:
        doc = json.loads(data[20 : 20 + chunk_len])
    except (json.JSONDecodeError, UnicodeDecodeError):
        return None
    return doc if isinstance(doc, dict) else None


def glb_rig_summary(path: Path) -> dict[str, int]:
    """``num_skins`` / ``joint_count`` / ``animation_clips`` for one GLB.

    The rig is the one property of an asset that several stages can silently
    destroy -- decimation, remeshing and format conversion all drop skins --
    and whose loss is invisible until a mechanic refuses to pose the mesh in
    a headset. Cheap enough to measure at every hand-off, so we do.
    """
    doc = read_glb_json(path)
    if doc is None:
        return {"num_skins": 0, "joint_count": 0, "animation_clips": 0}
    skins = doc.get("skins") or []
    return {
        "num_skins": len(skins),
        "joint_count": sum(
            len(s.get("joints") or []) for s in skins if isinstance(s, dict)
        ),
        "animation_clips": len(doc.get("animations") or []),
    }


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
    skins = doc.get("skins") or []

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
        # Recorded on the mesh this stage actually picked, so a later stage
        # that reports ``skinned: false`` can be caught contradicting the
        # source instead of being believed.
        "num_skins": len(skins),
        "joint_count": sum(
            len(s.get("joints") or []) for s in skins if isinstance(s, dict)
        ),
        "animation_clips": len(doc.get("animations") or []),
        "byte_length": len(data),
    }
