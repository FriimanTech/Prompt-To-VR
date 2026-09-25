"""Stage: fit generated GLB assets into the device's triangle budget.

Raw Tripo output is routinely ~250k vertices / ~500k triangles PER ASSET. Meta
publishes a budget of 1.3M-1.8M triangles for an entire Quest 3 frame, so a
dozen untouched assets overshoot the whole-frame budget several times over --
which is what produces multi-second first-frame stalls (ANRs) and sub-target
frame rates on device.

This stage is the deterministic fix. It:

  1. derives a :class:`PerformanceProfile` from the target device and how much
     GLB content the project actually has (see ``ptv_schemas.performance``),
  2. allocates a per-asset LOD0 triangle budget weighted by importance, with a
     quality floor so a huge project degrades gracefully rather than turning
     small props into blobs,
  3. runs the ``ptv_glb_optimize`` tool to emit an LOD chain per asset, and
  4. records ``lod_paths`` / triangle counts / bounds back into
     ``asset_normalization.json`` for the scene compiler and runtime.

IMPORTANT: the pipeline never caps how many assets a prompt may ask for. More
content simply means a smaller per-asset budget (never below the floor) plus
more aggressive runtime culling/LOD -- so the same engine handles a one-room
game and a large open world.
"""

from __future__ import annotations

import json
import subprocess
from pathlib import Path
from typing import Any

from ptv_schemas import (
    AssetBudgetRequest,
    AssetCategory,
    AssetImportance,
    PerformanceProfile,
    ProjectStage,
    ProjectStatus,
    TargetDevice,
    allocate_asset_budgets,
    derive_profile,
)

from .context import PipelineContext, StageResult
from .stage_asset_normalize import (
    glb_rig_summary,
    is_lod_output,
    pick_primary_glb,
)

#: How far (as a fraction) an LOD's longest axis may drift from LOD0's before
#: it is rebuilt with the topology-preserving simplifier.
LOD_EXTENT_TOLERANCE = 0.02


def _decimation_source(info: dict[str, Any], repo_root: Path) -> str:
    """The mesh to decimate for one asset, never one of our own outputs.

    Earlier runs of this stage recorded ``glb_source`` by copying whatever was
    in ``glb_path`` at the time, which on a resume was already LOD0. Those
    reports are on disk in real projects, so trusting the field blindly would
    keep re-decimating a previous output. A path that appears in ``lod_paths``
    is by definition ours, so when both fields point at our own output we
    recover the authored mesh sitting in the asset directory instead.
    """
    lods = {str(p) for p in (info.get("lod_paths") or [])}

    def is_ours(rel: str) -> bool:
        # ``lod_paths`` is the precise answer, but a report written by an
        # interrupted run can carry a LOD in ``glb_path`` without the list.
        # The filename shape catches that too.
        return rel in lods or is_lod_output(Path(rel))

    candidate = info.get("glb_source")
    if candidate and not is_ours(str(candidate)):
        return str(candidate)
    path = str(info["glb_path"])
    if is_ours(path):
        recovered = _authored_glb_beside(path, repo_root)
        if recovered is not None:
            return recovered
    return path


def _authored_glb_beside(rel_lod_path: str, repo_root: Path) -> str | None:
    """The authored mesh in the same directory as a LOD file.

    Defers to the normalizer's own precedence rather than re-listing candidate
    filenames here, so a stage that has lost track of its input recovers
    exactly the mesh the normalizer would have handed it -- including layouts
    a hard-coded list would miss (``*_pbr.glb``, nested directories). LOD
    outputs are excluded, or the recovery could hand back the very file it is
    recovering from.
    """
    rel_dir = Path(rel_lod_path).parent
    found = pick_primary_glb(repo_root / rel_dir, exclude_lods=True)
    if found is None:
        return None
    try:
        return str(found.relative_to(repo_root)).replace("\\", "/")
    except ValueError:
        return str(rel_dir / found.name).replace("\\", "/")


def _source_fingerprint(source: Path, repo_root: Path) -> dict[str, Any]:
    """Identity of the mesh a LOD chain was built from.

    Path alone is not enough: ``asset_rigging`` and ``asset_animation`` write
    ``model_rigged.glb`` / ``model_animated.glb`` beside ``model.glb`` and the
    primary pick changes, while a re-rig of the same asset changes the bytes
    under an unchanged name.
    """
    try:
        st = source.stat()
        size, mtime_ns = st.st_size, st.st_mtime_ns
    except OSError:
        size, mtime_ns = 0, 0
    try:
        rel = str(source.relative_to(repo_root)).replace("\\", "/")
    except ValueError:
        rel = source.name
    return {"path": rel, "size": size, "mtime_ns": mtime_ns}


