"""Stage 9: deterministic scene compile.

Combines SceneSpec(s), AssetManifest (asset normalization), and MechanicPlan
into an engine-readable ``scene.json`` under
``projects/<id>/builds/scene/<scene_id>.json`` plus a top-level
``scene.json`` pointing at the entry-point scene. The engine consumes this
JSON directly via ``engine/runtime/src/scene/SceneJson.cpp``.

This stage is fully deterministic and runs locally. No LLM calls.

Asset safety-net (Part 3 of apps/adjustments.md): every AssetItem in the
``AssetManifest`` is *guaranteed* to appear in the compiled scene. If the
LLM-generated ``SceneSpec`` failed to reference an asset, this stage
auto-instantiates a category-appropriate default entity for it so the
.glb still shows up in the running VR game. Concretely:

    environment -> world entity at the origin (static, mesh collision)
    character   -> NPC capsule (or enemy/player_avatar based on style_notes)
    prop        -> reachable interactable (or static dressing)
    fx          -> non-physics VFX impostor near the spawn
    ui          -> world-anchored HUD panel in front of the spawn

This makes the pipeline robust to LLM forgetfulness so a generated city
glb is never silently dropped.
"""

from __future__ import annotations

import json
from collections.abc import Callable, Iterable
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from ptv_schemas import (
    AssetItem,
    AssetManifest,
    BuildSpec,
    MechanicPlan,
    MechanicClassification,
    ProjectStage,
    ProjectStatus,
    SceneSpec,
)
from ptv_schemas.asset_manifest import AssetCategory

from ..engine_catalog import role_of
from .context import PipelineContext, StageResult
from ..fsutil import atomic_write_text
from .binding_resolution import (
    drop_unhosted_npc_ai,
    registered_ids,
    unresolved_bindings,
)
from .encounters import (
    apply_npc_population,
    finalize_encounter_zones,
    place_persistent_npcs,
)
from .game_flow_scene import apply_game_flow, game_flow_violations
from .interaction_rules import engine_drawn_fx_assets
from .projectile_scale import apply_scales, projectile_scale_corrections
from .weapon_scene import apply_weapons, weapon_violations
from .npc_population import repair_npc_plan
from .gameplay_relocation import (
    relocate_gameplay_layer,
    resolve_overlaps,
    spawn_pose,
    yaw_from_direction,
)
from .player_bindings import bind_player_roles, player_block_fields


SCENE_JSON_VERSION = 1


# ---------------------------------------------------------------------------
# Asset safety-net helpers
# ---------------------------------------------------------------------------


def _style_notes_lower(asset: AssetItem) -> str:
    return f"{asset.name} {asset.style_notes}".lower()


def _character_role(asset: AssetItem) -> str:
    """'enemy', 'player_avatar', or 'npc' for a character asset.

    Prefers the explicit ``character_role`` the manifest now carries. The
    keyword matching below is the fallback for artifacts generated before
    that field existed -- it guessed the player avatar from phrases like
    "player body", which worked until a prompt phrased it differently.
    """

    explicit = getattr(asset, "character_role", None)
    if explicit is not None and getattr(explicit, "value", "none") != "none":
        # 'enemy' is still a style-notes distinction: the schema records what
        # the character IS to the player, not which side it fights for, and
        # faction comes from the NpcPlan instead.
        if explicit.value == "player_avatar":
            return "player_avatar"

    text = _style_notes_lower(asset)
    enemy_kw = (
        "enemy", "hostile", "boss", "drone", "minion", "monster",
        "demon", "robot foe", "antagonist", "training drone",
    )
    if any(k in text for k in enemy_kw):
        return "enemy"
    avatar_kw = (
        "player avatar", "player body", "first-person body", "first person body",
        "player character", "main character", "protagonist body", "hero body",
    )
    if any(k in text for k in avatar_kw):
        return "player_avatar"
    return "npc"


def _prop_is_grabbable(asset: AssetItem) -> bool:
    text = _style_notes_lower(asset)
    grab_kw = (
        "grab", "grabbable", "pickup", "pick up", "wearable", "wield",
        "throwable", "interactable", "tool", "weapon", "ring", "battery",
    )
    return any(k in text for k in grab_kw)


def _spread_xz(index: int, radius: float = 1.5) -> tuple[float, float]:
    """Deterministic ring placement so multiple unreferenced props don't stack."""
    import math
    angle = (index * 137.508) * math.pi / 180.0  # golden-angle-ish, deterministic
    return radius * math.cos(angle), radius * math.sin(angle)


class SocketContractError(RuntimeError):
    """A bound mechanic needs a socket its asset does not have.

    Raised at scene-compile time because that is the only stage that can see
    both halves of the contract: the CodeTask's ``required_sockets`` (decided
    by the mechanic-plan agent, before any GLB existed) and the asset's
    derived sockets (decided by the asset_contract stage, which never knew
    which mechanic would be bound). Nothing else in the pipeline can catch the
    mismatch, and the runtime symptom -- a projectile leaving from the front
    of a bounding sphere instead of a barrel -- is easy to miss in a headset.
    """


class TextureContractError(RuntimeError):
    """A placed mesh references a texture file that does not exist."""


def _check_scene_textures(ctx: PipelineContext, compiled: list, state) -> list[str]:
    """Missing texture files across every placed mesh, plus a texture
    summary written into each scene's performance block.

    Quest projects also want every texture to have its ASTC KTX2 twin; a
    texture without one is reported (the headset decodes it on load) but is
    not fatal -- a missing file is."""
    from ptv_schemas import TargetDevice

    from .texture_refs import check_mesh_textures

    quest = getattr(state, "target_device", TargetDevice.pcvr) == TargetDevice.quest
    repo = ctx.settings.repo_root
    problems: list[str] = []
    checked: dict[str, tuple[list[str], list[str], int]] = {}
    for scene_id, doc in compiled:
        textured_meshes = 0
        no_ktx2_total: set[str] = set()
        for e in doc.get("entities") or []:
            mesh = e.get("mesh") if isinstance(e, dict) else None
            if not isinstance(mesh, dict):
                continue
            paths = [mesh.get("glb_path")] + list(mesh.get("lod_paths") or [])
            any_textured = False
            for p in dict.fromkeys(x for x in paths if isinstance(x, str) and x):
                if p not in checked:
                    glb = repo / p
                    checked[p] = (check_mesh_textures(glb, need_ktx2=quest)
                                  if glb.is_file() else ([], [], 0))
                missing, no_ktx2, refs = checked[p]
                problems.extend(f"{scene_id}/{e.get('id', '?')}: {m}" for m in missing)
                no_ktx2_total.update(no_ktx2)
                any_textured = any_textured or refs > 0
            textured_meshes += any_textured
        perf = doc.setdefault("performance", {})
        perf["textures"] = {"textured_mesh_entities": textured_meshes,
                            "textures_without_astc": len(no_ktx2_total)}
        if quest and no_ktx2_total:
            ctx.emit(ProjectStage.scene_compile, "warning",
                     f"{scene_id}: {len(no_ktx2_total)} texture(s) have no ASTC KTX2 and "
                     "will be decoded on the headset")
    return problems


