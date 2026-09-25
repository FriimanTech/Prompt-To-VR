"""Bind the player's mechanics explicitly in the compiled scene.

The engine no longer injects any player mechanic, so what scene.json binds is
exactly what runs. This pass makes the binding match the BuildSpec:

* every player role the game needs gets one binding on its host entity
  (``role:body`` on the ``player_avatar`` entity, everything else on
  ``player_rig``), using the mechanic plan's item for that role so its config
  and controller binding carry through;
* the same mechanic bound twice collapses to one;
* two DIFFERENT mechanics filling one role, or a locomotion / turn mechanic the
  BuildSpec does not allow, fails the compile -- that is the two-aim-arcs bug
  and a teleporter in a stationary game, and neither should ship;
* Hands Only drops the player-avatar entity and never binds the body.

A game with no locomotion at all is valid: a stationary archery game plans no
movement modes, binds none, and loads without complaint.
"""

from __future__ import annotations

from typing import Any, Callable

from ptv_schemas import BuildSpec, MechanicPlan, PlayerExperience

from ..engine_catalog import PLAYER_ROLES, role_of, role_of_plan_item
from .movement import (
    MOTION_ROLES,
    binding_id_for_item,
    default_scene_mode,
    default_scene_turn,
    effective_movement,
    required_roles,
    role_config,
)

PLAYER_RIG_ID = "player_rig"

#: Tags of player-owned entities that never render on their own.
_PLAYER_TAGS = ("player", "player_rig", "player_hand", "player_locomotion")


class PlayerRoleError(RuntimeError):
    """The scene's player mechanics contradict themselves or the BuildSpec."""


def _binding_role(binding: dict[str, Any], plan_by_id: dict[str, Any]) -> str:
    bid = str(binding.get("builtin_id") or binding.get("id") or "")
    role = role_of(bid)
    if role:
        return role
    key = str(binding.get("id") or "").split(".", 1)[-1]
    item = plan_by_id.get(key)
    return role_of_plan_item(item) if item is not None else ""


def _ensure_player_rig(entities: list[dict[str, Any]]) -> dict[str, Any]:
    for e in entities:
        if e.get("id") == PLAYER_RIG_ID or "player_rig" in (e.get("tags") or []):
            return e
    rig = {
        "id": PLAYER_RIG_ID,
        "name": "Player Rig",
        "transform": {
            "position": [0.0, 0.0, 0.0],
            "rotation": [0.0, 0.0, 0.0, 1.0],
            "scale": 1.0,
        },
        "mesh": None,
        "physics": None,
        "mechanic_bindings": [],
        "tags": ["player_rig", "player_system"],
    }
    entities.append(rig)
    return rig


def _binding_for_item(item: Any) -> dict[str, Any]:
    bid, builtin_id = binding_id_for_item(item)
    params = dict(item.config)
    if item.binding is not None and item.binding.control.value != "none":
        params.setdefault("control", item.binding.control.value)
        params.setdefault("hand", item.binding.hand.value)
    out: dict[str, Any] = {"id": bid, "params": params}
    if builtin_id:
        out["builtin_id"] = builtin_id
    return out