def run(ctx: PipelineContext) -> StageResult:
    stage = ProjectStage.asset_optimization
    ctx.emit(stage, "running", "Decimating GLB assets to the device budget")
    state = ctx.store.load_state(ctx.project_id)
    ctx.store.record_stage(state, stage, ProjectStatus.running)

    report_path = ctx.project_dir / "asset_normalization.json"
    if not report_path.exists():
        msg = "asset_normalization.json missing -- run asset_normalization first"
        ctx.store.record_stage(state, stage, ProjectStatus.failed, message=msg)
        ctx.emit(stage, "failed", msg)
        raise RuntimeError(msg)

    report: dict[str, dict[str, Any]] = json.loads(
        report_path.read_text(encoding="utf-8")
    )
    manifest = ctx.asset_manifest or ctx.store.load_asset_manifest(ctx.project_id)
    items = {item.id: item for item in manifest.items}

    target_device = getattr(state, "target_device", TargetDevice.pcvr)
    mesh_entity_count = _count_mesh_entities(ctx)
    usable = [
        asset_id
        for asset_id, info in report.items()
        if info.get("status") in ("ok", "warn_no_material") and info.get("glb_path")
    ]

    profile = derive_profile(
        target_device=target_device,
        asset_count=len(usable),
        mesh_entity_count=mesh_entity_count,
    )
    ctx.emit(
        stage,
        "info",
        (
            f"tier={profile.tier.value} device={target_device.value} "
            f"assets={len(usable)} mesh_entities={mesh_entity_count} "
            f"frame_budget={profile.frame_triangle_budget:,} tris"
        ),
    )

    # --- Allocate a triangle budget per asset ------------------------------
    requests: dict[str, AssetBudgetRequest] = {}
    for asset_id in usable:
        item = items.get(asset_id)
        info = report[asset_id]
        source_tris = info.get("source_triangles")
        requests[asset_id] = AssetBudgetRequest(
            importance=_importance_for(ctx, asset_id, item),
            target_scale_meters=(
                float(item.target_scale_meters) if item is not None else 1.0
            ),
            instance_count=max(1, _instance_count(ctx, asset_id)),
            source_triangles=(int(source_tris) if source_tris else None),
            max_triangles=(
                item.max_triangles if item is not None else None
            ),
        )
    budgets = allocate_asset_budgets(profile, requests)

    tool = _ensure_tool(ctx, stage)

    optimized = 0
    skipped = 0
    failed = 0
    total_src_tris = 0
    total_out_tris = 0

    for asset_id in usable:
        ctx.raise_if_cancelled()
        info = report[asset_id]
        # ``glb_source`` is the authored mesh; ``glb_path`` is whatever the last
        # run left behind, which on a resume is LOD0. Decimating that would
        # rebuild LOD0 from itself -- and because the tool's skinned path is
        # chosen by probing the INPUT for a skin, one such round trip turns a
        # rigged avatar into a static mesh permanently.
        source_rel = _decimation_source(info, ctx.settings.repo_root)
        src = ctx.settings.repo_root / source_rel
        if not src.exists():
            # Name the path and what IS in that directory. The bare
            # "missing_source" this used to record gave a reader nothing to
            # act on, and the usual cause -- a report pointing at a deleted
            # LOD -- is obvious the moment the directory listing is visible.
            present = sorted(
                p.name for p in src.parent.glob("*.glb")
            ) if src.parent.is_dir() else []
            reason = (
                f"source GLB not found: {source_rel}. GLBs present in that "
                f"directory: {present or 'none'}"
            )
            info["optimization"] = {
                "status": "missing_source",
                "reason": reason,
            }
            ctx.emit(stage, "warning", f"{asset_id}: {reason}")
            failed += 1
            continue

        budget = int(budgets.get(asset_id, profile.min_triangles_per_asset))
        try:
            lod_info = _build_lod_chain(
                tool=tool,
                source=src,
                asset_id=asset_id,
                budget=budget,
                profile=profile,
                repo_root=ctx.settings.repo_root,
                on_line=lambda m: ctx.emit(stage, "log", m[:400]),
            )
        except Exception as exc:  # noqa: BLE001 - report and keep going
            info["optimization"] = {"status": "failed", "reason": str(exc)}
            ctx.emit(stage, "log", f"{asset_id}: optimize failed: {exc}")
            failed += 1
            continue

        if lod_info.get("reused"):
            skipped += 1
        else:
            optimized += 1
        total_src_tris += int(lod_info.get("source_triangles") or 0)
        total_out_tris += int(lod_info.get("triangle_counts", [0])[0] or 0)

        # The scene compiler resolves meshes through ``glb_path``; point it at
        # LOD0 so an un-decimated 500k-triangle mesh can never reach the
        # headset. The untouched source stays on disk under ``glb_source``.
        #
        # Record the mesh we ACTUALLY decimated, which repairs the field as
        # well as setting it. A report written before this stage was
        # idempotent has ``glb_source`` pointing at LOD0; leaving that in
        # place means every future run has to re-derive the recovery, and any
        # other stage reading the field still gets a wrong answer.
        info["glb_source"] = source_rel
        info["glb_path"] = lod_info["lod_paths"][0]
        info["lod_paths"] = lod_info["lod_paths"]
        info["triangle_counts"] = lod_info["triangle_counts"]
        info["bounds_radius_m"] = lod_info["bounds_radius_m"]
        info["lod0_longest_axis"] = float(lod_info.get("lod0_longest_axis") or 0.0)
        info["triangle_budget"] = budget
        info["decimated"] = True
        info["skinned"] = bool(lod_info.get("skinned") or False)
        info["joint_count"] = int(lod_info.get("joint_count") or 0)
        info["optimization"] = {
            "status": "ok",
            "tier": profile.tier.value,
            "source_triangles": lod_info.get("source_triangles"),
        }

    # Persist the profile alongside the per-asset data so scene_compile (and
    # anyone debugging a build) can see exactly what budget was applied.
    report["__performance__"] = json.loads(profile.model_dump_json())
    report_path.write_text(
        json.dumps(report, indent=2, sort_keys=True), encoding="utf-8"
    )

    reduction = (
        f"{total_src_tris:,} -> {total_out_tris:,} tris"
        if total_src_tris
        else "no geometry change"
    )
    summary = (
        f"tier={profile.tier.value} optimized={optimized} reused={skipped} "
        f"failed={failed} ({reduction})"
    )
    rel = str(report_path.relative_to(ctx.settings.repo_root)).replace("\\", "/")
    ctx.store.record_stage(
        state, stage, ProjectStatus.succeeded, message=summary, artifacts=[rel]
    )
    ctx.emit(stage, "succeeded", summary)
    return StageResult(stage=stage, artifacts=[rel], data=report)