def _apply_scene_complexity(ctx: PipelineContext, compiled: list) -> None:
    from . import memory_model as mm
    from .scene_complexity import analyse, apply_static_batch_flags, decide_initial_settings

    calib_path = ctx.settings.projects_dir / "memory_calibration.json"
    calibration = mm.load_calibration(calib_path)
    summary: dict[str, Any] = {"scenes": {}, "calibration": calibration}
    worst_status = "estimated_safe_not_device_tested"
    # A headset run from before beats the model: its measured frame cost
    # drives this compile's starting settings.
    measured: dict[str, Any] = {}
    try:
        rv = json.loads((ctx.project_dir / "runtime_verification.json").read_text(
            encoding="utf-8"))
        measured = dict(rv.get("performance") or {})
    except (OSError, ValueError):
        measured = {}
    for scene_id, doc in compiled:
        perf = doc.setdefault("performance", {})
        analysis = analyse(doc)
        if measured.get("gpu_p95") and scene_id == (compiled[0][0] if compiled else scene_id):
            analysis["measured"] = {k: measured[k] for k in (
                "gpu", "gpu_p95", "cpu", "cpu_p95", "fps", "calls", "tris", "ffr")
                if k in measured}
            # Measured at that run's FFR level, i.e. with its saving already
            # in; it decides whether the scene counts as heavy.
            analysis["estimate"] = dict(analysis["estimate"],
                                        gpu_ms_p95=float(measured["gpu_p95"]),
                                        cpu_ms_p95=float(measured.get("cpu_p95")
                                                         or analysis["estimate"]["cpu_ms_p95"]),
                                        source="measured on Quest 3")
            if "estimate_with_room_culling" in analysis:
                analysis["estimate_with_room_culling"] = analysis["estimate"]
        notes = decide_initial_settings(perf, analysis)
        batched = (apply_static_batch_flags(doc, analysis["static_batch_ids"])
                   if perf.get("static_batching", True) else 0)
        perf["complexity"] = {k: v for k, v in analysis.items() if k != "static_batch_ids"}
        c, v, est = analysis["counts"], analysis["visibility"], analysis["estimate"]
        ctx.emit(ProjectStage.scene_compile, "info",
                 f"{scene_id}: {c['entities']} entities, {c['triangles_lod0_total']:,} LOD0 "
                 f"tris; visible p95 {int(v['visible_tris_p95']):,} tris / "
                 f"{int(v['draws_p95'])} draws; estimated GPU {est['gpu_ms_p95']:.1f} ms, "
                 f"CPU {est['cpu_ms_p95']:.1f} ms; {batched} static-batch entities")
        for note in notes:
            ctx.emit(ProjectStage.scene_compile, "info", f"{scene_id}: {note}")

        # Memory: exact from the built files where possible.
        mem = mm.estimate(mm.inputs_from_scene(doc, ctx.settings.repo_root),
                          baseline_mb=float(perf.get("engine_baseline_pss_mb") or 260.0),
                          target_mb=float(perf.get("memory_target_mb") or 4608.0),
                          ceiling_mb=float(perf.get("memory_ceiling_mb") or 5120.0),
                          calibration=calibration)
        perf["memory_estimate"] = {k: mem[k] for k in (
            "steady_mb", "peak_mb", "always_resident_mb", "streamable_mb",
            "load_transient_mb", "outcome", "calibration", "gap_mb")}
        perf["resident_memory_budget_mb"] = mem["resident_memory_budget_mb"]
        status = mm.memory_status_without_device(mem)
        if status == "at_risk":
            worst_status = status
        ctx.emit(ProjectStage.scene_compile, "warning" if mem["outcome"] == "over" else "info",
                 f"{scene_id}: memory estimate steady {mem['steady_mb']:.0f} MB, peak "
                 f"{mem['peak_mb']:.0f} MB (target {mem['target_mb']:.0f}, ceiling "
                 f"{mem['ceiling_mb']:.0f}, Meta limit 5888) -- {mem['outcome']}")
        summary["scenes"][scene_id] = {"complexity": perf["complexity"], "memory_estimate": mem}

    (ctx.project_dir / "scene_complexity.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True), encoding="utf-8")
    # Provisional until a headset measures it; never "verified" from here.
    status_path = ctx.project_dir / "memory_status.json"
    prev: dict[str, Any] = {}
    if status_path.exists():
        try:
            prev = json.loads(status_path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            prev = {}
    if prev.get("source") != "device":
        status_path.write_text(json.dumps({
            "status": worst_status, "source": "estimate",
            "reasons": ["not tested on a Quest 3"],
            "estimates": {sid: {"steady_mb": s["memory_estimate"]["steady_mb"],
                                "peak_mb": s["memory_estimate"]["peak_mb"]}
                          for sid, s in summary["scenes"].items()},
        }, indent=2), encoding="utf-8")


class EquipSlotContractError(RuntimeError):
    """Something binds ``builtin.equip`` and no wearer offers it a slot.

    Raised at scene-compile time for the same reason the socket error is:
    this is the only stage that sees both the mechanic bindings (with the
    MechanicPlan config merged in) and every entity's declared slots. The
    runtime symptom is an item that simply refuses to holster, with no error
    anywhere.
    """


class GameFlowContractError(RuntimeError):
    """The game design's loop cannot complete in a compiled scene."""


class WeaponContractError(RuntimeError):
    """A designed weapon cannot work in a compiled scene (no muzzle, no
    template for its projectile, a bow without limb tips)."""


class ModelFrameContractError(RuntimeError):
    """A skinned character reached scene_compile without a measured frame."""


class MissingNpcError(RuntimeError):
    """An NPC the game requires has no model to put in the scene."""


class UnresolvedBindingError(RuntimeError):
    """The scene binds mechanics the build does not contain."""


class SceneAssetMismatchError(RuntimeError):
    """The SceneSpec describes assets this project does not have.

    Always the same cause: a ``scene_spec.<id>.json`` left over from a run
    with a different AssetManifest. It validates against the schema, so
    nothing upstream objects; it just describes a different game. Compiling
    it anyway emits meshes pointing at GLBs that were never generated.
    """


@dataclass(frozen=True)
class _SocketNeed:
    """A required-socket check that cannot be answered entity-locally.

    The entity binding the mechanic has no mesh, so the mechanic is not
    reading the socket off ``self`` -- it resolves a held or spawned item at
    runtime. Whether that is satisfiable depends on the WHOLE scene, which is
    not known until every entity has been compiled.
    """

    entity_id: str
    mechanic_id: str
    sockets: tuple[str, ...]


def _check_required_sockets(
    entity: Any,
    plan_item: Any,
    mesh: dict[str, Any] | None,
    contracts: dict[str, dict[str, Any]],
) -> tuple[list[str], _SocketNeed | None]:
    """Violations for one entity/mechanic pair, plus any deferred check.

    Mechanics with no ``code_task``, or a code task that declares no
    ``required_sockets``, are skipped entirely -- so this is inert for every
    project generated before sockets existed.

    When the bound entity HAS a mesh, that mesh must carry every required
    socket: the mechanic is bound to this entity, so ``self`` is what it will
    read, and a missing socket is a hard failure. When the entity has NO mesh
    -- a hand, a spawn marker, a rig node -- there is no ``self`` geometry to
    read, so the mechanic must be resolving its target at runtime. That case
    is deferred to :func:`_check_deferred_socket_needs`, which asks the much
    weaker but still meaningful question "can anything in this game supply
    that socket?".
    """
    task = getattr(plan_item, "code_task", None)
    required = list(getattr(task, "required_sockets", None) or [])
    if not required:
        return [], None

    mechanic_id = getattr(task, "id", None) or getattr(plan_item, "id", "?")
    if mesh is None:
        return [], _SocketNeed(entity.id, mechanic_id, tuple(required))

    asset_id = mesh.get("asset_id", "?")
    available = sorted((mesh.get("sockets") or {}).keys())
    contract = contracts.get(asset_id) or {}
    out: list[str] = []
    for name in required:
        if name in (mesh.get("sockets") or {}):
            continue
        detail = ""
        if not contract:
            detail = " (asset has no entry in asset_contract.json)"
        elif contract.get("status") != "ok":
            detail = f" (asset contract status: {contract.get('status')})"
        out.append(
            f"entity '{entity.id}' binds '{mechanic_id}' which requires socket"
            f" '{name}', but asset '{asset_id}' provides {available}{detail}"
        )
    return out, None


def _equip_slot_json(slot: Any) -> dict[str, Any]:
    """One ``EquipSlotSpec`` in the shape ``SceneLoader`` reads.

    Vectors and quaternions are flattened to arrays like every other
    transform in ``scene.json``, so the C++ side needs no new parsing idiom.
    """
    return {
        "name": slot.name,
        "body_slot": slot.body_slot,
        "host_item_slot": slot.host_item_slot,
        "host_socket": slot.host_socket,
        "local_position": [
            slot.local_position.x,
            slot.local_position.y,
            slot.local_position.z,
        ],
        "local_rotation": [
            slot.local_rotation.x,
            slot.local_rotation.y,
            slot.local_rotation.z,
            slot.local_rotation.w,
        ],
        "snap_radius_m": slot.snap_radius_m,
        "exclusive": slot.exclusive,
        "accepts_tags": list(slot.accepts_tags),
    }


@dataclass(frozen=True)
class _EquipNeed:
    """An equip binding and the slot names it insists on."""

    entity_id: str
    mechanic_id: str
    slots: tuple[str, ...]


def _equip_need(
    entity: Any, binding: Any, resolved: dict[str, Any], plan_item: Any
) -> _EquipNeed | None:
    """``_EquipNeed`` when this binding equips things, else ``None``.

    Three ways a binding can be the equip mechanic, and all three need
    ``CEquipSlots`` on a wearer to work: the scene naming ``builtin.equip``
    directly, a ``builtin+config`` item resolving to it, and a
    ``builtin+config_code_edit`` copy of its source (which still reads
    ``params.slots`` the same way, being an edit of the same file).
    """
    is_equip = (
        binding.mechanic_id == "builtin.equip"
        or resolved.get("builtin_id") == "builtin.equip"
        or (
            plan_item is not None
            and getattr(plan_item, "base_builtin_id", None) == "builtin.equip"
        )
    )
    if not is_equip:
        return None
    wanted = (resolved.get("params") or {}).get("slots") or []
    return _EquipNeed(
        entity.id, binding.mechanic_id, tuple(str(s) for s in wanted)
    )


def _check_equip_slots(
    entities: list[dict[str, Any]], needs: Iterable[_EquipNeed]
) -> list[str]:
    """Violations for equip bindings that have nowhere to put anything.

    ``EquipMechanic::pick_slot_near`` returns no candidate at all when the
    wearer has no ``CEquipSlots``, so a scene that binds ``builtin.equip``
    without declaring a slot anywhere produces a game where the item simply
    never attaches -- silently, with every stage green. Holstering is usually
    something the prompt asked for by name, so shipping a build that cannot
    holster is worse than failing here.

    Deliberately scene-wide rather than per-entity: the binding sits on the
    ITEM (the pistol carries ``builtin.equip``) while the slot is declared on
    the WEARER, so "does this entity have slots" is the wrong question.
    """
    declared: dict[str, list[str]] = {}
    placed: dict[str, list[str]] = {}
    for e in entities:
        for slot in e.get("equip_slots") or []:
            declared.setdefault(slot["name"], []).append(e["id"])
            # A spawn template is a blueprint: every NPC type (and every
            # clone of one) carries the same hand slot by construction.
            if "template" not in (e.get("tags") or []):
                placed.setdefault(slot["name"], []).append(e["id"])

    out: list[str] = [
        f"equip slot '{name}' is declared by {owners}; slot names must be"
        " unique across the scene or params.slots cannot address them"
        for name, owners in sorted(placed.items())
        if len(owners) > 1
    ]

    for need in needs:
        if not declared:
            out.append(
                f"entity '{need.entity_id}' binds '{need.mechanic_id}' but no"
                " entity in the scene declares any equip_slots, so there is"
                " nowhere for it to be equipped. Declare the slot on the"
                " WEARER (the player rig, avatar or NPC) through"
                " EntitySpec.equip_slots -- not on the holster prop."
            )
            continue
        missing = [name for name in need.slots if name not in declared]
        if missing:
            out.append(
                f"entity '{need.entity_id}' binds '{need.mechanic_id}' with"
                f" params.slots={list(need.slots)}, but {missing} are declared"
                f" by no entity. Declared slots: {sorted(declared)}"
            )
    return out


def _check_deferred_socket_needs(
    needs: Iterable[_SocketNeed],
    contracts: dict[str, dict[str, Any]],
    referenced_asset_ids: set[str],
) -> tuple[list[str], list[str]]:
    """``(violations, warnings)`` for mechanics bound to mesh-less entities.

    A shooting hub bound to the player's right hand is the canonical case. The
    hand has no mesh, and the generated C++ correctly reads the muzzle off the
    weapon it resolves at runtime, guarded by ``has_socket``. Demanding the
    socket sit on the hand is a false positive that fails a correct build.

    What is still worth guaranteeing is that the socket exists SOMEWHERE in
    the game: if no asset in the scene can supply a "muzzle", every shot will
    take the suppression branch and the mechanic is dead code. So a satisfied
    need warns (naming which assets provide it, which is the useful debugging
    fact) and an unsatisfiable one fails.
    """
    providers: dict[str, list[str]] = {}
    for asset_id in sorted(referenced_asset_ids):
        for name in sorted((contracts.get(asset_id) or {}).get("sockets") or {}):
            providers.setdefault(name, []).append(asset_id)

    violations: list[str] = []
    warnings: list[str] = []
    for need in needs:
        for name in need.sockets:
            supplied = providers.get(name) or []
            if supplied:
                warnings.append(
                    f"entity '{need.entity_id}' binds '{need.mechanic_id}'"
                    f" requiring socket '{name}' but has no mesh of its own;"
                    f" satisfied at runtime by {supplied}"
                )
            else:
                violations.append(
                    f"entity '{need.entity_id}' binds '{need.mechanic_id}'"
                    f" which requires socket '{name}', and no asset in the"
                    " scene provides it"
                )
    return violations, warnings


def _default_entity_for_asset(
    asset: AssetItem,
    glb_path: str,
    spread_index: int,
    norm: dict[str, Any] | None = None,
    contract: dict[str, Any] | None = None,
) -> dict[str, Any]:
    """Build a category-appropriate default entity for an unreferenced asset."""

    base_mesh = {
        "asset_id": asset.id,
        "glb_path": glb_path,
        "target_scale_meters": asset.target_scale_meters,
        "collision_shape": asset.collision_shape,
        **_mesh_render_fields(
            norm or {}, asset.target_scale_meters, contract, asset_id=asset.id
        ),
    }
    transform_identity = {
        "position": [0.0, 0.0, 0.0],
        "rotation": [0.0, 0.0, 0.0, 1.0],
        "scale": 1.0,
    }

    if asset.category == AssetCategory.environment:
        return {
            "id": f"auto_{asset.id}",
            "name": asset.name or asset.id,
            "transform": transform_identity,
            "mesh": {**base_mesh, "collision_shape": "mesh"},
            "physics": {
                "kind": "static",
                "mass_kg": 1.0,
                "collision_shape": "mesh",
                "restitution": 0.1,
                "friction": 0.6,
            },
            "mechanic_bindings": [],
            "tags": ["environment", "auto"],
        }

    if asset.category == AssetCategory.character:
        role = _character_role(asset)
        if role == "player_avatar":
            return {
                "id": f"auto_{asset.id}",
                "name": asset.name or asset.id,
                "transform": transform_identity,
                "mesh": base_mesh,
                "physics": None,
                "mechanic_bindings": [],
                "tags": ["player_avatar", "auto"],
            }
        if role == "enemy":
            x, z = _spread_xz(spread_index, radius=4.0)
            return {
                "id": f"auto_{asset.id}",
                "name": asset.name or asset.id,
                "transform": {
                    "position": [x, 1.0, abs(z) + 3.0],
                    "rotation": [0.0, 0.0, 0.0, 1.0],
                    "scale": 1.0,
                },
                "mesh": base_mesh,
                "physics": {
                    "kind": "dynamic",
                    "mass_kg": 5.0,
                    "collision_shape": "capsule",
                    "restitution": 0.2,
                    "friction": 0.4,
                },
                "mechanic_bindings": [],
                "tags": ["enemy", "target", "auto"],
            }
        # Default character: NPC
        x, z = _spread_xz(spread_index, radius=1.5)
        return {
            "id": f"auto_{asset.id}",
            "name": asset.name or asset.id,
            "transform": {
                "position": [x, 0.0, z],
                "rotation": [0.0, 0.0, 0.0, 1.0],
                "scale": 1.0,
            },
            "mesh": base_mesh,
            "physics": {
                "kind": "static",
                "mass_kg": 1.0,
                "collision_shape": "capsule",
                "restitution": 0.2,
                "friction": 0.5,
            },
            "mechanic_bindings": [],
            "tags": ["npc", "auto"],
        }

    if asset.category == AssetCategory.prop:
        x, z = _spread_xz(spread_index, radius=0.8)
        if _prop_is_grabbable(asset):
            return {
                "id": f"auto_{asset.id}",
                "name": asset.name or asset.id,
                "transform": {
                    "position": [x, 1.0, -abs(z) - 0.3],
                    "rotation": [0.0, 0.0, 0.0, 1.0],
                    "scale": 1.0,
                },
                "mesh": base_mesh,
                "physics": {
                    "kind": "dynamic",
                    "mass_kg": 0.3,
                    "collision_shape": "aabb",
                    "restitution": 0.2,
                    "friction": 0.5,
                },
                "mechanic_bindings": [],
                "tags": ["interactable", "grabbable", "auto"],
            }
        # Static dressing prop.
        return {
            "id": f"auto_{asset.id}",
            "name": asset.name or asset.id,
            "transform": {
                "position": [x, 0.0, z],
                "rotation": [0.0, 0.0, 0.0, 1.0],
                "scale": 1.0,
            },
            "mesh": base_mesh,
            "physics": {
                "kind": "static",
                "mass_kg": 1.0,
                "collision_shape": "aabb",
                "restitution": 0.2,
                "friction": 0.5,
            },
            "mechanic_bindings": [],
            "tags": ["static", "auto"],
        }

    if asset.category == AssetCategory.fx:
        return {
            "id": f"auto_{asset.id}",
            "name": asset.name or asset.id,
            "transform": {
                "position": [0.0, 1.2, -1.0],
                "rotation": [0.0, 0.0, 0.0, 1.0],
                "scale": 1.0,
            },
            "mesh": base_mesh,
            "physics": None,
            "mechanic_bindings": [],
            "tags": ["vfx", "auto"],
        }

    # ui and any future categories: world-anchored HUD panel.
    return {
        "id": f"auto_{asset.id}",
        "name": asset.name or asset.id,
        "transform": {
            "position": [0.0, 1.5, -1.2],
            "rotation": [0.0, 0.0, 0.0, 1.0],
            "scale": 1.0,
        },
        "mesh": base_mesh,
        "physics": None,
        "mechanic_bindings": [],
        "tags": ["hud", "auto"],
    }


def _apply_hold_offsets(
    entities: list[dict[str, Any]], asset_by_id: dict[str, AssetItem]
) -> None:
    """Carry the manifest's per-asset grip correction onto its grab binding.

    ``hold_rotation_offset_deg`` is for the rare asset whose geometry-derived
    grip socket still sits wrong in the hand. It belongs to the asset, so it
    applies wherever that asset is grabbable; a per-entity param wins.
    """
    for e in entities:
        mesh = e.get("mesh") or {}
        asset = asset_by_id.get(str(mesh.get("asset_id") or ""))
        offset = getattr(asset, "hold_rotation_offset_deg", None)
        if not offset:
            continue
        for b in e.get("mechanic_bindings") or []:
            if not isinstance(b, dict):
                continue
            if (b.get("builtin_id") or b.get("id")) != "builtin.grab":
                continue
            b.setdefault("params", {}).setdefault(
                "hold_rotation_offset_deg", [float(v) for v in offset]
            )


def _mesh_render_fields(
    norm: dict[str, Any],
    target_scale_meters: float,
    contract: dict[str, Any] | None = None,
    *,
    asset_id: str = "",
) -> dict[str, Any]:
    """Per-mesh metadata from ``asset_optimization`` and ``asset_contract``.

    ``lod_paths`` is the decimated LOD chain (coarser as the index grows) and
    ``bounds_radius_m`` is the world-space bounding-sphere radius the renderer
    uses for frustum + distance culling. Both are omitted when the asset has
    not been optimized, in which case the runtime falls back to drawing
    ``glb_path`` at full detail with a conservative radius.

    ``sockets`` are the named attachment points (muzzle, grip) the
    ``asset_contract`` stage derived from geometry. This is where they stop
    being unit-space and become metres -- the same conversion, and for the
    same reason, as ``bounds_radius_m`` just below.
    """

    out: dict[str, Any] = {}
    lod_paths = norm.get("lod_paths")
    if lod_paths:
        out["lod_paths"] = list(lod_paths)
    if norm.get("triangle_counts"):
        out["lod_triangle_counts"] = list(norm["triangle_counts"])
    errs = norm.get("lod_errors_rel") or []
    if errs and len(errs) == len(lod_paths or []):
        # Projected-error LOD selection at runtime (relative to mesh extent).
        out["lod_errors_rel"] = [float(e) for e in errs]
    unit_radius = norm.get("bounds_radius_m")
    if unit_radius:
        # The optimizer reports the radius of a mesh normalised to a 1 m
        # longest axis; the runtime rescales by target_scale_meters, so the
        # world-space radius scales identically.
        out["bounds_radius_m"] = float(unit_radius) * float(target_scale_meters)
    # Metres per GLB unit, measured once on LOD0. The runtime applies it to
    # every LOD instead of re-measuring each file, so an object is the same
    # size at every distance and on every launch.
    lod0_longest = float(norm.get("lod0_longest_axis") or 0.0)
    if lod0_longest > 1e-6 and float(target_scale_meters or 0.0) > 0.0:
        out["unit_scale"] = float(target_scale_meters) / lod0_longest
    # Rigged assets need a CSkeleton and the skinned draw pipeline. Carrying
    # the flag in scene.json lets the runtime decide without opening every
    # GLB, and gives the build a record of which assets are animatable.
    if norm.get("skinned"):
        out["skinned"] = True
        out["joint_count"] = int(norm.get("joint_count") or 0)
    # Semantic clip contract: intent ("walk") -> the glTF animation name Tripo
    # actually produced. Generated C++ asks for the intent, exactly as it asks
    # for a socket by name, and never has to know what the retargeter called
    # the clip.
    animations = norm.get("animations")
    if isinstance(animations, dict) and animations:
        out["animations"] = {str(k): str(v) for k, v in animations.items()}
        speeds = _animation_reference_speeds(
            animations.keys(), target_scale_meters
        )
        # Uthana clips carry their own speed (requested or measured from the
        # clip, already at the asset's in-scene size) for every key,
        # gait variants such as "run.punching" included. Those win over the
        # height-derived estimate.
        overrides = norm.get("animation_speed_overrides")
        if isinstance(overrides, dict):
            for key, mps in overrides.items():
                if str(key) in out["animations"] and float(mps) > 0.0:
                    speeds[str(key)] = round(float(mps), 4)
        if speeds:
            out["animation_speeds"] = speeds
    # The character's measured model frame (asset_contract). The runtime
    # pivots the model so its soles sit ``height / 2`` below the entity
    # origin -- the centre of its capsule -- and its face points down -Z,
    # whatever the source put where. So ``foot_offset_m`` is ALWAYS half the
    # in-scene height for a framed character: it is never derived from the
    # sign of the bind-pose sole, which is what used to leave Tripo models
    # (origin at the feet, sole 0) with no offset and a half-height guess.
    frame = (contract or {}).get("frame") if contract else None
    framed = (
        isinstance(frame, dict)
        and frame.get("status") not in (None, "failed")
        and float(frame.get("height") or 0.0) > 1e-6
    )
    socket_scale = float(target_scale_meters)
    if framed:
        height_raw = float(frame["height"])
        target = float(target_scale_meters)
        # A character's target scale is its HEIGHT. Scaling by the longest
        # axis shrinks anything whose arm span or tail beats its height.
        out["scale_basis"] = "height"
        out["unit_scale"] = target / height_raw
        out["foot_offset_m"] = round(0.5 * target, 4)
        out["model_yaw_offset_rad"] = round(float(frame.get("yaw_offset_rad") or 0.0), 6)
        out["model_frame"] = {
            "sole_y": float(frame.get("sole_y") or 0.0),
            "height": height_raw,
            "yaw_offset_rad": out["model_yaw_offset_rad"],
            "method": str(frame.get("method") or ""),
        }
        if out.get("bounds_radius_m"):
            longest_raw = float(frame.get("raw_longest_axis") or height_raw)
            out["bounds_radius_m"] = (
                float(norm["bounds_radius_m"]) * longest_raw * target / height_raw
            )
        longest_raw = float(frame.get("raw_longest_axis") or 0.0)
        if longest_raw > 1e-6:
            socket_scale = longest_raw * target / height_raw
    elif norm.get("skinned"):
        raise ModelFrameContractError(
            f"skinned asset '{asset_id}' has no measured model frame in"
            " asset_contract.json;"
            " re-run asset_contract (it measures the sole, height and forward"
            " axis the runtime needs to stand and face the character)"
        )
    sockets = _sockets_in_meters(contract, socket_scale)
    if sockets:
        out["sockets"] = sockets
        hold = _hold_for(contract)
        if hold is not None:
            out["hold"] = hold
    return out


# Apparent ground speed of a locomotion clip, as a multiple of the character's
# height. A 1.8 m human walks at roughly 1.4 m/s and runs at roughly 4 m/s,
# which is where these come from; expressing them per metre of height keeps a
# house-sized creature and a knee-high one both looking right.
_SPEED_PER_METER_OF_HEIGHT = {
    "walk": 0.78,
    "run": 2.2,
}


def _animation_reference_speeds(
    intents: Iterable[str], target_scale_meters: float
) -> dict[str, float]:
    """How fast each locomotion clip LOOKS like it is travelling, in m/s.

    Tripo retargets with ``animate_in_place=True``, so a walk clip has no root
    motion to measure -- the feet cycle while the root stays put. Without a
    number for the speed the animation *implies*, the runtime cannot tell
    whether moving the character at 3 m/s matches its stride or makes it skate
    across the floor, and foot-sliding is the single most obvious tell that a
    generated character is fake.

    So we derive it the only way available without inspecting the clip:
    from the character's height, which the asset pipeline has already
    normalised to ``target_scale_meters``. ``builtin.character_locomotion``
    divides real speed by this to set the playback rate.

    Only locomotion intents get an entry. An attack or death clip has no
    meaningful ground speed and must never be rate-scaled.
    """
    height = float(target_scale_meters or 0.0)
    if height <= 0.0:
        return {}
    out: dict[str, float] = {}
    for intent in intents:
        factor = _SPEED_PER_METER_OF_HEIGHT.get(str(intent))
        if factor is None:
            continue
        out[str(intent)] = round(factor * height, 4)
    return out


def _sockets_in_meters(
    contract: dict[str, Any] | None,
    target_scale_meters: float,
) -> dict[str, Any]:
    """Convert one asset's unit-space sockets into scene.json metres.

    The tool normalises socket positions by the mesh's longest-axis extent so
    they survive decimation; the runtime scales vertices by
    ``target_scale_meters`` about the origin. Multiplying here puts the socket
    in exactly the same space as the geometry it sits on.
    """
    if not contract:
        return {}
    raw = contract.get("sockets") or {}
    scale = float(target_scale_meters)
    out: dict[str, Any] = {}
    for name, socket in raw.items():
        if not isinstance(socket, dict):
            continue
        pos = socket.get("position") or [0.0, 0.0, 0.0]
        rot = socket.get("rotation") or [0.0, 0.0, 0.0, 1.0]
        if len(pos) < 3 or len(rot) < 4:
            continue
        entry: dict[str, Any] = {
            "position": [float(pos[0]) * scale,
                         float(pos[1]) * scale,
                         float(pos[2]) * scale],
            # Rotation is scale-invariant; pass it through as [x, y, z, w],
            # which is the convention SceneLoader::read_quat expects.
            "rotation": [float(rot[0]), float(rot[1]), float(rot[2]),
                         float(rot[3])],
        }
        # socket_generation's extras: kind (hand sockets are in the OpenXR
        # grip frame), confidence, and unit-space lengths turned to metres.
        for key in ("kind", "confidence", "mirror", "wrap_of"):
            if socket.get(key):
                entry[key] = str(socket[key])
        for unit_key, m_key in (("bore_diameter_unit", "bore_diameter_m"),
                                ("handle_thickness_unit", "handle_thickness_m"),
                                ("lateral_offset_unit", "lateral_offset_m")):
            if socket.get(unit_key) is not None:
                entry[m_key] = round(float(socket[unit_key]) * scale, 5)
        out[name] = entry
    return out


def _hold_for(contract: dict[str, Any] | None) -> dict[str, Any] | None:
    """The contract's hold profile, with its sockets checked to exist."""
    if not contract:
        return None
    hold = contract.get("hold")
    if not isinstance(hold, dict) or hold.get("kind") in (None, "", "none"):
        return None
    sockets = contract.get("sockets") or {}
    if hold.get("primary_socket") not in sockets:
        return None
    out = {k: hold[k] for k in ("kind", "aim_aligned", "two_hand", "primary_socket",
                                "support_socket", "forearm_socket", "finger_pose")
           if k in hold}
    for k in ("support_socket", "forearm_socket"):
        if out.get(k) and out[k] not in sockets:
            out[k] = ""
    for k in ("forward", "up"):
        v = hold.get(k)
        if isinstance(v, list) and len(v) == 3:
            out[k] = [float(x) for x in v]
    return out


def hold_violations(entities: list[dict[str, Any]]) -> tuple[list[str], list[str]]:
    """Holds the runtime cannot solve (errors), and holds it can solve but
    whose geometry looks wrong (warnings: two hands on the same point, a
    forearm strap nowhere near the hand strap)."""
    errors: list[str] = []
    out: list[str] = []
    for e in entities:
        mesh = e.get("mesh") or {}
        hold = mesh.get("hold")
        if not isinstance(hold, dict):
            continue
        sockets = mesh.get("sockets") or {}
        prim = sockets.get(hold.get("primary_socket", ""))
        if prim is None:
            errors.append(f"entity '{e.get('id')}' hold names missing primary socket "
                          f"'{hold.get('primary_socket')}'")
            continue
        if hold.get("kind") == "forearm_strap" and not sockets.get(
                hold.get("forearm_socket") or ""):
            errors.append(f"entity '{e.get('id')}' is held by a forearm strap but has no "
                          "forearm_strap socket")
        for key, lo, hi in (("support_socket", 0.0, 1.2), ("forearm_socket", 0.06, 0.5)):
            other = sockets.get(hold.get(key) or "")
            if other is None:
                continue
            if other.get("wrap_of"):
                continue
            d = sum((float(a) - float(b)) ** 2 for a, b in
                    zip(prim["position"], other["position"])) ** 0.5
            if key == "support_socket" and d < 0.05:
                out.append(f"entity '{e.get('id')}': support socket only {d * 100:.1f} cm "
                           "from the primary grip")
            if not (lo <= d <= hi):
                out.append(f"entity '{e.get('id')}': {key} is {d * 100:.0f} cm from the "
                           f"primary grip (expected {lo * 100:.0f}-{hi * 100:.0f} cm)")
    return errors, out


def _resolve_glb_path(
    asset_id: str,
    manifest_path_hint: str | None,
    normalization_report: dict[str, dict[str, Any]],
) -> str | None:
    """Best-effort GLB path lookup for an unreferenced asset.

    Falls back to the asset-normalization report (which is the source of
    truth populated by stage_asset_normalize). Returns None if no .glb is
    on disk for this asset, in which case we skip auto-instantiation
    rather than emit a broken mesh reference.
    """

    norm = normalization_report.get(asset_id, {}) or {}
    candidate = norm.get("glb_path") or manifest_path_hint
    return candidate or None


def _generated_calls(repo_root: Path, project_id: str, pattern: str) -> bool | None:
    """Whether this project's generated C++ matches ``pattern``; None when the
    project has no generated sources on disk."""
    import re as _re

    rx = _re.compile(pattern)
    found_any = False
    for kind in ("mechanics", "ai", "dialogue"):
        root = repo_root / "engine" / "generated" / kind / project_id
        if not root.is_dir():
            continue
        for f in root.rglob("*"):
            if f.suffix not in (".cpp", ".h", ".hpp"):
                continue
            found_any = True
            if rx.search(f.read_text(encoding="utf-8", errors="replace")):
                return True
    return False if found_any else None


def mesh_for_asset(
    asset_id: str,
    manifest: AssetManifest,
    normalization_report: dict[str, dict[str, Any]] | None,
    asset_contract: dict[str, dict[str, Any]] | None,
) -> dict[str, Any] | None:
    """The compiled ``mesh`` block for one manifest asset, exactly as an
    authored entity showing it would get. None when the asset has no GLB."""
    asset = next((a for a in manifest.items if a.id == asset_id), None)
    norm = (normalization_report or {}).get(asset_id) or {}
    if asset is None or not norm.get("glb_path"):
        return None
    return {
        "asset_id": asset_id,
        "glb_path": norm["glb_path"],
        "target_scale_meters": asset.target_scale_meters,
        "collision_shape": asset.collision_shape,
        **_mesh_render_fields(norm, asset.target_scale_meters,
                              (asset_contract or {}).get(asset_id), asset_id=asset_id),
    }


def compile_scene_json(
    build_spec: BuildSpec,
    manifest: AssetManifest,
    plan: MechanicPlan,
    scene: SceneSpec,
    normalization_report: dict[str, dict[str, Any]] | None = None,
    asset_contract: dict[str, dict[str, Any]] | None = None,
    on_warning: Callable[[str], None] | None = None,
) -> dict[str, Any]:
    """Pure function: produce the engine-loadable scene JSON dict.

    Raises ``SocketContractError`` when a bound mechanic declares a
    ``required_sockets`` entry the asset's contract cannot supply -- see
    :func:`_check_required_sockets`.

    ``on_warning`` receives findings that are worth saying out loud but must
    not fail the build, so the caller can route them to the event bus.
    """

    asset_by_id = {a.id: a for a in manifest.items}
    # Index plan items by BOTH the mechanic-plan id and the code_task id so a
    # scene binding of either "builtin.<x>" or "generated.<code_task_id>"
    # resolves back to its plan item (needed for config + binding merge).
    plan_by_id: dict[str, Any] = {}
    for it in plan.items:
        plan_by_id[it.id] = it
        if it.code_task is not None:
            plan_by_id[it.code_task.id] = it
    nrep = normalization_report or {}
    contracts = asset_contract or {}
    referenced_asset_ids: set[str] = set()
    socket_violations: list[str] = []
    # Deferred until every entity (including the auto-instantiated safety-net
    # ones) is known, because the question is about the whole scene.
    socket_needs: list[_SocketNeed] = []
    # Same reason: the equip binding is on the item, the slot is on the
    # wearer, and the wearer may be authored later in the list.
    equip_needs: list[_EquipNeed] = []

    # A mesh naming an asset the manifest has never heard of used to fall
    # through to ``e.mesh.glb_path``, emitting a path to a GLB that was never
    # generated -- an engine-side load failure at launch, from a stage that
    # reported success. The only way that happens is a SceneSpec composed
    # against a different manifest, so say so here and stop.
    unknown_meshes = sorted(
        {
            e.mesh.asset_id
            for e in scene.entities
            if e.mesh is not None and e.mesh.asset_id not in asset_by_id
        }
    )
    if unknown_meshes:
        raise SceneAssetMismatchError(
            f"scene '{scene.scene_id}' references {len(unknown_meshes)}"
            f" asset(s) that are not in the AssetManifest: {unknown_meshes}."
            "\n\nThis is a SceneSpec composed against an older asset set."
            f" Delete projects/<id>/scene_spec.{scene.scene_id}.json and"
            " re-run the scene_spec stage to recompose it against the"
            " current manifest."
        )

    entities_out: list[dict[str, Any]] = []
    for e in scene.entities:
        mesh: dict[str, Any] | None = None
        if e.mesh is not None:
            asset = asset_by_id.get(e.mesh.asset_id)
            norm = nrep.get(e.mesh.asset_id, {})
            scale_m = asset.target_scale_meters if asset is not None else 1.0
            mesh = {
                "asset_id": e.mesh.asset_id,
                "glb_path": norm.get("glb_path") or e.mesh.glb_path,
                "target_scale_meters": scale_m,
                "collision_shape": (
                    asset.collision_shape if asset is not None else "aabb"
                ),
                **_mesh_render_fields(
                    norm, scale_m, contracts.get(e.mesh.asset_id),
                    asset_id=e.mesh.asset_id,
                ),
            }
            referenced_asset_ids.add(e.mesh.asset_id)
        bindings: list[dict[str, Any]] = []
        for b in e.mechanic_bindings:
            item = plan_by_id.get(b.mechanic_id.split(".", 1)[-1])
            resolved: dict[str, Any] = {
                "id": b.mechanic_id,
                "params": dict(b.params),
            }
            if item is not None:
                if item.classification in (
                    MechanicClassification.builtin,
                    MechanicClassification.builtin_config,
                ):
                    resolved["builtin_id"] = item.builtin_id or b.mechanic_id
                    # A SceneSpec composed while this item was still a
                    # generated copy names "generated.<id>"; nothing by that
                    # name is built any more.
                    if (item.builtin_id and item.code_task is None
                            and b.mechanic_id.startswith("generated.")):
                        resolved["id"] = item.builtin_id
                # Merge item.config with per-entity params; per-entity wins.
                # (config is typically empty for builtin / generate, non-empty
                # for builtin+config and code_edit tuning.)
                merged = dict(item.config)
                merged.update(resolved["params"])
                resolved["params"] = merged
                # Materialize the resolved controller binding into params so
                # the running mechanic reads its control/hand at on_attach and
                # checks the live InputSnapshot. Per-entity params win.
                if (
                    item.binding is not None
                    and item.binding.control.value != "none"
                ):
                    resolved["params"].setdefault(
                        "control", item.binding.control.value
                    )
                    resolved["params"].setdefault(
                        "hand", item.binding.hand.value
                    )
                violations, deferred = _check_required_sockets(
                    e, item, mesh, contracts
                )
                socket_violations.extend(violations)
                if deferred is not None:
                    socket_needs.append(deferred)
            equip_need = _equip_need(e, b, resolved, item)
            if equip_need is not None:
                equip_needs.append(equip_need)
            bindings.append(resolved)
        entities_out.append(
            {
                "id": e.id,
                "name": e.name,
                "transform": {
                    "position": [
                        e.transform.position.x,
                        e.transform.position.y,
                        e.transform.position.z,
                    ],
                    "rotation": [
                        e.transform.rotation.x,
                        e.transform.rotation.y,
                        e.transform.rotation.z,
                        e.transform.rotation.w,
                    ],
                    "scale": e.transform.scale,
                },
                "mesh": mesh,
                "physics": (
                    e.physics.model_dump(mode="json") if e.physics is not None else None
                ),
                "mechanic_bindings": bindings,
                "tags": list(e.tags),
                **(
                    {"health": e.health.model_dump(mode="json")}
                    if e.health is not None
                    else {}
                ),
                **(
                    {"faction": e.faction.model_dump(mode="json")}
                    if e.faction is not None
                    else {}
                ),
                **(
                    {
                        "attached_to": {
                            "entity_id": e.attached_to.entity_id,
                            "anchor": e.attached_to.anchor,
                            "position": [
                                e.attached_to.position.x,
                                e.attached_to.position.y,
                                e.attached_to.position.z,
                            ],
                            "rotation": [
                                e.attached_to.rotation.x,
                                e.attached_to.rotation.y,
                                e.attached_to.rotation.z,
                                e.attached_to.rotation.w,
                            ],
                        }
                    }
                    if e.attached_to is not None
                    else {}
                ),
                **(
                    {"equip_slots": [_equip_slot_json(s) for s in e.equip_slots]}
                    if e.equip_slots
                    else {}
                ),
            }
        )

    # ---- Safety net: every AssetItem MUST appear in the compiled scene. ----
    # If the LLM-generated SceneSpec dropped an asset, auto-instantiate a
    # category-appropriate default so the .glb still ships into the running
    # game. This is what makes "the generated city is the world the player
    # spawns in" robust even when the planner forgets.
    auto_index = 0
    for asset in manifest.items:
        if asset.id in referenced_asset_ids:
            continue
        glb_path = _resolve_glb_path(asset.id, None, nrep)
        if not glb_path:
            # No .glb on disk yet (e.g. the Tripo stage failed for this
            # asset). Skip rather than emit a dangling mesh path that the
            # engine's GLB loader would reject.
            continue
        entities_out.append(
            _default_entity_for_asset(
                asset,
                glb_path,
                auto_index,
                nrep.get(asset.id, {}),
                contracts.get(asset.id),
            )
        )
        # An auto-instantiated asset is in the scene like any other, so it
        # counts as a socket provider for the deferred checks below.
        referenced_asset_ids.add(asset.id)
        auto_index += 1

    # Every player mechanic is bound explicitly -- the engine injects none.
    # Hands Only also drops any player-avatar entity here.
    bind_player_roles(entities_out, plan, build_spec, on_warning)
    _apply_hold_offsets(entities_out, asset_by_id)

    # Now that every mesh in the scene is known, answer the checks that could
    # not be answered entity-locally.
    deferred_violations, deferred_warnings = _check_deferred_socket_needs(
        socket_needs, contracts, referenced_asset_ids
    )
    socket_violations.extend(deferred_violations)
    if on_warning is not None:
        for warning in deferred_warnings:
            on_warning(warning)
    hold_errors, hold_warnings = hold_violations(entities_out)
    socket_violations.extend(hold_errors)
    if on_warning is not None:
        for warning in hold_warnings:
            on_warning(warning)

    equip_violations = _check_equip_slots(entities_out, equip_needs)
    if equip_violations:
        raise EquipSlotContractError(
            "mechanics can equip items into slots the scene never declared:\n"
            + "\n".join(f"  - {v}" for v in equip_violations)
            + "\n\nAdd the slot to the wearer's `equip_slots` in the SceneSpec"
            " (body_slot 'waist' for a hip holster, 'back' for a sling), or"
            " drop the builtin.equip binding."
        )

    # Fail loudly, and all at once. Reporting every violation in a single
    # raise means one fix-up round instead of N build-fail cycles.
    if socket_violations:
        raise SocketContractError(
            "generated mechanics require sockets their assets do not have:\n"
            + "\n".join(f"  - {v}" for v in socket_violations)
            + "\n\nEither add the socket name to the asset's `socket_hints` in"
            " the AssetManifest (so the asset_contract stage derives it), or"
            " drop it from the CodeTask's `required_sockets`."
        )

    return {
        "version": SCENE_JSON_VERSION,
        "prompt_id": scene.prompt_id,
        "scene_id": scene.scene_id,
        "title": build_spec.title,
        "name": scene.name,
        "player": {
            "height_meters": build_spec.player_height_meters,
            "locomotion": build_spec.locomotion.value,
            "comfort": build_spec.comfort.value,
            **player_block_fields(build_spec, entities_out, plan),
            "spawn": {
                "position": [
                    scene.player_spawn.transform.position.x,
                    scene.player_spawn.transform.position.y,
                    scene.player_spawn.transform.position.z,
                ],
                "rotation": [
                    scene.player_spawn.transform.rotation.x,
                    scene.player_spawn.transform.rotation.y,
                    scene.player_spawn.transform.rotation.z,
                    scene.player_spawn.transform.rotation.w,
                ],
            },
        },
        "sky_color": list(scene.sky_color),
        "ambient_intensity": scene.ambient_intensity,
        "lights": [
            {
                "id": l.id,
                "kind": l.kind,
                "transform": {
                    "position": [
                        l.transform.position.x,
                        l.transform.position.y,
                        l.transform.position.z,
                    ],
                    "rotation": [
                        l.transform.rotation.x,
                        l.transform.rotation.y,
                        l.transform.rotation.z,
                        l.transform.rotation.w,
                    ],
                    "scale": l.transform.scale,
                },
                "color_rgb": list(l.color_rgb),
                "intensity": l.intensity,
            }
            for l in scene.lights
        ],
        "entities": entities_out,
        # Device budget + runtime quality policy resolved by the
        # asset_optimization stage. The engine reads this to configure MSAA,
        # foveation, culling, LOD switching and the adaptive governor. Absent
        # for projects compiled before that stage existed, in which case the
        # runtime falls back to safe built-in defaults.
        **({"performance": nrep["__performance__"]} if "__performance__" in nrep else {}),
    }


def sanitize_physics(
    entities: list[dict],
    on_warning: Callable[[str], None] | None = None,
) -> int:
    """Repair physics values the engine cannot accept. Returns how many.

    Applied to the FINAL entity list rather than inside
    :func:`compile_scene_json`, because that is the only point where all three
    sources of entities meet: the SceneSpec (schema-validated), the world
    builder's C++ output (merged verbatim as raw dicts, so nothing has
    validated it), and this stage's own asset safety net.

    Today this enforces one rule: a ``dynamic`` or ``kinematic`` body must
    weigh more than nothing. Jolt builds an inverse inertia tensor by dividing
    by the mass and asserts on a non-positive one, and a failed assert is a
    breakpoint -- SIGTRAP on a headset. Three kinematic popup targets authored
    with ``mass_kg: 0.0`` took down a shipped Quest APK during scene load,
    showing no error at all: the app just returned to the home environment.

    The engine clamps this too. Both exist on purpose -- the engine can never
    trust its input, and fixing it here means the compiled scene.json on disk
    says what the game will actually do.
    """
    fixed = 0
    for entity in entities:
        physics = entity.get("physics")
        if not isinstance(physics, dict):
            continue
        if physics.get("kind") not in ("dynamic", "kinematic"):
            continue
        try:
            mass = float(physics.get("mass_kg", 0.0))
        except (TypeError, ValueError):
            mass = 0.0
        if mass > 0.0:
            continue
        physics["mass_kg"] = 1.0
        fixed += 1
        if on_warning is not None:
            on_warning(
                f"entity '{entity.get('id')}' is"
                f" '{physics.get('kind')}' with mass_kg={mass}; raised to 1.0."
                " A movable body needs a positive mass -- use kind 'static'"
                " for something that never moves."
            )
    return fixed


def count_animated_agents(entities: list[dict]) -> int:
    """Entities in a compiled scene that will be posed every frame.

    A skinned mesh that is not a hidden spawn template gets a skeleton at
    load and costs CPU from the first frame. Counted separately from
    triangles because a scene can sit comfortably inside its triangle budget
    and still miss frame rate on thirty skeletons.
    """
    n = 0
    for e in entities:
        mesh = e.get("mesh") or {}
        if not mesh.get("skinned"):
            continue
        if "template" in (e.get("tags") or []):
            continue
        n += 1
    return n


def check_animated_agent_budget(
    entities: list[dict], performance: dict | None
) -> str:
    """Warn when a scene places more animated characters than it can afford.

    Deliberately a warning rather than a hard failure: the honest fix is
    fewer concurrent NPCs, which is a design decision, and the runtime
    already refuses to SPAWN past the cap. Failing the build here would
    reject a scene that is merely ambitious about its opening shot.
    """
    if not performance:
        return ""
    cap = int(performance.get("max_animated_agents") or 0)
    if cap <= 0:
        return ""
    placed = count_animated_agents(entities)
    if placed <= cap:
        return ""
    return (
        f"{placed} animated characters placed in the scene but the "
        f"{performance.get('tier', 'current')} budget allows {cap}; expect "
        "frame drops. Spawn the extras from a hidden 'template' entity "
        "instead of placing them all at load."
    )


def _repoint_world_meshes_to_lods(
    entities: list[dict],
    nrep: dict[str, dict[str, Any]],
    contracts: dict[str, dict[str, Any]] | None = None,
    manifest_scales: dict[str, float] | None = None,
) -> list[dict]:
    """Point world-generated mesh entities at the optimized LOD chain.

    ``world_generation`` runs BEFORE ``asset_optimization``, so any prop the
    world builder placed still references the raw Tripo GLB. Those entities are
    merged into the scene verbatim, bypassing ``compile_scene_json``'s mesh
    mapping -- without this remap a single world-placed prop would drag a
    500k-triangle mesh onto the headset.

    The size comes from the manifest, not from what the world builder wrote:
    the same asset placed by the world and by the SceneSpec must be the same
    size (a world rack at 1.0 m beside a gameplay rack at 1.15 m).
    """

    scales = manifest_scales or {}
    out: list[dict] = []
    for entity in entities:
        mesh = entity.get("mesh")
        if not isinstance(mesh, dict) or not mesh.get("asset_id"):
            out.append(entity)
            continue
        asset_id = mesh["asset_id"]
        scale_m = float(
            scales.get(asset_id) or mesh.get("target_scale_meters") or 1.0
        )
        norm = nrep.get(asset_id) or {}
        merged = dict(entity)
        new_mesh = {**mesh, "target_scale_meters": scale_m}
        if norm.get("lod_paths"):
            new_mesh["glb_path"] = norm["glb_path"]
            new_mesh.update(
                _mesh_render_fields(
                    norm, scale_m, (contracts or {}).get(asset_id), asset_id=asset_id
                )
            )
        merged["mesh"] = new_mesh
        out.append(merged)
    return out


#: A world copy this close to a gameplay entity of the same asset is the same
#: object placed twice.
_DUPLICATE_RADIUS_M = 0.5


def reconcile_world_copies(
    world_entities: list[dict],
    gameplay_entities: list[dict],
    on_warning: Callable[[str], None] | None = None,
) -> list[dict]:
    """Make world-placed copies of gameplay objects behave like the originals.

    The world builder dresses the environment and sometimes places an asset
    the SceneSpec already made interactive -- a pistol on a table that turned
    out not to be grabbable, next to the one that was. Equivalent objects
    must behave the same everywhere (the BuildSpec INTERACTION RULES default),
    so a world copy inherits the gameplay entity's bindings, physics and
    interaction tags. A copy sitting on top of the gameplay entity is the
    same object placed twice and is dropped.
    """
    warn = on_warning or (lambda _m: None)
    templates: dict[str, dict] = {}
    positions: dict[str, list[list[float]]] = {}
    for e in gameplay_entities:
        mesh = e.get("mesh") or {}
        aid = mesh.get("asset_id")
        if not aid:
            continue
        pos = ((e.get("transform") or {}).get("position")) or [0.0, 0.0, 0.0]
        positions.setdefault(aid, []).append([float(v) for v in pos[:3]])
        if e.get("mechanic_bindings") and aid not in templates:
            templates[aid] = e

    out: list[dict] = []
    for w in world_entities:
        mesh = w.get("mesh") or {}
        aid = mesh.get("asset_id")
        if not aid or aid not in positions:
            out.append(w)
            continue
        wpos = ((w.get("transform") or {}).get("position")) or [0.0, 0.0, 0.0]
        near = any(
            sum((float(a) - float(b)) ** 2 for a, b in zip(wpos[:3], p)) ** 0.5
            < _DUPLICATE_RADIUS_M
            for p in positions[aid]
        )
        if near:
            warn(
                f"world entity '{w.get('id')}' duplicates gameplay asset"
                f" '{aid}' at the same spot; dropped the world copy"
            )
            continue
        tmpl = templates.get(aid)
        if tmpl is not None and not w.get("mechanic_bindings"):
            w = dict(w)
            w["mechanic_bindings"] = [
                json.loads(json.dumps(b))
                for b in tmpl.get("mechanic_bindings") or []
                if isinstance(b, dict)
                and not role_of(str(b.get("builtin_id") or b.get("id") or ""))
            ]
            if tmpl.get("physics") is not None:
                w["physics"] = json.loads(json.dumps(tmpl["physics"]))
            tags = list(w.get("tags") or [])
            for t in tmpl.get("tags") or []:
                if t in ("grabbable", "interactable", "equippable", "weapon",
                         "target") and t not in tags:
                    tags.append(t)
            w["tags"] = tags
            warn(
                f"world entity '{w.get('id')}' is a copy of interactive asset"
                f" '{aid}'; gave it the same bindings and physics as"
                f" '{tmpl.get('id')}'"
            )
        out.append(w)
    return out


# ---------------------------------------------------------------------------
# The affordance binder
# ---------------------------------------------------------------------------
#
# Turns each interactive the world builder declared into a real mechanic
# binding on the entity that carries it. This is deliberately a TABLE applied
# uniformly rather than a fourth LLM stage, and the reason is the property the
# design most needs: equivalent objects must behave the same way in every
# environment of a game. An agent reading six environments and all their
# mechanics is exactly the thing that wires grass in the hub and forgets it in
# the mission. A lookup applied to every environment cannot forget.
#
# Intentional divergence still has a home -- BuildSpec.interaction_rules --
# so a difference is always explicit and reviewable rather than an accident.

AFFORDANCE_MECHANICS: dict[str, str] = {
    "door": "builtin.door",
    "switchable": "builtin.switchable",
    "container": "builtin.container",
    "mover": "builtin.mover",
    "breakable": "builtin.breakable",
    "harvestable": "builtin.harvestable",
    # 'movable' is intentionally absent: it declares that an object's
    # transform should persist, which the engine handles from the declaration
    # itself. builtin.grab already provides the interaction.
    "movable": "",
}


def _interaction_denied(
    build_spec: Any, scene_id: str, kind: str, tags: list[str]
) -> bool:
    """Does a prompt-specified rule forbid interacting with this here?

    Matches on the affordance kind or any of its tags, so a rule can be
    written against either vocabulary ("no harvestable in the hub" or "no
    grabbable in the hub").
    """

    for rule in getattr(build_spec, "interaction_rules", None) or []:
        if rule.scene_id != scene_id:
            continue
        if rule.affordance != kind and rule.affordance not in tags:
            continue
        if not rule.allowed:
            return True
    return False


def bind_affordances(
    doc: dict, build_spec: Any, scene_id: str
) -> tuple[int, list[str]]:
    """Attach affordance mechanics to the entities that declared them.

    Returns ``(bound_count, notes)``. Mutates ``doc`` in place: each matching
    entity gains a ``mechanic_bindings`` entry, and an interactive a policy
    forbids is marked so the runtime refuses the interaction AND suppresses
    its affordance cue -- advertising an action that will be refused is worse
    than showing no cue at all.
    """

    interactives = doc.get("interactives")
    if not isinstance(interactives, list) or not interactives:
        return 0, []

    by_id: dict[str, dict] = {}
    for entity in doc.get("entities") or []:
        if isinstance(entity, dict) and entity.get("id"):
            by_id[str(entity["id"])] = entity

    bound = 0
    notes: list[str] = []
    for spec in interactives:
        if not isinstance(spec, dict):
            continue
        interactive_id = str(spec.get("id") or "")
        kind = str(spec.get("kind") or "")
        if not interactive_id or not kind:
            continue

        tags = [str(t) for t in (spec.get("tags") or [])]
        if _interaction_denied(build_spec, scene_id, kind, tags):
            # The mechanic is still bound; only permission changes. The
            # player performs the identical gesture in both environments and
            # the policy decides whether it succeeds, which is how the
            # requirement was framed.
            spec["interaction_allowed"] = False
            notes.append(f"{interactive_id}: {kind} denied by interaction_rules")

        mechanic_id = AFFORDANCE_MECHANICS.get(kind)
        if mechanic_id is None:
            notes.append(f"{interactive_id}: unknown affordance kind '{kind}'")
            continue
        if not mechanic_id:
            continue  # declaration-only kind, e.g. 'movable'

        entity = by_id.get(interactive_id)
        if entity is None:
            # The D8 validator reports this as a hard error; here we simply
            # do not invent a binding for geometry that is not present.
            notes.append(
                f"{interactive_id}: no entity with that id, not bound"
            )
            continue

        bindings = entity.setdefault("mechanic_bindings", [])
        if any(
            isinstance(b, dict) and b.get("id") == mechanic_id
            for b in bindings
        ):
            continue  # already bound, e.g. by an explicit SceneSpec binding

        params: dict[str, Any] = {}
        # Per-instance parameters come from the declaration, which is the
        # whole reason a tag could not have done this job: a tag is a bare
        # string and cannot carry a hinge axis.
        if kind == "harvestable":
            params["reach_m"] = 0.5
        bindings.append(
            {"id": mechanic_id, "builtin_id": mechanic_id, "params": params}
        )
        bound += 1

    return bound, notes


def _merge_world_into_scene(
    doc: dict,
    world_doc: dict,
    nrep: dict[str, dict[str, Any]] | None = None,
    contracts: dict[str, dict[str, Any]] | None = None,
    manifest: AssetManifest | None = None,
    on_warning: Callable[[str], None] | None = None,
    spec_yaw: float | None = None,
) -> dict:
    """Merge the C++-generated environment (world.scene.json) into a compiled
    gameplay scene doc: environment entities + sky + lights + spawn come from
    the world; gameplay entities render on top of it.

    When the world starts the player somewhere other than where the SceneSpec
    composed the gameplay layer, the layer moves with the player (see
    gameplay_relocation). ``spec_yaw`` is the heading the SceneSpec composed
    around; by default it is read from the compiled spawn rotation."""
    scales = (
        {a.id: float(a.target_scale_meters) for a in manifest.items}
        if manifest is not None
        else None
    )
    world_player = world_doc.get("player") or {}
    world_spawn = (
        world_player.get("spawn") if isinstance(world_player, dict) else None
    )
    w_pos, w_yaw = spawn_pose(world_spawn)
    s_pos, s_yaw = spawn_pose((doc.get("player") or {}).get("spawn"))
    if w_pos is not None and s_pos is not None:
        relocate_gameplay_layer(
            list(doc.get("entities") or []),
            s_pos,
            spec_yaw if spec_yaw is not None else (s_yaw or 0.0),
            w_pos,
            w_yaw or 0.0,
            on_warning,
        )
    world_nav = world_doc.get("navigation")
    regions = (
        list(world_nav.get("regions") or [])
        if isinstance(world_nav, dict) else []
    )
    resolve_overlaps(
        list(doc.get("entities") or []),
        list(world_doc.get("entities") or []),
        regions,
        on_warning,
    )
    world_entities = _repoint_world_meshes_to_lods(
        list(world_doc.get("entities") or []), nrep or {}, contracts, scales
    )
    world_entities = reconcile_world_copies(
        world_entities, list(doc.get("entities") or []), on_warning
    )
    doc["entities"] = list(world_entities) + list(doc.get("entities") or [])
    if "sky" in world_doc:
        doc["sky"] = world_doc["sky"]
    if "sky_color" in world_doc:
        doc["sky_color"] = world_doc["sky_color"]
    if "ambient_intensity" in world_doc:
        doc["ambient_intensity"] = world_doc["ambient_intensity"]
    world_lights = world_doc.get("lights") or []
    if world_lights:
        doc["lights"] = list(world_lights) + list(doc.get("lights") or [])
    # Walkable topology the builder declared while placing the level. Without
    # it NPCs only have local steering, which cannot route through a doorway.
    if isinstance(world_nav, dict) and world_nav.get("regions"):
        doc["navigation"] = world_nav
    if isinstance(world_spawn, dict) and "position" in world_spawn:
        doc.setdefault("player", {})
        doc["player"]["spawn"] = world_spawn

    # Declared state, exits and named arrival points travel with the
    # environment that declared them.
    for key in ("interactives", "spawn_points", "portals", "npc_spawn_zones",
                "npc_anchors"):
        if world_doc.get(key):
            doc[key] = world_doc[key]

    # Provenance. The merge used to drop this, which meant a compiled scene
    # carried an environment with no record of where it came from -- and with
    # several environments per project, that is the difference between
    # debugging a bad world and guessing which agent produced it.
    if world_doc.get("builder_id"):
        doc["environment_builder_id"] = world_doc["builder_id"]
    return doc


def run(ctx: PipelineContext) -> StageResult:
    ctx.emit(ProjectStage.scene_compile, "running", "Compiling scene JSON")
    state = ctx.store.load_state(ctx.project_id)
    ctx.store.record_stage(state, ProjectStage.scene_compile, ProjectStatus.running)

    build_spec = ctx.build_spec or ctx.store.load_build_spec(ctx.project_id)
    manifest = ctx.asset_manifest or ctx.store.load_asset_manifest(ctx.project_id)
    plan = ctx.mechanic_plan or ctx.store.load_mechanic_plan(ctx.project_id)

    norm_path = ctx.project_dir / "asset_normalization.json"
    normalization = (
        json.loads(norm_path.read_text(encoding="utf-8"))
        if norm_path.exists()
        else {}
    )

    # Named sockets derived from geometry by the asset_contract stage. Absent
    # for projects generated before that stage existed, in which case every
    # mesh simply ships without sockets and the validator stays inert.
    contract_path = ctx.project_dir / "asset_contract.json"
    asset_contract = (
        json.loads(contract_path.read_text(encoding="utf-8"))
        if contract_path.exists()
        else {}
    )

    # The environment merges into the entry-point scene (the launched scene.json).
    entry_id: str | None = next(
        (s.id for s in build_spec.scenes if s.entry_point), None
    )
    if entry_id is None and build_spec.scenes:
        entry_id = build_spec.scenes[0].id

    def _load_world(scene_id: str) -> dict | None:
        """The generated environment for one scene.

        Looks for ``world.<scene_id>.scene.json`` first, so every scene can
        have its own environment, and falls back to the single project-wide
        ``world.scene.json`` for projects generated before that was true.
        A scene that reuses a sibling's environment is resolved through
        ``reuse_environment_from``, because ``needs_unique_environment:
        false`` on its own never said WHICH environment was being borrowed.
        """

        entry = next(
            (s for s in build_spec.scenes if s.id == scene_id), None
        )
        source = scene_id
        if entry is not None:
            borrowed = getattr(entry, "reuse_environment_from", None)
            if borrowed:
                source = borrowed

        candidates = [
            ctx.project_dir / f"world.{source}.scene.json",
            ctx.project_dir / "world.scene.json",
        ]
        for path in candidates:
            if not path.exists():
                continue
            try:
                return json.loads(path.read_text(encoding="utf-8"))
            except Exception:
                # A malformed world document must not take the whole compile
                # down: the scene still builds, just without its environment,
                # and the stage reports it.
                ctx.emit(
                    ProjectStage.scene_compile,
                    "warning",
                    f"{scene_id}: could not parse {path.name}",
                )
                return None
        return None

    builds_dir = ctx.project_dir / "builds" / "scene"
    builds_dir.mkdir(parents=True, exist_ok=True)
    artifacts: list[str] = []

    try:
        npc_plan = ctx.npc_plan or ctx.store.load_npc_plan(ctx.project_id)
    except Exception:  # noqa: BLE001 - projects from before the NPC plan
        npc_plan = None
    # None for projects from before the game_design stage: they keep the run
    # flow their own generated code provides.
    game_design = ctx.game_design or ctx.store.load_game_design(ctx.project_id)
    # None for projects from before interaction_design: their generated
    # weapons stay as they were.
    interaction_design = (ctx.interaction_design
                          or ctx.store.load_interaction_design(ctx.project_id))
    known_generated = {
        x for it in plan.items if it.code_task is not None
        for x in (it.id, it.code_task.id)
    }
    # Does anything start a "signal"-started encounter? None without engine
    # source (test fixtures): unknown, kept as planned.
    start_signal_sent = _generated_calls(ctx.settings.repo_root, ctx.project_id,
                                         r"encounters\s*->\s*start\s*\(")
    # Projectiles drawn at the size of the bore they leave (and arrows longer
    # than the draw); the templates below are built from the corrected sizes.
    proj_scales, scale_notes = projectile_scale_corrections(
        interaction_design, manifest, asset_contract)
    for note in scale_notes:
        ctx.emit(ProjectStage.scene_compile,
                 "warning" if "rescaled" in note or "lengthened" in note else "info",
                 f"projectile scale: {note}"[:300])
        ctx.store.append_log(ctx.project_id, "scene_compile", f"projectile scale: {note}")
    weapon_manifest = apply_scales(manifest, proj_scales)
    drawn_fx_assets = engine_drawn_fx_assets(manifest, interaction_design)
    if npc_plan is not None and npc_plan.npcs:
        # Same deterministic repairs the npc_plan stage applies, so a plan
        # written before encounters existed still gets its companion placed
        # and its waves run.
        npc_plan, repairs = repair_npc_plan(npc_plan, build_spec)
        for note in repairs:
            ctx.emit(ProjectStage.scene_compile, "info", f"NPC plan: {note}")
    # Generated mechanics that drive an NPC's movement: an NPC bound to one
    # keeps it instead of also getting a built-in behaviour.
    generated_ai_ids = {
        f"generated.{it.code_task.id}"
        for it in plan.items
        if it.code_task is not None
        and getattr(it.code_task.kind, "value", str(it.code_task.kind)) == "ai"
    }
    # What this build actually registers. Empty only without engine source
    # (test fixtures), in which case the resolution checks are skipped.
    registered = registered_ids(ctx.settings.repo_root, ctx.project_id)
    unresolved_all: list[str] = []
    missing_npcs: dict[str, list[str]] = {}

    # Accumulated, not written as we go: every scene's document needs the
    # index, and the index is not known until every scene has been compiled.
    compiled: list[tuple[str, dict]] = []
    entry_scene_json: str | None = None
    entry_id_written: str | None = None
    for scene_entry in build_spec.scenes:
        spec_path = ctx.project_dir / f"scene_spec.{scene_entry.id}.json"
        if not spec_path.exists():
            continue
        scene_spec = SceneSpec.model_validate(
            json.loads(spec_path.read_text(encoding="utf-8"))
        )
        doc = compile_scene_json(
            build_spec,
            manifest,
            plan,
            scene_spec,
            normalization,
            asset_contract,
            on_warning=lambda m, sid=scene_entry.id: ctx.emit(
                ProjectStage.scene_compile, "warning", f"{sid}: {m}"
            ),
        )
        fd = scene_spec.player_spawn.face_direction
        spec_yaw = yaw_from_direction([fd.x, fd.y, fd.z])
        # The NPC population: one hidden template per spawned type, companions
        # beside the player, per-type stats and behaviours, and the encounter
        # director. In the SceneSpec's frame, so the relocation below carries
        # companions to wherever the world starts the player.
        npc_assets = {n.asset_id for n in (npc_plan.npcs if npc_plan else [])}
        scene_has_npcs = scene_entry.id == entry_id or any(
            e.mesh is not None and e.mesh.asset_id in npc_assets
            for e in scene_spec.entities
        )
        pop: dict = {}
        if npc_plan is not None and scene_has_npcs:
            # Generated AI that never made it into the build would leave its
            # NPC standing still; drop it so the built-in behaviour binds.
            hosted_ai = generated_ai_ids
            if registered:
                hosted_ai, ai_notes = drop_unhosted_npc_ai(
                    doc.get("entities") or [], registered, generated_ai_ids)
                for note in ai_notes:
                    ctx.emit(ProjectStage.scene_compile, "warning",
                             f"{scene_entry.id}: {note}")
            pop = apply_npc_population(
                doc, npc_plan, hosted_ai, spec_yaw=spec_yaw,
                start_signal_sent=start_signal_sent,
                on_warning=(
                    (lambda m, sid=scene_entry.id: ctx.emit(
                        ProjectStage.scene_compile, "warning", f"{sid}: {m}"))
                    if scene_entry.id == entry_id else None
                ),
            )
            if pop.get("templates") or pop.get("persistent"):
                ctx.store.append_log(
                    ctx.project_id, "scene_compile",
                    f"{scene_entry.id}: NPC templates {pop['templates']}, "
                    f"placed {pop['persistent']}, built-in behaviours "
                    f"{pop['behaviours']}",
                )
            if pop.get("missing"):
                missing_npcs[scene_entry.id] = list(pop["missing"])

        # Every scene gets its own environment now, not just the entry point.
        # Previously the other scenes compiled to gameplay entities floating
        # in an empty void, which is what a six-scene project actually
        # produced.
        world_doc = _load_world(scene_entry.id)
        if world_doc is not None:
            doc = _merge_world_into_scene(
                doc, world_doc, normalization, asset_contract, manifest,
                on_warning=lambda m, sid=scene_entry.id: ctx.emit(
                    ProjectStage.scene_compile, "warning", f"{sid}: {m}"
                ),
                spec_yaw=spec_yaw,
            )
        # Placed-at-load NPCs, checked against the merged world itself.
        if npc_plan is not None and pop.get("persistent"):
            for note in place_persistent_npcs(
                doc, pop["persistent"], npc_plan,
                on_warning=lambda m, sid=scene_entry.id: ctx.emit(
                    ProjectStage.scene_compile, "warning", f"{sid}: {m}"),
            ):
                ctx.emit(ProjectStage.scene_compile, "info",
                         f"{scene_entry.id}: {note}")
                ctx.store.append_log(ctx.project_id, "scene_compile",
                                     f"{scene_entry.id}: {note}")
        zones = finalize_encounter_zones(
            doc,
            on_warning=lambda m, sid=scene_entry.id: ctx.emit(
                ProjectStage.scene_compile, "warning", f"{sid}: {m}"
            ),
        )
        if zones:
            ctx.emit(ProjectStage.scene_compile, "info",
                     f"{scene_entry.id}: encounter director has {zones} spawn zone(s)")

        # The game loop: one director with the flow, its panel and the HUD,
        # then a static check that every path through it can complete here.
        flow = apply_game_flow(
            doc, game_design,
            on_warning=lambda m, sid=scene_entry.id: ctx.emit(
                ProjectStage.scene_compile, "warning", f"{sid}: {m}"),
        )
        flow_problems = game_flow_violations(doc, flow)
        if flow_problems:
            msg = f"{scene_entry.id}: game flow cannot complete: " + "; ".join(flow_problems)
            ctx.store.record_stage(state, ProjectStage.scene_compile,
                                   ProjectStatus.failed, message=msg[:400])
            ctx.emit(ProjectStage.scene_compile, "failed", msg[:400])
            raise GameFlowContractError(msg)
        if flow is not None:
            ctx.emit(ProjectStage.scene_compile, "info",
                     f"{scene_entry.id}: game flow bound ({flow['experience']}, "
                     f"lives {flow['lives']}, {len(flow['objectives'])} objective(s))")

        # Weapons: the interaction design decides which built-in runs each
        # one, what it fires and which NPCs shoot back.
        weapons = apply_weapons(
            doc, interaction_design, known_generated=known_generated,
            mesh_for=lambda aid: mesh_for_asset(aid, weapon_manifest, normalization,
                                                asset_contract),
            npc_plan=npc_plan if scene_has_npcs else None,
            on_warning=lambda m, sid=scene_entry.id: ctx.emit(
                ProjectStage.scene_compile, "warning", f"{sid}: {m}"),
            drawn_fx_assets=drawn_fx_assets,
        )
        weapon_errors, weapon_warnings = weapon_violations(doc, interaction_design)
        for w in weapon_warnings:
            ctx.emit(ProjectStage.scene_compile, "warning", f"{scene_entry.id}: {w}")
        if weapon_errors:
            msg = (f"{scene_entry.id}: weapons cannot work: " + "; ".join(weapon_errors))
            ctx.store.append_log(ctx.project_id, "scene_compile", msg)
            ctx.store.record_stage(state, ProjectStage.scene_compile,
                                   ProjectStatus.failed, message=msg[:400])
            ctx.emit(ProjectStage.scene_compile, "failed", msg[:400])
            raise WeaponContractError(msg)
        if weapons.get("weapons") or weapons.get("npc_ranged"):
            note = (f"{scene_entry.id}: weapons {weapons.get('weapons', [])}, templates "
                    f"{weapons.get('templates', [])}, sources {weapons.get('sources', [])}, "
                    f"NPC ranged {weapons.get('npc_ranged', [])}")
            ctx.emit(ProjectStage.scene_compile, "info", note)
            ctx.store.append_log(ctx.project_id, "scene_compile", note)

        # Every entity is now present -- authored, world-merged and
        # auto-instantiated alike -- so this is the one place a physics value
        # can be checked once for all of them.
        repaired = sanitize_physics(
            doc.get("entities") or [],
            on_warning=lambda m, sid=scene_entry.id: ctx.emit(
                ProjectStage.scene_compile, "warning", f"{sid}: {m}"
            ),
        )
        if repaired:
            ctx.store.append_log(
                ctx.project_id,
                "scene_compile",
                f"{scene_entry.id}: repaired physics on {repaired} entity(ies)",
            )

        # Connect the declarations to the mechanics. Uniform across every
        # scene by construction, so equivalent objects cannot behave
        # differently in one environment than another unless a rule says so.
        bound, notes = bind_affordances(doc, build_spec, scene_entry.id)
        if bound:
            ctx.emit(
                ProjectStage.scene_compile,
                "info",
                f"{scene_entry.id}: bound {bound} affordance mechanic(s)",
            )
        for note in notes:
            ctx.emit(
                ProjectStage.scene_compile,
                "warning",
                f"{scene_entry.id}: {note}",
            )

        over_budget = check_animated_agent_budget(
            doc.get("entities") or [], doc.get("performance")
        )
        if over_budget:
            ctx.emit(
                ProjectStage.scene_compile,
                "warning",
                f"{scene_entry.id}: {over_budget}",
            )
            ctx.store.append_log(
                ctx.project_id, "scene_compile",
                f"{scene_entry.id}: {over_budget}",
            )
        if registered:
            unresolved_all.extend(
                f"{scene_entry.id}/{p}"
                for p in unresolved_bindings(doc.get("entities") or [], registered))
        compiled.append((scene_entry.id, doc))
        if scene_entry.entry_point or entry_id_written is None:
            entry_id_written = scene_entry.id

    # Textures: every LOD a scene places must resolve the texture files it
    # references, or the headset would draw that surface magenta / flat.
    tex_problems = _check_scene_textures(ctx, compiled, state)
    if tex_problems:
        msg = ("texture files referenced by placed meshes are missing (re-run "
               "asset_optimization): " + "; ".join(tex_problems[:12]))
        ctx.store.append_log(ctx.project_id, "scene_compile", msg)
        raise TextureContractError(msg)

    # What each scene costs to draw, and the starting settings that follow
    # from it (initial FFR, room culling, static batching) -- never for a
    # knob the user locked.
    _apply_scene_complexity(ctx, compiled)

    # Both checks name every problem at once, and both stop the stage: a
    # scene that ships either one loads fine and quietly lacks what the
    # prompt asked for.
    if missing_npcs:
        msg = (
            "NPCs the game requires have no model to place: "
            + "; ".join(f"{sid}: {ids}" for sid, ids in missing_npcs.items())
            + ". Re-run tripo_models / rigging for them before scene_compile."
        )
        ctx.store.append_log(ctx.project_id, "scene_compile", msg)
        raise MissingNpcError(msg)
    if unresolved_all:
        msg = (
            f"{len(unresolved_all)} mechanic binding(s) would do nothing at "
            "runtime:\n  " + "\n  ".join(unresolved_all)
            + "\nRe-run code_generation (or fix the mechanic plan) first."
        )
        ctx.store.append_log(ctx.project_id, "scene_compile", msg)
        raise UnresolvedBindingError(msg)

    # ------------------------------------------------------------------
    # The scene index.
    #
    # A portal names a target scene; the runtime has to turn that into a file
    # without knowing how a project is laid out, and it has to work identically
    # inside a Quest APK where there is nothing to go looking around in. So the
    # compiler writes down where every scene went and the engine just reads it.
    # Paths are relative to the document, so a project can be moved or unpacked
    # anywhere.
    #
    # Every scene gets the WHOLE index, not just its own exits. A menu that
    # offers a mission list, or a generated mechanic ending a chapter, needs to
    # reach a scene no portal in the current room points at.
    # ------------------------------------------------------------------
    index = {scene_id: f"scene.{scene_id}.json" for scene_id, _doc in compiled}
    # The Quest packager ships every scene.<id>.json beside scene.json; one
    # left by an earlier plan would ride along into the APK.
    from .run_reset import remove_stale_scene_files

    stale_scenes = remove_stale_scene_files(ctx.project_dir, "scene", index)
    if stale_scenes:
        ctx.emit(ProjectStage.scene_compile, "info",
                 f"removed compiled scenes no longer planned: {', '.join(stale_scenes)}")
    for scene_id, doc in compiled:
        doc["scene_index"] = index

        # Written beside scene.json, because that is what the index paths are
        # relative to and what the APK packages.
        runtime_path = ctx.project_dir / f"scene.{scene_id}.json"
        payload = json.dumps(doc, indent=2, sort_keys=True)
        atomic_write_text(runtime_path, payload)
        artifacts.append(
            str(runtime_path.relative_to(ctx.settings.repo_root)).replace("\\", "/")
        )

        # Kept for artifact continuity and debugging; the runtime does not read
        # from here.
        build_copy = builds_dir / f"{scene_id}.scene.json"
        build_copy.write_text(payload, encoding="utf-8")
        artifacts.append(
            str(build_copy.relative_to(ctx.settings.repo_root)).replace("\\", "/")
        )

        if scene_id == entry_id_written:
            entry_scene_json = payload

    # Canonical top-level scene.json used by the runtime: a copy of the entry
    # point, sitting alongside its siblings so the index resolves from it.
    if entry_scene_json is not None:
        top = ctx.project_dir / "scene.json"
        atomic_write_text(top, entry_scene_json)
        state.latest_scene_json = str(top)
        ctx.store.save_state(state)
        artifacts.append(str(top.relative_to(ctx.settings.repo_root)).replace("\\", "/"))

    ctx.store.record_stage(
        state,
        ProjectStage.scene_compile,
        ProjectStatus.succeeded,
        message=f"{len(artifacts)} scene file(s)",
        artifacts=artifacts,
    )
    ctx.emit(ProjectStage.scene_compile, "succeeded", f"{len(artifacts)} files")
    return StageResult(stage=ProjectStage.scene_compile, artifacts=artifacts)
