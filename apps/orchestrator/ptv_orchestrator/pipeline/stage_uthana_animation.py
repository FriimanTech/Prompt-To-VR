"""Stage 9c: resolve, download and merge every planned Uthana motion.

For each character in ``uthana_motion_plan.json``, one motion at a time:

* **Locomotion** -- List your motions (up to ``uthana_motion_list_limit``),
  Motion Verification by exact canonical name; on a match the existing
  motion is reused, otherwise Generate Locomotion creates it and Rename gives
  it the canonical name (only after the create returned its id).
* **Library** -- the motion id is used as-is (the shared idle).
* **Download** -- ``glb``, ``no_mesh=True``, ``in_place=True``, retargeted onto
  THIS character's ``character_id``, into ``assets/<id>/uthana_motions/``.

Only when every motion of a character is on disk does the merge run (Part 5):
the clips are combined with the Uthana-rigged GLB into ``model_animated.glb``
and the intent -> clip map is written to ``asset_animation.json`` exactly as
the Tripo path does, plus the clip speeds the scene compiler needs.

``uthana_animation.json`` is the ledger: per motion, how it was obtained
(created / reused / library), its id and file, so a Resume never generates
or downloads anything twice.
"""

from __future__ import annotations

from pathlib import Path
from typing import Any

from ptv_providers import UthanaClient, UthanaClientError
from ptv_schemas import ProjectStage, ProjectStatus, UthanaCharacterMotionPlan

from .character_artifacts import (
    ASSET_ANIMATION_NAME,
    UTHANA_LEDGER_NAME,
    UTHANA_MOTIONS_DIR,
    UTHANA_RIGGED_GLB,
    load_json,
    load_motion_plan,
    rel,
    save_json,
    sha256_file,
)
from .context import PipelineContext, StageResult
from .uthana.clip_analysis import measure_root_speed
from .uthana.glb_merge import MergeReport, MotionInput, merge_character
from .uthana.gltf_io import Glb, GltfError
from .uthana.motion_library import (
    CostGuardExceeded,
    MotionCatalogue,
    build_resolvers,
    download_motion,
    motion_file_name,
)

_STAGE = ProjectStage.uthana_animation
_ANIMATED_GLB = "model_animated.glb"


def run(ctx: PipelineContext) -> StageResult:
    return run_with(ctx)


def run_with(ctx: PipelineContext, *, client: Any = None) -> StageResult:
    ctx.emit(_STAGE, "running", "Building Uthana motions")
    state = ctx.store.load_state(ctx.project_id)
    ctx.store.record_stage(state, _STAGE, ProjectStatus.running)

    plan = load_motion_plan(ctx.project_dir)
    characters = list(plan.characters) if plan else []
    if not characters:
        message = "No Uthana motions planned"
        ctx.store.record_stage(state, _STAGE, ProjectStatus.succeeded, message=message)
        ctx.emit(_STAGE, "succeeded", message)
        return StageResult(stage=_STAGE)

    if client is None:
        client = UthanaClient(ctx.settings.uthana_api_key)
    log = lambda text: ctx.store.append_log(ctx.project_id, "uthana_animation", text)  # noqa: E731
    catalogue = MotionCatalogue(client, limit=ctx.settings.uthana_motion_list_limit, log=log)
    resolvers = build_resolvers(
        client, catalogue,
        max_generated_seconds=ctx.settings.uthana_max_generated_seconds_per_run, log=log,
    )
    listed = False

    ledger_path = ctx.project_dir / UTHANA_LEDGER_NAME
    ledger = load_json(ledger_path)
    anim_path = ctx.project_dir / ASSET_ANIMATION_NAME
    anim_report = load_json(anim_path)

    produced: list[str] = []
    done = incomplete = 0
    for char in characters:
        ctx.raise_if_cancelled()
        entry = ledger.setdefault(char.asset_id, {})
        entry["character_id"] = char.character_id
        motions_entry: dict[str, Any] = entry.setdefault("motions", {})
        item_dir = ctx.project_dir / "assets" / char.asset_id
        files: dict[str, Path] = {}
        missing: list[str] = []

        for spec in char.motions:
            ctx.raise_if_cancelled()
            dest = item_dir / UTHANA_MOTIONS_DIR / motion_file_name(spec.motion_name)
            prior = motions_entry.get(spec.key)
            if (
                isinstance(prior, dict)
                and prior.get("motion_name") == spec.motion_name
                and prior.get("character_id") == char.character_id
                and dest.is_file()
                and prior.get("sha256") == sha256_file(dest)
            ):
                if spec.gait is not None and not prior.get("root_motion_mps"):
                    prior["root_motion_mps"] = _root_motion_speed(
                        client, char.character_id, prior["motion_id"], dest)
                files[spec.key] = dest
                continue
            resolver = resolvers.get(spec.kind)
            if resolver is None:
                missing.append(f"{spec.key} (no resolver for {spec.kind.value})")
                continue
            try:
                if spec.kind.value == "uthana_locomotion" and not listed:
                    catalogue.refresh()
                    listed = True
                resolved = resolver.resolve(spec, char.character_id)
                download_motion(client, char.character_id, resolved.motion_id, dest)
                root_mps = None
                if spec.gait is not None:
                    root_mps = _root_motion_speed(client, char.character_id,
                                                  resolved.motion_id, dest)
            except CostGuardExceeded as exc:
                ctx.emit(_STAGE, "warning", f"{char.asset_id}: {exc}")
                missing.append(spec.key)
                continue
            except (UthanaClientError, RuntimeError, ValueError, OSError) as exc:
                ctx.emit(_STAGE, "warning", f"{char.asset_id} {spec.key}: {exc}"[:400])
                log(f"{char.asset_id} {spec.key}: {exc}")
                missing.append(spec.key)
                continue
            motions_entry[spec.key] = {
                "motion_name": spec.motion_name,
                "motion_id": resolved.motion_id,
                "source": resolved.source,
                "kind": spec.kind.value,
                "character_id": char.character_id,
                "file": rel(ctx.settings.repo_root, dest),
                "sha256": sha256_file(dest),
                "requested_move_speed": spec.move_speed,
                "root_motion_mps": root_mps,
                "source_height_m": char.height_for_motion_m,
            }
            files[spec.key] = dest
            ctx.emit(_STAGE, "info",
                     f"{char.asset_id}: {spec.key} <- {spec.motion_name} ({resolved.source})")
            save_json(ledger_path, ledger)

        # Completion gate: merge only with every motion present.
        if missing:
            entry["status"] = "incomplete"
            entry["missing"] = missing
            incomplete += 1
            ctx.emit(_STAGE, "warning",
                     f"{char.asset_id}: not merged, missing {', '.join(missing)}; "
                     "the character stays rigged-only")
            anim_report[char.asset_id] = {"status": "incomplete", "provider": "uthana",
                                          "missing": missing}
            continue

        try:
            out = item_dir / _ANIMATED_GLB
            merge = merge_character(
                item_dir / UTHANA_RIGGED_GLB,
                [MotionInput(key=s.key, clip_name=s.motion_name, path=files[s.key])
                 for s in char.motions],
                out,
            )
        except (GltfError, OSError, KeyError) as exc:
            entry["status"] = "merge_failed"
            entry["error"] = str(exc)[:600]
            incomplete += 1
            ctx.emit(_STAGE, "warning", f"{char.asset_id}: merge failed ({exc})")
            anim_report[char.asset_id] = {"status": "failed", "provider": "uthana",
                                          "error": str(exc)[:600]}
            continue
        for w in merge.warnings:
            ctx.emit(_STAGE, "warning", f"{char.asset_id}: {w}"[:400])
        entry["status"] = "animated"
        entry["merge"] = merge.as_dict()
        root_speeds = {
            k: float(v["root_motion_mps"]) for k, v in motions_entry.items()
            if isinstance(v, dict) and v.get("root_motion_mps")
        }
        anim_report[char.asset_id] = animation_entry(ctx, char, merge, out, root_speeds)
        produced.append(rel(ctx.settings.repo_root, out))
        done += 1

    save_json(ledger_path, ledger)
    save_json(anim_path, anim_report)
    parts = [f"{done} animated"]
    if incomplete:
        parts.append(f"{incomplete} incomplete")
    message = ", ".join(parts)
    ctx.store.record_stage(state, _STAGE, ProjectStatus.succeeded, message=message,
                           artifacts=produced)
    ctx.emit(_STAGE, "succeeded", message)
    return StageResult(stage=_STAGE, artifacts=produced)