# ---------------------------------------------------------------------------
# LOD chain generation
# ---------------------------------------------------------------------------


def _build_lod_chain(
    *,
    tool: Path,
    source: Path,
    asset_id: str,
    budget: int,
    profile: PerformanceProfile,
    repo_root: Path,
    on_line,
) -> dict[str, Any]:
    """Emit LOD0..LODn for one asset and return the recorded metadata.

    Idempotent: if every LOD already exists and was built for the same budget,
    the files are reused so re-running the pipeline is cheap.
    """

    out_dir = source.parent
    stamp_path = out_dir / f"{asset_id}_lod.json"

    targets = [
        max(1, int(round(budget * ratio))) for ratio in profile.lod_ratios
    ]
    fingerprint = _source_fingerprint(source, repo_root)

    if stamp_path.exists():
        try:
            prev = json.loads(stamp_path.read_text(encoding="utf-8"))
        except Exception:  # noqa: BLE001 - corrupt stamp, just rebuild
            prev = {}
        # The source has to match, not just the targets. ``asset_rigging`` and
        # ``asset_animation`` REPLACE the primary GLB with a rigged/animated
        # one beside it, leaving the budget untouched -- so a targets-only
        # check reuses the chain built from the un-rigged mesh and the
        # skeleton never reaches the scene.
        # A stamp without ``lod0_longest_axis`` predates the size contract
        # and is rebuilt so every mesh ships with its measured unit scale.
        if (
            prev.get("targets") == targets
            and prev.get("source") == fingerprint
            and float(prev.get("lod0_longest_axis") or 0.0) > 0.0
            and all(
                (repo_root / p).exists() for p in prev.get("lod_paths", [])
            )
        ):
            prev["reused"] = True
            return prev

    lod_paths: list[str] = []
    triangle_counts: list[int] = []
    longest_axes: list[float] = []
    source_triangles = 0
    unit_radius = 0.0
    skinned = False
    joint_count = 0

    def run_level(level: int, target: int, sloppy: bool) -> dict[str, Any]:
        out_file = out_dir / f"{asset_id}_lod{level}.glb"
        report_file = out_dir / f"{asset_id}_lod{level}.report.json"
        args = [
            str(tool),
            "--input",
            str(source),
            "--output",
            str(out_file),
            "--target-tris",
            str(target),
            "--report",
            str(report_file),
        ]
        if sloppy:
            args.append("--sloppy")
        proc = subprocess.run(args, capture_output=True, text=True)
        if proc.returncode != 0:
            raise RuntimeError(
                f"ptv_glb_optimize failed for {asset_id} lod{level}: "
                f"{(proc.stderr or proc.stdout)[:300]}"
            )
        if proc.stdout:
            on_line(proc.stdout.strip().splitlines()[-1])
        rep = json.loads(report_file.read_text(encoding="utf-8"))
        report_file.unlink(missing_ok=True)
        return rep

    for level, target in enumerate(targets):
        out_file = out_dir / f"{asset_id}_lod{level}.glb"
        # The error-bounded simplifier preserves topology and can stall well
        # above very aggressive targets; allow the sloppy fallback from LOD1
        # down, where silhouette fidelity matters far less than hitting budget.
        #
        # Rigged assets are the exception, but we cannot know that before the
        # first invocation reports it -- and the tool refuses the flag on its
        # own regardless, because sloppy simplification invents vertex
        # positions and would detach the mesh from its skeleton. Skipping it
        # here once we do know just avoids a noisy log line per LOD.
        sloppy = level >= 1 and not skinned
        rep = run_level(level, target, sloppy)
        longest = float(rep.get("longest_axis") or 0.0)
        # Every LOD must be the same size as LOD0: the runtime scales them all
        # by LOD0's unit scale, so a sloppy LOD that lost an extremity would
        # visibly shrink or grow when the LOD switches.
        if (
            sloppy
            and longest_axes
            and longest_axes[0] > 0.0
            and abs(longest - longest_axes[0]) > LOD_EXTENT_TOLERANCE * longest_axes[0]
        ):
            on_line(
                f"{asset_id} lod{level}: longest axis {longest:.4f} vs LOD0"
                f" {longest_axes[0]:.4f}; rebuilding without --sloppy"
            )
            rep = run_level(level, target, False)
            longest = float(rep.get("longest_axis") or 0.0)
        longest_axes.append(longest)
        source_triangles = int(rep.get("source_triangles") or 0)
        triangle_counts.append(int(rep.get("output_triangles") or 0))
        unit_radius = float(rep.get("unit_bounds_radius") or 0.0)
        skinned = bool(rep.get("skinned") or False)
        joint_count = int(rep.get("joint_count") or 0)
        lod_paths.append(
            str(out_file.relative_to(repo_root)).replace("\\", "/")
        )

    # A rigged source that produced an un-rigged chain means the skeleton was
    # dropped somewhere in decimation. Recording that as ``skinned: false``
    # would be believed by every stage downstream: the scene compiler omits
    # ``mesh.skinned``, the loader attaches no CSkeleton, and builtin.avatar_body
    # / character_locomotion decline to pose the mesh -- a static statue where
    # an animated character should be, with every stage reporting success.
    rig = glb_rig_summary(source)
    if rig["num_skins"] > 0 and not skinned:
        raise RuntimeError(
            f"{asset_id}: source {source.name} has {rig['num_skins']} skin(s) "
            f"and {rig['joint_count']} joint(s), but the generated LOD chain "
            "has none -- the skeleton was stripped during decimation. Refusing "
            "to record an un-rigged chain for a rigged asset."
        )

    stamp = {
        "targets": targets,
        "source": fingerprint,
        "lod_paths": lod_paths,
        "triangle_counts": triangle_counts,
        "source_triangles": source_triangles,
        # ``unit_bounds_radius`` is for a mesh normalised to a 1 m longest
        # axis; the runtime rescales by target_scale_meters, so the world-space
        # cull radius scales the same way.
        "bounds_radius_m": unit_radius,
        # LOD0's longest axis in GLB units. scene_compile turns it into
        # ``mesh.unit_scale`` (metres per GLB unit) so every LOD, and every
        # launch, renders at exactly the manifest size instead of each file
        # being re-measured on its own.
        "lod0_longest_axis": longest_axes[0] if longest_axes else 0.0,
        "lod_longest_axes": longest_axes,
        # Every LOD of a rigged asset carries an identical skeleton by
        # construction, so one flag describes the whole chain. The scene
        # compiler forwards it so the runtime knows to attach a CSkeleton and
        # draw through the skinned pipeline.
        "skinned": skinned,
        "joint_count": joint_count,
        "reused": False,
    }
    stamp_path.write_text(json.dumps(stamp, indent=2), encoding="utf-8")
    return stamp


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _ensure_tool(ctx: PipelineContext, stage: ProjectStage) -> Path:
    """Locate ``ptv_glb_optimize``, building it once if necessary."""

    exe = "ptv_glb_optimize.exe" if _is_windows() else "ptv_glb_optimize"
    engine_build = ctx.settings.engine_dir / "build"
    if engine_build.exists():
        for candidate in engine_build.rglob(exe):
            return candidate

    ctx.emit(stage, "info", "Building ptv_glb_optimize (first run)")
    from ..build import configure_and_build

    build_dir = ctx.settings.engine_dir / "build" / "vs2026-x64"
    result = configure_and_build(
        source_dir=ctx.settings.engine_dir,
        build_dir=build_dir,
        generator=ctx.settings.cmake_generator,
        platform=ctx.settings.cmake_generator_platform,
        config=ctx.settings.cmake_build_config,
        vcpkg_root=ctx.settings.vcpkg_root,
        build_target="ptv_glb_optimize",
        on_line=lambda m: ctx.emit(stage, "log", m[:400]),
        **proc_hooks(ctx),
    )
    if result.exit_code != 0:
        raise RuntimeError(
            "could not build ptv_glb_optimize (see logs); asset optimization "
            "cannot run without it"
        )
    for candidate in (ctx.settings.engine_dir / "build").rglob(exe):
        return candidate
    raise RuntimeError("ptv_glb_optimize built but the executable was not found")


