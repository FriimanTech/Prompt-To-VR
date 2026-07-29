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

from .context import PipelineContext, StageResult


SCENE_JSON_VERSION = 1


# ---------------------------------------------------------------------------
# Asset safety-net helpers
# ---------------------------------------------------------------------------


def _style_notes_lower(asset: AssetItem) -> str:
    return f"{asset.name} {asset.style_notes}".lower()


def _character_role(asset: AssetItem) -> str:
    """Heuristic: 'enemy', 'player_avatar', or 'npc' for a character asset."""
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


def _default_entity_for_asset(
    asset: AssetItem,
    glb_path: str,
    spread_index: int,
    norm: dict[str, Any] | None = None,
) -> dict[str, Any]:
    """Build a category-appropriate default entity for an unreferenced asset."""

    base_mesh = {
        "asset_id": asset.id,
        "glb_path": glb_path,
        "target_scale_meters": asset.target_scale_meters,
        "collision_shape": asset.collision_shape,
        **_mesh_render_fields(norm or {}, asset.target_scale_meters),
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


def _mesh_render_fields(
    norm: dict[str, Any],
    target_scale_meters: float,
) -> dict[str, Any]:
    """Per-mesh rendering metadata produced by ``asset_optimization``.

    ``lod_paths`` is the decimated LOD chain (coarser as the index grows) and
    ``bounds_radius_m`` is the world-space bounding-sphere radius the renderer
    uses for frustum + distance culling. Both are omitted when the asset has
    not been optimized, in which case the runtime falls back to drawing
    ``glb_path`` at full detail with a conservative radius.
    """

    out: dict[str, Any] = {}
    lod_paths = norm.get("lod_paths")
    if lod_paths:
        out["lod_paths"] = list(lod_paths)
    if norm.get("triangle_counts"):
        out["lod_triangle_counts"] = list(norm["triangle_counts"])
    unit_radius = norm.get("bounds_radius_m")
    if unit_radius:
        # The optimizer reports the radius of a mesh normalised to a 1 m
        # longest axis; the runtime rescales by target_scale_meters, so the
        # world-space radius scales identically.
        out["bounds_radius_m"] = float(unit_radius) * float(target_scale_meters)
    return out


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


def compile_scene_json(
    build_spec: BuildSpec,
    manifest: AssetManifest,
    plan: MechanicPlan,
    scene: SceneSpec,
    normalization_report: dict[str, dict[str, Any]] | None = None,
) -> dict[str, Any]:
    """Pure function: produce the engine-loadable scene JSON dict."""

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
    referenced_asset_ids: set[str] = set()

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
                **_mesh_render_fields(norm, scale_m),
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
                asset, glb_path, auto_index, nrep.get(asset.id, {})
            )
        )
        auto_index += 1

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


def _repoint_world_meshes_to_lods(
    entities: list[dict], nrep: dict[str, dict[str, Any]]
) -> list[dict]:
    """Point world-generated mesh entities at the optimized LOD chain.

    ``world_generation`` runs BEFORE ``asset_optimization``, so any prop the
    world builder placed still references the raw Tripo GLB. Those entities are
    merged into the scene verbatim, bypassing ``compile_scene_json``'s mesh
    mapping -- without this remap a single world-placed prop would drag a
    500k-triangle mesh onto the headset.
    """

    out: list[dict] = []
    for entity in entities:
        mesh = entity.get("mesh")
        if not isinstance(mesh, dict) or not mesh.get("asset_id"):
            out.append(entity)
            continue
        norm = nrep.get(mesh["asset_id"]) or {}
        if not norm.get("lod_paths"):
            out.append(entity)
            continue
        scale_m = float(mesh.get("target_scale_meters") or 1.0)
        merged = dict(entity)
        merged["mesh"] = {
            **mesh,
            "glb_path": norm["glb_path"],
            **_mesh_render_fields(norm, scale_m),
        }
        out.append(merged)
    return out


def _merge_world_into_scene(
    doc: dict, world_doc: dict, nrep: dict[str, dict[str, Any]] | None = None
) -> dict:
    """Merge the C++-generated environment (world.scene.json) into a compiled
    gameplay scene doc: environment entities + sky + lights + spawn come from
    the world; gameplay entities render on top of it."""
    world_entities = _repoint_world_meshes_to_lods(
        list(world_doc.get("entities") or []), nrep or {}
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
    world_player = world_doc.get("player") or {}
    world_spawn = (
        world_player.get("spawn") if isinstance(world_player, dict) else None
    )
    if isinstance(world_spawn, dict) and "position" in world_spawn:
        doc.setdefault("player", {})
        doc["player"]["spawn"] = world_spawn
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

    # The C++-generated environment for this project (from world_generation).
    world_doc: dict | None = None
    world_path = ctx.project_dir / "world.scene.json"
    if world_path.exists():
        try:
            world_doc = json.loads(world_path.read_text(encoding="utf-8"))
        except Exception:
            world_doc = None

    # The environment merges into the entry-point scene (the launched scene.json).
    entry_id: str | None = next(
        (s.id for s in build_spec.scenes if s.entry_point), None
    )
    if entry_id is None and build_spec.scenes:
        entry_id = build_spec.scenes[0].id

    builds_dir = ctx.project_dir / "builds" / "scene"
    builds_dir.mkdir(parents=True, exist_ok=True)
    artifacts: list[str] = []

    entry_scene_json: str | None = None
    for scene_entry in build_spec.scenes:
        spec_path = ctx.project_dir / f"scene_spec.{scene_entry.id}.json"
        if not spec_path.exists():
            continue
        scene_spec = SceneSpec.model_validate(
            json.loads(spec_path.read_text(encoding="utf-8"))
        )
        doc = compile_scene_json(build_spec, manifest, plan, scene_spec, normalization)
        if world_doc is not None and scene_entry.id == entry_id:
            doc = _merge_world_into_scene(doc, world_doc, normalization)
        out = builds_dir / f"{scene_entry.id}.scene.json"
        out.write_text(json.dumps(doc, indent=2, sort_keys=True), encoding="utf-8")
        artifacts.append(str(out.relative_to(ctx.settings.repo_root)).replace("\\", "/"))
        if scene_entry.entry_point or entry_scene_json is None:
            entry_scene_json = str(out)

    # Canonical top-level scene.json used by the runtime.
    if entry_scene_json is not None:
        top = ctx.project_dir / "scene.json"
        Path(top).write_text(
            Path(entry_scene_json).read_text(encoding="utf-8"), encoding="utf-8"
        )
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