def _root_motion_speed(client: Any, character_id: str, motion_id: str, dest: Path):
    """True average speed, from a free root-motion download of the same motion."""
    root_path = dest.with_name(dest.stem + ".root.glb")
    try:
        download_motion(client, character_id, motion_id, root_path, in_place=False)
        return measure_root_speed(Glb.load(root_path))
    except (UthanaClientError, RuntimeError, GltfError, OSError):
        return None


def _plausible(value: Any, requested: float) -> bool:
    return (
        isinstance(value, (int, float)) and requested > 0
        and 0.5 * requested <= float(value) <= 1.5 * requested
    )


def animation_entry(
    ctx: PipelineContext,
    char: UthanaCharacterMotionPlan,
    merge: MergeReport,
    out: Path,
    root_speeds: dict[str, float] | None = None,
) -> dict[str, Any]:
    """The asset_animation.json record, in the Tripo path's shape."""
    by_key = {c.key: c for c in merge.clips}
    root_speeds = root_speeds or {}
    speeds: dict[str, Any] = {}
    for spec in char.motions:
        clip = by_key.get(spec.key)
        if spec.gait is None:
            continue
        measured = clip.ground_speed_mps if clip else None
        root = root_speeds.get(spec.key)
        requested = spec.move_speed or 0.0
        # Preference: the root-motion speed (the clip's real average travel),
        # then the in-place foot-slip estimate, then what was requested. A
        # reading far from the request is treated as a measurement artefact.
        if _plausible(root, requested):
            glb_speed, used = float(root), "root_motion"
        elif _plausible(measured, requested):
            glb_speed, used = float(measured), "foot_slip"
        else:
            glb_speed, used = requested, "requested"
        speeds[spec.key] = {
            "requested_mps": spec.move_speed,
            "root_motion_mps": root,
            "measured_mps": measured,
            "used": used,
            "source_height_m": char.height_for_motion_m,
            "engine_scale": char.engine_scale,
            # What the engine sees once it scales the character to its target
            # size: stride scales with the body, so speed does too.
            "engine_mps": round(glb_speed * char.engine_scale, 4),
        }
    return {
        "status": "animated",
        "provider": "uthana",
        "character_id": char.character_id,
        "animations": {s.key: s.motion_name for s in char.motions},
        "clip_names": [s.motion_name for s in char.motions],
        "clip_speeds": speeds,
        "hips_offset_m": merge.hips_offset_m,
        "validation_ok": merge.ok,
        "warnings": merge.warnings,
        "animated_glb": rel(ctx.settings.repo_root, out),
        "animate_in_place": True,
    }
