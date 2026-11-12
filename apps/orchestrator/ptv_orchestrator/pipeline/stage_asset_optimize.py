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

Colour survives decimation. Before the LOD chain is built, every base-colour
image of every asset is listed and extracted (``ptv_glb_optimize
--list-images``), deduplicated by content hash into
``assets/ptv_textures/``, sized by ``texture_policy`` (the detail the player can
see from the closest point they can reach, capped by importance and fitted to
the profile's texture memory budget) and compressed by
``ptv_texture_compress`` into an ASTC KTX2 with mips plus a resized PNG/JPEG
for PC VR. Each LOD references those shared files (``--textures``), so a
texture used by four LODs and two assets is stored and uploaded once. A
source that had textured triangles and a LOD that has none fails the asset
loudly; a source that was never textured is reported as such, not as an
error.

IMPORTANT: the pipeline never caps how many assets a prompt may ask for. More
content simply means a smaller per-asset budget (never below the floor) plus
more aggressive runtime culling/LOD -- so the same engine handles a one-room
game and a large open world.
"""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
from pathlib import Path
from typing import Any, Optional

from ptv_schemas import (
    AssetBudgetRequest,
    AssetCategory,
    AssetImportance,
    PerformanceProfile,
    ProjectStage,
    ProjectStatus,
    TargetDevice,
    allocate_asset_budgets,
    apply_overrides,
    derive_profile,
)

from . import texture_policy as tp
from .collision_shapes import try_measure as try_measure_collision
from .context import PipelineContext, StageResult
from .proc import proc_hooks
from .stage_asset_normalize import (
    glb_rig_summary,
    is_lod_output,
    pick_primary_glb,
)

#: Bumped whenever the texture processing changes in a way that invalidates
#: previously written KTX2 / resized images.
TEXTURE_PIPELINE_VERSION = 1
#: Shared, hash-named texture files: ``projects/<id>/assets/ptv_textures/``.
TEXTURE_DIR_NAME = "ptv_textures"

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


def _memory_fit(ctx: PipelineContext, stage: ProjectStage, profile: PerformanceProfile,
                budgets: dict[str, int], requests: dict[str, AssetBudgetRequest],
                prefs) -> dict[str, Any]:
    """Planned memory for this game and, when even the always-resident set
    would not fit the target, the corrections (texture caps, background LOD
    budgets, a user's render scale high) applied to ``profile`` / ``budgets``
    in place. Never removes content. Quest only."""
    from . import memory_model as mm

    if profile.target_device != TargetDevice.quest:
        return {"outcome": "not_applicable (PC VR)"}
    cls_of = {"hero": "hero", "standard": "standard", "background": "background"}
    inp = mm.MemoryInputs(render_scale_ceiling=profile.render_scale_ceiling,
                          frames_in_flight=profile.frames_in_flight,
                          max_fx_quads=profile.max_fx_quads,
                          max_live_projectiles=profile.max_live_projectiles,
                          max_decals=profile.max_decals,
                          animated_agents=profile.max_animated_agents,
                          entities=max(1, _count_mesh_entities(ctx)) * 3,
                          bodies=max(1, _count_mesh_entities(ctx)) * 2,
                          largest_glb_bytes=64 * 1024 * 1024)
    for asset_id, req in requests.items():
        cls = cls_of.get(req.importance.value, "standard")
        size = min(int(profile.texture_max_size.get(cls, 1024)), 2048)
        inp.textures.append(mm.TextureCost(key=asset_id, bytes=tp.astc_bytes(size), size=size,
                                           importance=cls))
        lod0 = int(budgets.get(asset_id, profile.min_triangles_per_asset))
        inp.meshes.append(mm.MeshCost(
            key=asset_id, importance=cls,
            lod_bytes=[int(lod0 * r * mm.BYTES_PER_TRI_SKINNED) for r in profile.lod_ratios]))
    locks = profile.locks
    est, notes = mm.plan_corrections(
        inp, baseline_mb=profile.engine_baseline_pss_mb, target_mb=profile.memory_target_mb,
        ceiling_mb=profile.memory_ceiling_mb,
        calibration=mm.load_calibration(ctx.settings.projects_dir / "memory_calibration.json"),
        resolution_high_locked=locks.resolution.value == "high",
        texture_high_locked=locks.texture_quality.value == "high")
    if notes:
        # Apply what the plan decided to the real build inputs.
        for t in inp.textures:
            if t.size:
                cls = t.importance
                profile.texture_max_size[cls] = min(profile.texture_max_size.get(cls, t.size),
                                                    max(t.size, 256))
        for m in inp.meshes:
            if m.importance == "background" and m.key in budgets:
                budgets[m.key] = max(profile.min_triangles_per_asset // 2,
                                     int(budgets[m.key] * 0.6))
        profile.render_scale_ceiling = min(profile.render_scale_ceiling,
                                           inp.render_scale_ceiling)
        for n in notes:
            ctx.emit(stage, "warning", f"memory: {n}")
    ctx.emit(stage, "warning" if est["outcome"] == "over" else "info",
             f"memory plan: steady {est['steady_mb']:.0f} MB, peak {est['peak_mb']:.0f} MB "
             f"(target {profile.memory_target_mb:.0f}, ceiling {profile.memory_ceiling_mb:.0f}, "
             f"Meta limit {profile.memory_limit_mb:.0f}) -- {est['outcome']}"
             + (" -- the APK is still built and flagged at risk" if est["outcome"] == "over"
                else ""))
    return {"planned": est, "compromises": notes}


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
    # The user's own choices (optimization_preferences) go on top of the
    # automatic profile as locks the runtime governor never moves.
    from .stage_optimization_preferences import load_preferences

    prefs = load_preferences(ctx.project_dir)
    if prefs is not None and prefs.settings():
        profile, override_notes = apply_overrides(profile, prefs.to_locks())
        for note in override_notes:
            ctx.emit(stage, "info", f"user override: {note}")
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
    # A projectile mesh is drawn many times at once and for a fraction of a
    # second each: it gets the profile's projectile cap, never hero detail.
    design = ctx.interaction_design or ctx.store.load_interaction_design(ctx.project_id)
    projectile_assets = {p.asset_id for p in (design.projectiles if design else [])
                         if p.asset_id}
    requests: dict[str, AssetBudgetRequest] = {}
    for asset_id in usable:
        item = items.get(asset_id)
        info = report[asset_id]
        source_tris = info.get("source_triangles")
        cap = item.max_triangles if item is not None else None
        if asset_id in projectile_assets:
            cap = min(cap or profile.projectile_max_triangles, profile.projectile_max_triangles)
        requests[asset_id] = AssetBudgetRequest(
            importance=_importance_for(ctx, asset_id, item),
            target_scale_meters=(
                float(item.target_scale_meters) if item is not None else 1.0
            ),
            instance_count=max(1, _instance_count(ctx, asset_id)),
            source_triangles=(int(source_tris) if source_tris else None),
            max_triangles=cap,
        )
    budgets = allocate_asset_budgets(profile, requests)

    # Memory fit (Quest 3 PSS): planned sizes against the engine's 4.5 GiB
    # target, corrected least-visible-first before anything is built.
    memory_plan = _memory_fit(ctx, stage, profile, budgets, requests, prefs)

    tool = _ensure_tool(ctx, stage)

    # --- Textures: list, size, fit the budget, compress (shared by hash) ---
    tex_plan = _plan_textures(ctx, stage, tool, usable, report, items, profile,
                              projectile_assets)

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
        plan = tex_plan.get(asset_id) or {}
        try:
            lod_info = _build_lod_chain(
                tool=tool,
                source=src,
                asset_id=asset_id,
                budget=budget,
                profile=profile,
                repo_root=ctx.settings.repo_root,
                on_line=lambda m: ctx.emit(stage, "log", m[:400]),
                drop_triangles=_engine_drawn_parts(ctx, asset_id),
                textures_map=plan.get("map_path"),
                textures_fingerprint=plan.get("fingerprint", ""),
            )
        except Exception as exc:  # noqa: BLE001 - report and keep going
            info["optimization"] = {"status": "failed", "reason": str(exc)}
            ctx.emit(stage, "warning", f"{asset_id}: optimize failed: {exc}")
            failed += 1
            continue

        # Colour must survive: a textured source whose LOD came out
        # untextured is a pipeline bug, never something to ship silently.
        tex_info = dict(plan.get("info") or {})
        src_textured = int(tex_info.get("source_textured_triangles") or 0)
        lod_textured = list(lod_info.get("textured_triangles") or [])
        if src_textured > 0 and (not lod_textured or min(lod_textured) == 0):
            reason = (f"source has {src_textured} textured triangles but LOD textured "
                      f"triangle counts are {lod_textured}: the base-colour texture was "
                      "lost during decimation (stage: asset_optimization)")
            info["optimization"] = {"status": "failed", "reason": reason}
            ctx.emit(stage, "warning", f"{asset_id}: {reason}")
            failed += 1
            continue
        tex_info["lod_textured_triangles"] = lod_textured
        # Render source and LOD0/LOD1 from the same cameras and compare the
        # colours: a texture that is bound but mapped wrong, a surface gone
        # white, or a magenta diagnostic all show up here before any headset.
        check = _compare_lods(ctx, stage, src, lod_info, asset_id)
        if check:
            tex_info["visual_check"] = check
            if check.get("status") == "failed":
                reason = f"visual comparison failed: {check.get('reason')}"
                info["optimization"] = {"status": "failed", "reason": reason}
                info["textures"] = tex_info
                ctx.emit(stage, "warning", f"{asset_id}: {reason}")
                failed += 1
                continue
        info["textures"] = tex_info

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
        info["lod_errors_rel"] = lod_info.get("lod_errors_rel") or []
        info["decimated"] = True
        info["skinned"] = bool(lod_info.get("skinned") or False)
        info["joint_count"] = int(lod_info.get("joint_count") or 0)
        # Characters collide as capsules; everything else gets a collider
        # measured from its own mesh (see collision_shapes).
        if not info["skinned"]:
            collision = try_measure_collision(
                info["lod_paths"], info["lod_errors_rel"], ctx.settings.repo_root)
            if collision is not None:
                info["collision"] = collision
            else:
                info.pop("collision", None)
                ctx.emit(stage, "warning",
                         f"{asset_id}: could not measure a collider from its LODs")
        info["optimization"] = {
            "status": "ok",
            "tier": profile.tier.value,
            "source_triangles": lod_info.get("source_triangles"),
        }

    # Persist the profile alongside the per-asset data so scene_compile (and
    # anyone debugging a build) can see exactly what budget was applied.
    report["__performance__"] = json.loads(profile.model_dump_json())
    report["__memory__"] = memory_plan
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


def _engine_drawn_parts(ctx: PipelineContext, asset_id: str) -> Path | None:
    """Triangles of a part the engine draws itself and the mesh must lose: a
    modelled bowstring (builtin.bow draws a string that bends). From the
    socket_generation region recorded in asset_contract.json."""
    path = ctx.project_dir / "asset_contract.json"
    if not path.exists():
        return None
    try:
        entry = json.loads(path.read_text(encoding="utf-8")).get(asset_id) or {}
    except json.JSONDecodeError:
        return None
    region = (entry.get("part_regions") or {}).get("bowstring")
    if not region:
        return None
    p = Path(region)
    p = p if p.is_absolute() else ctx.settings.repo_root / p
    return p if p.exists() else None


def _build_lod_chain(
    *,
    tool: Path,
    source: Path,
    asset_id: str,
    budget: int,
    profile: PerformanceProfile,
    repo_root: Path,
    on_line,
    drop_triangles: Path | None = None,
    textures_map: Path | None = None,
    textures_fingerprint: str = "",
) -> dict[str, Any]:
    """Emit LOD0..LODn for one asset and return the recorded metadata.

    Idempotent: if every LOD already exists and was built for the same budget,
    the same source and the same textures, the files are reused so re-running
    the pipeline is cheap.
    """

    out_dir = source.parent
    stamp_path = out_dir / f"{asset_id}_lod.json"

    targets = [
        max(1, int(round(budget * ratio))) for ratio in profile.lod_ratios
    ]
    fingerprint = _source_fingerprint(source, repo_root)
    if drop_triangles is not None:
        fingerprint = f"{fingerprint}|drop:{drop_triangles.stat().st_size}"
    fingerprint = f"{fingerprint}|tex:{TEXTURE_PIPELINE_VERSION}:{textures_fingerprint}"

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
            and "textured_triangles" in prev
            and all(
                (repo_root / p).exists() for p in prev.get("lod_paths", [])
            )
        ):
            prev["reused"] = True
            return prev

    lod_paths: list[str] = []
    triangle_counts: list[int] = []
    textured_counts: list[int] = []
    lod_errors: list[float] = []
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
        if drop_triangles is not None:
            args += ["--drop-triangles", str(drop_triangles)]
        if textures_map is not None:
            args += ["--textures", str(textures_map)]
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
        # above very aggressive targets; allow the sloppy fallback from LOD2
        # down only. Sloppy simplification ignores UV seams, so on LOD1 --
        # still seen at mid range -- it would smear the texture; at LOD2+
        # distances that is far less visible than missing the budget.
        #
        # Rigged assets are the exception, but we cannot know that before the
        # first invocation reports it -- and the tool refuses the flag on its
        # own regardless, because sloppy simplification invents vertex
        # positions and would detach the mesh from its skeleton. Skipping it
        # here once we do know just avoids a noisy log line per LOD.
        sloppy = level >= 2 and not skinned
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
        textured_counts.append(int(rep.get("textured_triangles") or 0))
        lod_errors.append(float(rep.get("simplify_error_rel") or 0.0))
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
    # The same failure for clips: an animated character (a Uthana merge can
    # carry idle + several gaits) whose LODs lost clips would silently stop
    # animating at a distance, or everywhere.
    if rig["animation_clips"] > 0:
        for p in lod_paths:
            lod_rig = glb_rig_summary(repo_root / p)
            if lod_rig["animation_clips"] != rig["animation_clips"]:
                raise RuntimeError(
                    f"{asset_id}: source {source.name} has {rig['animation_clips']} "
                    f"animation clip(s) but {Path(p).name} has "
                    f"{lod_rig['animation_clips']} -- clips were dropped during "
                    "decimation."
                )

    stamp = {
        "targets": targets,
        "source": fingerprint,
        "lod_paths": lod_paths,
        "triangle_counts": triangle_counts,
        "textured_triangles": textured_counts,
        # meshopt's simplification error per LOD, relative to the mesh extent:
        # times the asset's size in metres it is the largest geometric
        # deviation, which the runtime projects to pixels to pick a LOD.
        "lod_errors_rel": lod_errors,
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
# Textures
# ---------------------------------------------------------------------------


def _list_images(tool: Path, source: Path, extract_dir: Path) -> dict[str, Any]:
    """``ptv_glb_optimize --list-images`` for one source, extracting each
    base-colour image to ``extract_dir/<hash>.<ext>``."""
    proc = subprocess.run(
        [str(tool), "--input", str(source), "--list-images", "--extract-images",
         str(extract_dir)],
        capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(f"--list-images failed for {source.name}: "
                           f"{(proc.stderr or proc.stdout)[:300]}")
    out = proc.stdout
    start = out.find("{")
    if start < 0:
        raise RuntimeError(f"--list-images printed no JSON for {source.name}")
    return json.loads(out[start:])


def _class_rank(cls: str) -> int:
    """How demanding a texture class is (higher keeps more detail)."""
    order = ["projectile", "background", "standard", "ui", "hero", "held"]
    return order.index(cls) if cls in order else 2


def _texture_class(ctx: PipelineContext, asset_id: str, item,
                   importance: AssetImportance, projectile_assets: set[str]) -> str:
    if asset_id in projectile_assets:
        return "projectile"
    if item is not None and item.category == AssetCategory.ui:
        return "ui"
    if _is_held_by_player(ctx, asset_id) or _is_player_worn(ctx, asset_id):
        return "held"
    return importance.value


def _is_held_by_player(ctx: PipelineContext, asset_id: str) -> bool:
    """Something the player holds in a hand: the interaction design gave it a
    hold profile, or the scene tags it grabbable / held."""
    design = ctx.interaction_design or ctx.store.load_interaction_design(ctx.project_id)
    for it in (design.items if design else []):
        if it.asset_id == asset_id and it.hold is not None and str(
                getattr(it.hold.kind, "value", it.hold.kind)) != "none":
            return True
    for e in _scene_entities(ctx):
        mesh = e.get("mesh") or {}
        if mesh.get("asset_id") != asset_id:
            continue
        tags = {str(t).lower() for t in (e.get("tags") or [])}
        if tags & {"grabbable", "held", "weapon", "tool"}:
            return True
    return False


def _is_player_worn(ctx: PipelineContext, asset_id: str) -> bool:
    for e in _scene_entities(ctx):
        mesh = e.get("mesh") or {}
        if mesh.get("asset_id") != asset_id:
            continue
        tags = {str(t).lower() for t in (e.get("tags") or [])}
        if tags & {"player_avatar", "player", "worn", "held", "player_hands"}:
            return True
    return False


def _walkable_regions(ctx: PipelineContext) -> list[tuple[float, float, float, float]]:
    """Walkable rectangles (min_x, min_z, max_x, max_z) the world declared."""
    regions: list[tuple[float, float, float, float]] = []
    path = ctx.project_dir / "world.scene.json"
    try:
        world = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return regions
    for r in ((world.get("navigation") or {}).get("regions") or []):
        lo, hi = r.get("min") or [], r.get("max") or []
        if len(lo) >= 3 and len(hi) >= 3:
            regions.append((float(lo[0]), float(lo[2]), float(hi[0]), float(hi[2])))
    return regions


def _closest_view_m(ctx: PipelineContext, asset_id: str, item, tex_class: str,
                    regions: list[tuple[float, float, float, float]]) -> float:
    """Closest distance the player's eyes can get to this asset (metres)."""
    if tex_class == "held":
        return 0.4
    if tex_class == "ui":
        return 0.6
    if item is not None and item.category == AssetCategory.character:
        # NPCs walk up to the player (melee, followers, conversations).
        return 0.9
    size = float(item.target_scale_meters) if item is not None else 1.0
    best: Optional[float] = None
    for e in _scene_entities(ctx):
        mesh = e.get("mesh") or {}
        if mesh.get("asset_id") != asset_id:
            continue
        pos = ((e.get("transform") or {}).get("position")) or e.get("position")
        if isinstance(pos, dict):
            if "x" not in pos or "z" not in pos:
                continue
            x, z = float(pos["x"]), float(pos["z"])
        elif isinstance(pos, (list, tuple)) and len(pos) >= 3:
            x, z = float(pos[0]), float(pos[2])
        else:
            continue
        if regions:
            d = min(max(0.0, max(r[0] - x, x - r[2], 0.0) ** 2 +
                         max(r[1] - z, z - r[3], 0.0) ** 2) ** 0.5 for r in regions)
        else:
            d = 1.0
        d = max(0.3, d - 0.5 * size)
        best = d if best is None else min(best, d)
    return best if best is not None else 2.0


def _ensure_texture_tool(ctx: PipelineContext, stage: ProjectStage) -> Optional[Path]:
    """``ptv_texture_compress`` if it exists (or can be built), else None --
    textures then ship as their original PNG/JPEG and the headset decodes
    them, which works but costs load time and ~4x the GPU memory."""
    exe = "ptv_texture_compress.exe" if _is_windows() else "ptv_texture_compress"
    build_root = ctx.settings.engine_dir / "build"
    if build_root.exists():
        found = sorted(build_root.rglob(exe), key=lambda p: p.stat().st_mtime)
        if found:
            return found[-1]
    ctx.emit(stage, "info", "Building ptv_texture_compress (first run)")
    try:
        from ..build import configure_and_build

        result = configure_and_build(
            source_dir=ctx.settings.engine_dir,
            build_dir=ctx.settings.engine_dir / "build" / "vs2026-x64",
            generator=ctx.settings.cmake_generator,
            platform=ctx.settings.cmake_generator_platform,
            config=ctx.settings.cmake_build_config,
            vcpkg_root=ctx.settings.vcpkg_root,
            build_target="ptv_texture_compress",
            on_line=lambda m: ctx.emit(stage, "log", m[:400]),
            **proc_hooks(ctx),
        )
        if result.exit_code == 0:
            found = sorted(build_root.rglob(exe), key=lambda p: p.stat().st_mtime)
            if found:
                return found[-1]
    except Exception as exc:  # noqa: BLE001 - reported below
        ctx.emit(stage, "log", f"ptv_texture_compress build failed: {exc}")
    return None


def _plan_textures(ctx: PipelineContext, stage: ProjectStage, tool: Path,
                   usable: list[str], report: dict[str, dict[str, Any]], items: dict,
                   profile: PerformanceProfile,
                   projectile_assets: set[str]) -> dict[str, dict[str, Any]]:
    """List, size, budget and compress every base-colour image.

    Returns asset_id -> {"map_path": Path | None, "fingerprint": str,
    "info": report section}. Assets whose source has no texture get an
    ``untextured_source`` status instead of an error.
    """
    repo = ctx.settings.repo_root
    # No leading underscore: Android's aapt drops asset directories named
    # "_*" from the APK by default (ignoreAssetsPattern "<dir>_*"), so an
    # "_textures" folder packaged fine and then did not exist on the headset.
    tex_dir = ctx.project_dir / "assets" / TEXTURE_DIR_NAME
    src_dir = tex_dir / "src"
    src_dir.mkdir(parents=True, exist_ok=True)
    regions = _walkable_regions(ctx)
    # The profile's ASTC block for 3D textures (6x6; 4x4 with the user's
    # texture quality "high"). UI always uses 4x4.
    try:
        world_block = max(4, min(12, int(str(profile.texture_astc_block).lower().split("x")[0])))
    except ValueError:
        world_block = 6
    listings: dict[str, dict[str, Any]] = {}
    needs: dict[str, tp.TextureNeed] = {}
    files: dict[str, Path] = {}
    mimes: dict[str, str] = {}
    out: dict[str, dict[str, Any]] = {}

    for asset_id in usable:
        info = report[asset_id]
        src = repo / _decimation_source(info, repo)
        if not src.exists():
            continue
        try:
            listing = _list_images(tool, src, src_dir)
        except Exception as exc:  # noqa: BLE001 - recorded on the asset
            out[asset_id] = {"info": {"status": "list_failed", "reason": str(exc)}}
            ctx.emit(stage, "warning", f"{asset_id}: {exc}")
            continue
        listings[asset_id] = listing
        item = items.get(asset_id)
        importance = _importance_for(ctx, asset_id, item)
        cls = _texture_class(ctx, asset_id, item, importance, projectile_assets)
        closest = _closest_view_m(ctx, asset_id, item, cls, regions)
        size_m = float(item.target_scale_meters) if item is not None else 1.0
        for img in listing.get("images") or []:
            if not img.get("hash") or int(img.get("textured_triangles") or 0) == 0:
                continue
            if not img.get("decodable"):
                ctx.emit(stage, "warning",
                         f"{asset_id}: base-colour image {img.get('name') or img['hash']} "
                         f"({img.get('mime')}) cannot be decoded; it is shipped as is")
            h = img["hash"]
            files[h] = Path(img["file"]) if img.get("file") else src_dir / f"{h}.{img.get('ext')}"
            mimes[h] = str(img.get("mime") or "image/png")
            n = needs.get(h)
            if n is None:
                n = tp.TextureNeed(hash=h,
                                   source_size=max(int(img.get("width") or 0),
                                                   int(img.get("height") or 0)),
                                   importance=cls, size_m=size_m, closest_view_m=closest,
                                   block=4 if cls == "ui" else world_block)
                needs[h] = n
            else:
                # Shared image: the most demanding user decides.
                if _class_rank(cls) > _class_rank(n.importance):
                    n.importance = cls
                n.closest_view_m = min(n.closest_view_m, closest)
                n.size_m = max(n.size_m, size_m)
                if cls == "ui":
                    n.block = 4
            n.users.append(asset_id)

    need_list = list(needs.values())
    tp.choose_sizes(need_list, profile.texture_max_size)
    budget = int(profile.texture_memory_budget_mb * 1024 * 1024)
    reductions = tp.fit_budget(need_list, budget)
    total = tp.total_bytes(need_list)
    ctx.emit(stage, "info",
             f"textures: {len(need_list)} unique base-colour image(s), "
             f"{total / 1048576.0:.1f} MB of {profile.texture_memory_budget_mb:.0f} MB budget"
             + (f"; {len(reductions)} reduction(s) to fit" if reductions else ""))
    for note in reductions[:20]:
        ctx.emit(stage, "info", f"textures: reduced {note}")

    compress = _ensure_texture_tool(ctx, stage) if need_list else None
    if need_list and compress is None:
        ctx.emit(stage, "warning",
                 "ptv_texture_compress unavailable (vcpkg port 'ktx' missing?): textures "
                 "ship as their original PNG/JPEG and are decoded on the headset -- "
                 "slower to load and about 4x the GPU memory of ASTC")

    results: dict[str, dict[str, Any]] = {}
    for n in need_list:
        src_file = files[n.hash]
        ext = "png" if mimes[n.hash] == "image/png" else "jpg"
        b = f"{n.block}x{n.block}"
        ktx2 = tex_dir / f"{n.hash}_{n.chosen}_astc{b}.ktx2"
        image = tex_dir / f"{n.hash}_{n.chosen}.{ext}"
        rep_file = tex_dir / f"{n.hash}_{n.chosen}_astc{b}.json"
        entry: dict[str, Any] = {"hash": n.hash, "users": n.users,
                                 "class": n.importance, "closest_view_m": round(n.closest_view_m, 2),
                                 "source_size": n.source_size, "size": n.chosen,
                                 "block": b, "reductions": n.reductions,
                                 "gpu_bytes_estimate": n.bytes_at(n.chosen)}
        if compress is not None:
            fresh = ktx2.exists() and image.exists() and rep_file.exists()
            if fresh:
                try:
                    prev = json.loads(rep_file.read_text(encoding="utf-8"))
                    fresh = int(prev.get("pipeline_version") or 0) == TEXTURE_PIPELINE_VERSION
                except (OSError, ValueError):
                    fresh = False
            if not fresh:
                proc = subprocess.run(
                    [str(compress), "--input", str(src_file), "--max-size", str(n.chosen),
                     "--block", b, "--out-ktx2", str(ktx2), "--out-image", str(image),
                     "--report", str(rep_file)],
                    capture_output=True, text=True)
                if proc.returncode != 0:
                    entry["error"] = (proc.stderr or proc.stdout)[:300]
                    ctx.emit(stage, "warning",
                             f"textures: compressing {n.hash[:8]} failed: {entry['error']}")
                else:
                    if proc.stdout:
                        ctx.emit(stage, "log", proc.stdout.strip().splitlines()[-1][:400])
                    rep = json.loads(rep_file.read_text(encoding="utf-8"))
                    rep["pipeline_version"] = TEXTURE_PIPELINE_VERSION
                    rep_file.write_text(json.dumps(rep, indent=2), encoding="utf-8")
            if "error" not in entry and rep_file.exists():
                rep = json.loads(rep_file.read_text(encoding="utf-8"))
                entry.update({"ktx2": ktx2, "image": Path(rep.get("image") or image),
                              "gpu_bytes": int(rep.get("gpu_bytes") or 0),
                              "width": rep.get("width"), "height": rep.get("height"),
                              "levels": rep.get("levels"), "has_alpha": rep.get("has_alpha")})
                if rep.get("has_alpha") and str(entry["image"]).endswith(".jpg"):
                    entry["image"] = Path(str(entry["image"])[:-4] + ".png")
        if "image" not in entry:
            # No compressor (or it failed): ship the original image as is.
            fallback = tex_dir / f"{n.hash}.{ext}"
            if not fallback.exists() and src_file.exists():
                shutil.copy2(src_file, fallback)
            entry["image"] = fallback
            entry["ktx2"] = None
        results[n.hash] = entry

    for asset_id, listing in listings.items():
        info = report[asset_id]
        src = repo / _decimation_source(info, repo)
        out_dir = src.parent
        images = {}
        tex_records = []
        for img in listing.get("images") or []:
            h = img.get("hash")
            if not h or h not in results:
                continue
            r = results[h]
            rel_img = os.path.relpath(r["image"], out_dir).replace("\\", "/")
            rel_ktx = (os.path.relpath(r["ktx2"], out_dir).replace("\\", "/")
                       if r.get("ktx2") else "")
            images[h] = {"uri": rel_img, "ktx2": rel_ktx,
                         "mime": "image/png" if rel_img.endswith(".png") else "image/jpeg"}
            tex_records.append({
                "hash": h, "class": r["class"], "closest_view_m": r["closest_view_m"],
                "source_size": r["source_size"], "size": r["size"], "block": r["block"],
                "ktx2": str(r["ktx2"].relative_to(repo)).replace("\\", "/") if r.get("ktx2") else None,
                "image": str(Path(r["image"]).relative_to(repo)).replace("\\", "/"),
                "gpu_bytes": r.get("gpu_bytes") or r["gpu_bytes_estimate"],
                "reductions": r["reductions"], "error": r.get("error"),
                "shared_with": [u for u in r["users"] if u != asset_id]})
        source_textured = int(listing.get("textured_triangles") or 0)
        tex_info: dict[str, Any] = {
            "status": ("textured" if source_textured > 0 else "untextured_source"),
            "source_textured_triangles": source_textured,
            "source_triangles": int(listing.get("triangles") or 0),
            "materials": len(listing.get("materials") or []),
            "missing_uvs": bool(listing.get("missing_uvs")),
            "vertex_colors": bool(listing.get("had_vertex_colors")),
            "source_maps_kept_on_disk": listing.get("other_maps") or {},
            "images": tex_records,
        }
        if source_textured == 0:
            ctx.emit(stage, "info",
                     f"{asset_id}: source GLB has no base-colour texture; its colour comes "
                     "from material factors / vertex colours (reported, not an error)")
        if listing.get("missing_uvs"):
            ctx.emit(stage, "warning",
                     f"{asset_id}: a textured material has no TEXCOORD; that surface "
                     "renders with its factor colour only")
        map_path = None
        fingerprint = ""
        if images:
            map_path = out_dir / f"{asset_id}_textures.json"
            doc = {"images": images}
            map_path.write_text(json.dumps(doc, indent=2, sort_keys=True), encoding="utf-8")
            fingerprint = hashlib.sha1(json.dumps(doc, sort_keys=True).encode()).hexdigest()[:16]
        out[asset_id] = {"map_path": map_path, "fingerprint": fingerprint, "info": tex_info}
    return out


#: Visual comparison thresholds (source vs LOD rendered from the same views).
#: Mean colour difference is CIE76 delta E over the object's pixels; a value
#: around 2-3 is just noticeable, the limits leave room for the lower detail.
VISUAL_MAX_MEAN_DELTA_E = {0: 6.0, 1: 10.0}
VISUAL_MAX_WHITE_GAIN = 0.05
VISUAL_MAX_MAGENTA = 0.01


def _compare_lods(ctx: PipelineContext, stage: ProjectStage, source: Path,
                  lod_info: dict[str, Any], asset_id: str) -> dict[str, Any]:
    """``ptv_glb_views --compare`` for LOD0 and LOD1 against the source.

    Cached next to the LOD chain under the chain's own fingerprint, so a reused
    chain is not re-rendered. Missing tool -> no check (reported as skipped)."""
    views_tool = _newest_build_tool(ctx, "ptv_glb_views")
    if views_tool is None:
        return {"status": "skipped", "reason": "ptv_glb_views not built"}
    lods = list(lod_info.get("lod_paths") or [])[:2]
    if not lods:
        return {}
    repo = ctx.settings.repo_root
    out_root = source.parent / "visual_check"
    stamp = out_root / "stamp.json"
    key = json.dumps({"source": lod_info.get("source"), "lods": lods,
                      "targets": lod_info.get("targets")}, sort_keys=True)
    if stamp.exists():
        try:
            prev = json.loads(stamp.read_text(encoding="utf-8"))
            if prev.get("key") == key and prev.get("result"):
                return prev["result"]
        except (OSError, ValueError):
            pass
    result: dict[str, Any] = {"status": "passed", "lods": []}
    for level, rel in enumerate(lods):
        out_dir = out_root / f"lod{level}"
        proc = subprocess.run([str(views_tool), "--input", str(source), "--compare",
                               str(repo / rel), "--out-dir", str(out_dir), "--size", "256"],
                              capture_output=True, text=True)
        if proc.returncode != 0:
            result["lods"].append({"level": level, "error": (proc.stderr or proc.stdout)[:200]})
            continue
        doc = json.loads((out_dir / "compare.json").read_text(encoding="utf-8"))
        entry = {k: doc.get(k) for k in ("max_delta_e", "mean_delta_e", "max_hist_distance",
                                          "max_white_gain", "max_magenta", "min_iou",
                                          "source_textured", "lod_textured")}
        entry["level"] = level
        entry["images"] = str(out_dir.relative_to(repo)).replace("\\", "/")
        problems = []
        if doc.get("source_textured") and not doc.get("lod_textured"):
            problems.append("the LOD has no texture although the source does")
        if float(doc.get("mean_delta_e") or 0.0) > VISUAL_MAX_MEAN_DELTA_E.get(level, 10.0):
            problems.append(f"mean colour difference {doc.get('mean_delta_e'):.1f} dE")
        if float(doc.get("max_white_gain") or 0.0) > VISUAL_MAX_WHITE_GAIN:
            problems.append(f"{doc.get('max_white_gain'):.0%} more white surface")
        if float(doc.get("max_magenta") or 0.0) > VISUAL_MAX_MAGENTA:
            problems.append("missing-texture magenta in the render")
        entry["problems"] = problems
        result["lods"].append(entry)
        if problems:
            result["status"] = "failed"
            result["reason"] = f"LOD{level}: " + "; ".join(problems)
        ctx.emit(stage, "log",
                 f"{asset_id} LOD{level} vs source: mean dE {float(doc.get('mean_delta_e') or 0):.2f}, "
                 f"white +{float(doc.get('max_white_gain') or 0):.3f}, IoU {float(doc.get('min_iou') or 0):.3f}")
    out_root.mkdir(parents=True, exist_ok=True)
    stamp.write_text(json.dumps({"key": key, "result": result}, indent=2), encoding="utf-8")
    return result


def _newest_build_tool(ctx: PipelineContext, name: str) -> Optional[Path]:
    exe = f"{name}.exe" if _is_windows() else name
    root = ctx.settings.engine_dir / "build"
    found = sorted(root.rglob(exe), key=lambda p: p.stat().st_mtime) if root.exists() else []
    return found[-1] if found else None


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _ensure_tool(ctx: PipelineContext, stage: ProjectStage) -> Path:
    """Locate ``ptv_glb_optimize``, building it once if necessary."""

    exe = "ptv_glb_optimize.exe" if _is_windows() else "ptv_glb_optimize"
    engine_build = ctx.settings.engine_dir / "build"
    if engine_build.exists():
        # Newest wins: several build trees can each hold a copy, and an old
        # one would silently drop textures.
        found = sorted(engine_build.rglob(exe), key=lambda p: p.stat().st_mtime)
        if found:
            return found[-1]

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
