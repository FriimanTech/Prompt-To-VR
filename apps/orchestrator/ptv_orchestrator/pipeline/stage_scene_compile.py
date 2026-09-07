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
from collections.abc import Iterable
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


def _check_required_sockets(
    entity: Any,
    plan_item: Any,
    mesh: dict[str, Any] | None,
    contracts: dict[str, dict[str, Any]],
) -> list[str]:
    """Violations for one entity/mechanic pair. Empty list when satisfied.

    Mechanics with no ``code_task``, or a code task that declares no
    ``required_sockets``, are skipped entirely -- so this is inert for every
    project generated before sockets existed.
    """
    task = getattr(plan_item, "code_task", None)
    required = list(getattr(task, "required_sockets", None) or [])
    if not required:
        return []

    mechanic_id = getattr(task, "id", None) or getattr(plan_item, "id", "?")
    if mesh is None:
        return [
            f"entity '{entity.id}' binds '{mechanic_id}' which requires"
            f" sockets {required}, but the entity has no mesh to carry them"
        ]

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
    return out


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
        **_mesh_render_fields(norm or {}, asset.target_scale_meters, contract),
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
    contract: dict[str, Any] | None = None,
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
    unit_radius = norm.get("bounds_radius_m")
    if unit_radius:
        # The optimizer reports the radius of a mesh normalised to a 1 m
        # longest axis; the runtime rescales by target_scale_meters, so the
        # world-space radius scales identically.
        out["bounds_radius_m"] = float(unit_radius) * float(target_scale_meters)
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
        if speeds:
            out["animation_speeds"] = speeds
    sockets = _sockets_in_meters(contract, target_scale_meters)
    if sockets:
        out["sockets"] = sockets
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
        out[name] = {
            "position": [float(pos[0]) * scale,
                         float(pos[1]) * scale,
                         float(pos[2]) * scale],
            # Rotation is scale-invariant; pass it through as [x, y, z, w],
            # which is the convention SceneLoader::read_quat expects.
            "rotation": [float(rot[0]), float(rot[1]), float(rot[2]),
                         float(rot[3])],
        }
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
    asset_contract: dict[str, dict[str, Any]] | None = None,
) -> dict[str, Any]:
    """Pure function: produce the engine-loadable scene JSON dict.

    Raises ``SocketContractError`` when a bound mechanic declares a
    ``required_sockets`` entry the asset's contract cannot supply -- see
    :func:`_validate_required_sockets`.
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
                    norm, scale_m, contracts.get(e.mesh.asset_id)
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
                socket_violations.extend(
                    _check_required_sockets(e, item, mesh, contracts)
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
        auto_index += 1

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
            **_mesh_render_fields(
                norm, scale_m, (contracts or {}).get(mesh["asset_id"])
            ),
        }
        out.append(merged)
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
) -> dict:
    """Merge the C++-generated environment (world.scene.json) into a compiled
    gameplay scene doc: environment entities + sky + lights + spawn come from
    the world; gameplay entities render on top of it."""
    world_entities = _repoint_world_meshes_to_lods(
        list(world_doc.get("entities") or []), nrep or {}, contracts
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
    world_nav = world_doc.get("navigation")
    if isinstance(world_nav, dict) and world_nav.get("regions"):
        doc["navigation"] = world_nav
    world_player = world_doc.get("player") or {}
    world_spawn = (
        world_player.get("spawn") if isinstance(world_player, dict) else None
    )
    if isinstance(world_spawn, dict) and "position" in world_spawn:
        doc.setdefault("player", {})
        doc["player"]["spawn"] = world_spawn

    # Declared state, exits and named arrival points travel with the
    # environment that declared them.
    for key in ("interactives", "spawn_points", "portals"):
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
            build_spec, manifest, plan, scene_spec, normalization, asset_contract
        )
        # Every scene gets its own environment now, not just the entry point.
        # Previously the other scenes compiled to gameplay entities floating
        # in an empty void, which is what a six-scene project actually
        # produced.
        world_doc = _load_world(scene_entry.id)
        if world_doc is not None:
            doc = _merge_world_into_scene(
                doc, world_doc, normalization, asset_contract
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
        compiled.append((scene_entry.id, doc))
        if scene_entry.entry_point or entry_id_written is None:
            entry_id_written = scene_entry.id

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
    for scene_id, doc in compiled:
        doc["scene_index"] = index

        # Written beside scene.json, because that is what the index paths are
        # relative to and what the APK packages.
        runtime_path = ctx.project_dir / f"scene.{scene_id}.json"
        payload = json.dumps(doc, indent=2, sort_keys=True)
        runtime_path.write_text(payload, encoding="utf-8")
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
        top.write_text(entry_scene_json, encoding="utf-8")
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