def _is_windows() -> bool:
    import sys

    return sys.platform == "win32"


def _importance_for(ctx: PipelineContext, asset_id: str, item) -> AssetImportance:
    """Derive how much budget an asset deserves when not explicitly authored.

    Characters and anything the player can pick up / interact with are the
    objects a VR player literally holds in front of their face, so they get
    the hero budget. Large scenery reads at a distance and gets less.
    """

    if item is not None and item.importance is not None:
        return item.importance

    if item is not None and item.category == AssetCategory.character:
        return AssetImportance.hero
    if _is_interactive(ctx, asset_id):
        return AssetImportance.hero
    if item is not None and item.category in (AssetCategory.fx, AssetCategory.ui):
        return AssetImportance.background
    if item is not None and item.target_scale_meters >= 4.0:
        # Big set-dressing (buildings, terrain chunks) is usually seen far away.
        return AssetImportance.background
    return AssetImportance.standard


def _scene_entities(ctx: PipelineContext) -> list[dict[str, Any]]:
    """Every entity across all compiled/spec'd scenes, best-effort.

    Runs BEFORE scene_compile, so read the per-scene SceneSpec artifacts the
    scene_spec stage wrote. Returns an empty list when they are unavailable
    (the budget then falls back to asset count alone).
    """

    entities: list[dict[str, Any]] = []
    for spec_file in sorted(ctx.project_dir.glob("scene_spec.*.json")):
        try:
            doc = json.loads(spec_file.read_text(encoding="utf-8"))
        except Exception:  # noqa: BLE001
            continue
        entities.extend(doc.get("entities") or [])
    return entities


def _count_mesh_entities(ctx: PipelineContext) -> int:
    """How many entities actually reference a GLB mesh."""
    return sum(1 for e in _scene_entities(ctx) if e.get("mesh"))


def _instance_count(ctx: PipelineContext, asset_id: str) -> int:
    """How many entities place this asset (shared mesh, many draw calls)."""
    count = 0
    for e in _scene_entities(ctx):
        mesh = e.get("mesh") or {}
        if mesh.get("asset_id") == asset_id:
            count += 1
    return count


def _is_interactive(ctx: PipelineContext, asset_id: str) -> bool:
    """True when any entity using this asset is grabbable / mechanic-driven."""
    for e in _scene_entities(ctx):
        mesh = e.get("mesh") or {}
        if mesh.get("asset_id") != asset_id:
            continue
        tags = {str(t).lower() for t in (e.get("tags") or [])}
        if tags & {"grabbable", "interactive", "held", "weapon", "tool"}:
            return True
        if e.get("mechanic_bindings"):
            return True
    return False