def bind_player_roles(
    entities: list[dict[str, Any]],
    plan: MechanicPlan,
    build_spec: BuildSpec,
    on_warning: Callable[[str], None] | None = None,
) -> list[dict[str, Any]]:
    """Make the scene bind exactly the player roles the BuildSpec implies.

    Returns the (possibly shortened) entity list; mutates entities in place.
    Raises :class:`PlayerRoleError` on a contradiction.
    """
    warn = on_warning or (lambda _m: None)
    hands_only = build_spec.player_experience == PlayerExperience.hands_only
    required = required_roles(build_spec)
    allowed = set(required)

    if hands_only:
        kept = []
        for e in entities:
            if "player_avatar" in (e.get("tags") or []):
                warn(
                    f"entity '{e.get('id')}' is a player avatar but this is a"
                    " Hands Only game; dropped it"
                )
                continue
            kept.append(e)
        entities[:] = kept

    plan_by_id = _plan_index(plan)
    plan_by_role: dict[str, Any] = {}
    for it in plan.items:
        role = role_of_plan_item(it)
        if role and role not in plan_by_role:
            plan_by_role[role] = it

    # role -> [(entity, binding)]
    found: dict[str, list[tuple[dict[str, Any], dict[str, Any]]]] = {}
    for e in entities:
        for b in list(e.get("mechanic_bindings") or []):
            if not isinstance(b, dict):
                continue
            role = _binding_role(b, plan_by_id)
            if role and role in PLAYER_ROLES:
                found.setdefault(role, []).append((e, b))

    errors: list[str] = []
    movement = effective_movement(build_spec)
    for role, uses in found.items():
        if role not in allowed:
            if role in MOTION_ROLES:
                errors.append(
                    f"{role} is bound ({uses[0][1].get('id')} on"
                    f" '{uses[0][0].get('id')}') but the BuildSpec movement"
                    f" block allows modes={[m.value for m in movement.modes]}"
                    f" turning={[t.value for t in movement.turning]}"
                )
                continue
            for e, b in uses:
                e["mechanic_bindings"].remove(b)
                warn(f"removed {b.get('id')} from '{e.get('id')}': this game"
                     f" has no {role}")
            continue
        ids = {str(b.get("id")) for _e, b in uses}
        if len(ids) > 1:
            errors.append(
                f"{role} is filled by {len(ids)} different mechanics"
                f" ({sorted(ids)}); a role has exactly one owner"
            )
            continue
        for e, b in uses[1:]:
            e["mechanic_bindings"].remove(b)
            warn(f"removed duplicate {b.get('id')} from '{e.get('id')}'"
                 f" ({role} is already bound on '{uses[0][0].get('id')}')")

    if errors:
        raise PlayerRoleError(
            "the scene's player mechanics contradict the BuildSpec:\n"
            + "\n".join(f"  - {m}" for m in errors)
            + "\n\nEach player role (teleport, slide, snap/smooth turn,"
            " fingers, body, settings menu) has exactly one mechanic, and only"
            " the modes the BuildSpec `movement` block lists are allowed."
            " Fix the SceneSpec bindings or the BuildSpec movement block."
        )

    rig: dict[str, Any] | None = None
    avatars = [e for e in entities if "player_avatar" in (e.get("tags") or [])]
    for role in required:
        if role in found:
            continue
        item = plan_by_role.get(role)
        if item is not None:
            binding = _binding_for_item(item)
        else:
            builtin = PLAYER_ROLES[role]["builtin"]
            binding = {"id": builtin, "builtin_id": builtin,
                       "params": role_config(build_spec, role)}
        if role == "role:body":
            if not avatars:
                warn("Full Player Avatar game but the scene has no"
                     " player_avatar entity; the player gets default hands")
                continue
            host = avatars[0]
            params = binding.setdefault("params", {})
            for k, v in role_config(build_spec, role).items():
                params.setdefault(k, v)
            if host.pop("attached_to", None) is not None:
                warn(
                    f"entity '{host.get('id')}' is the player avatar and was"
                    " attached to another entity; dropped the attachment so"
                    " builtin.avatar_body can plant it on the XR rig"
                )
        else:
            if rig is None:
                rig = _ensure_player_rig(entities)
            host = rig
        host.setdefault("mechanic_bindings", []).append(binding)

    for e in entities:
        tags = e.setdefault("tags", [])
        if e.get("mesh"):
            continue
        if e.get("id") == PLAYER_RIG_ID or any(t in tags for t in _PLAYER_TAGS):
            if "player_system" not in tags:
                tags.append("player_system")
    return entities


def _plan_index(plan: MechanicPlan) -> dict[str, Any]:
    out: dict[str, Any] = {}
    for it in plan.items:
        out[it.id] = it
        if it.code_task is not None:
            out[it.code_task.id] = it
    return out


def body_is_bound(entities: list[dict[str, Any]], plan: MechanicPlan) -> bool:
    plan_by_id = _plan_index(plan)
    for e in entities:
        if "player_avatar" not in (e.get("tags") or []):
            continue
        for b in e.get("mechanic_bindings") or []:
            if isinstance(b, dict) and _binding_role(b, plan_by_id) == "role:body":
                return True
    return False


def player_block_fields(
    build_spec: BuildSpec, entities: list[dict[str, Any]], plan: MechanicPlan
) -> dict[str, Any]:
    """The explicit player settings the engine reads from scene.json."""
    avatar = (
        build_spec.player_experience == PlayerExperience.full_avatar
        and body_is_bound(entities, plan)
    )
    out: dict[str, Any] = {
        "locomotion_menu": False,
        "default_turn_mode": default_scene_turn(build_spec),
        "hands": {"source": "avatar" if avatar else "default"},
    }
    mode = default_scene_mode(build_spec)
    if mode:
        out["default_locomotion_mode"] = mode
    return out
