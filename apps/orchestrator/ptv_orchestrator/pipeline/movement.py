"""Player roles: which player mechanics a game gets, decided from the BuildSpec.

Nothing about the player is injected by the engine any more. Every player
mechanic -- finger posing, the avatar body, each locomotion mode, each turn
style and the settings menu -- is planned explicitly in ``mechanic_plan.json``
and bound explicitly in ``scene.json``. This module is where "what does this
game need" is answered, once, from the BuildSpec:

* ``role:fingers``   -- every game (both experiences have controllable hands);
* ``role:body``      -- Full Player Avatar only;
* one role per ``movement.modes`` entry (may be none: a stationary game);
* one role per ``movement.turning`` entry (may be none);
* ``role:movement_settings_menu`` -- when the game offers a choice between
  modes or turn styles (or a vignette switch the prompt asked for).

``reconcile_mechanic_plan`` then makes the plan say exactly that: it adds the
stock built-in for any required role nothing fills, collapses two items
filling one role (a generated replacement beats its base built-in), and
removes locomotion/turning the BuildSpec does not allow -- an LLM-planned
teleport in a stationary game.
"""

from __future__ import annotations

import re
from dataclasses import dataclass

from ptv_schemas import (
    BuildSpec,
    MechanicClassification,
    MechanicPlan,
    MechanicPlanItem,
    MovementMode,
    MovementSpec,
    PlayerExperience,
    TurnStyle,
)
from ptv_schemas.build_spec import LocomotionStyle

from ..engine_catalog import (
    MOVEMENT_MODE_ROLES,
    PLAYER_ROLES,
    TURN_ROLES,
    entry,
    role_of_plan_item,
)

#: Roles that move or turn the player -- the ones the movement block governs.
MOTION_ROLES = frozenset(set(MOVEMENT_MODE_ROLES.values()) | set(TURN_ROLES.values()))

#: Plan-item id to use when the reconcile has to add a role's built-in.
_ROLE_ITEM_IDS = {
    "role:fingers": "hand_pose",
    "role:body": "avatar_body",
    "role:teleport": "teleport",
    "role:slide_head": "slide_head_relative",
    "role:slide_controller": "slide_controller_relative",
    "role:snap_turn": "snap_turn",
    "role:smooth_turn": "smooth_turn",
    "role:movement_settings_menu": "movement_settings_menu",
}

#: What scene.json's ``player.default_locomotion_mode`` calls each mode.
SCENE_MODE_NAMES = {
    MovementMode.teleport: "teleport",
    MovementMode.slide_head: "slide_head_relative",
    MovementMode.slide_controller: "slide_controller_relative",
}


def effective_movement(spec: BuildSpec) -> MovementSpec:
    """The movement block, derived from the legacy ``locomotion`` field for a
    BuildSpec written before ``movement`` existed."""
    if "movement" in spec.model_fields_set:
        return spec.movement
    legacy = spec.locomotion
    if legacy == LocomotionStyle.stationary:
        modes: list[MovementMode] = []
    elif legacy == LocomotionStyle.teleport:
        modes = [MovementMode.teleport]
    elif legacy == LocomotionStyle.smooth:
        modes = [MovementMode.slide_head, MovementMode.slide_controller]
    else:  # mixed
        modes = [
            MovementMode.teleport,
            MovementMode.slide_head,
            MovementMode.slide_controller,
        ]
    return MovementSpec(
        modes=modes,
        turning=[TurnStyle.snap],
        settings_menu=len(modes) >= 2,
    )


def offers_choice(movement: MovementSpec) -> bool:
    """Whether the settings menu would have anything on it.

    The menu shows a locomotion section only for 2+ modes, a turning section
    only for both turn styles, and a vignette switch only when the prompt
    asked for one on a motion that actually exists. A game with nothing to
    choose never gets the menu, whatever ``settings_menu`` says.
    """
    slides = any(m in (MovementMode.slide_head, MovementMode.slide_controller)
                 for m in movement.modes)
    smooth = TurnStyle.smooth in movement.turning
    v = movement.vignette
    vignette_switch = v.user_toggle and (
        (slides and v.slide) or (smooth and v.smooth_turn)
    )
    return (
        len(set(movement.modes)) >= 2
        or len(set(movement.turning)) >= 2
        or vignette_switch
    )


def required_roles(spec: BuildSpec) -> list[str]:
    """Every player role this game needs, in a stable order."""
    movement = effective_movement(spec)
    roles = ["role:fingers"]
    if spec.player_experience == PlayerExperience.full_avatar:
        roles.append("role:body")
    for mode in movement.modes:
        role = MOVEMENT_MODE_ROLES[mode.value]
        if role not in roles:
            roles.append(role)
    for turn in movement.turning:
        role = TURN_ROLES[turn.value]
        if role not in roles:
            roles.append(role)
    if offers_choice(movement):
        roles.append("role:movement_settings_menu")
    return roles


def role_config(spec: BuildSpec, role: str) -> dict[str, object]:
    """Built-in params the BuildSpec implies for a role (vignette, etc.)."""
    movement = effective_movement(spec)
    v = movement.vignette
    if role in ("role:slide_head", "role:slide_controller"):
        if v.slide or v.user_toggle:
            return {"vignette": {"enabled": bool(v.slide),
                                 "user_toggle": bool(v.user_toggle)}}
        return {}
    if role == "role:smooth_turn":
        if v.smooth_turn or v.user_toggle:
            return {"vignette": {"enabled": bool(v.smooth_turn),
                                 "user_toggle": bool(v.user_toggle)}}
        return {}
    if role == "role:body":
        a = spec.player_avatar
        return {
            "coverage": a.coverage.value,
            "head_visibility": a.head_visibility.value,
            "leg_animation": bool(a.leg_animation),
        }
    return {}


@dataclass
class ReconcileResult:
    plan: MechanicPlan
    added: list[str]
    removed: list[str]
    notes: list[str]


def _is_replacement(item: MechanicPlanItem) -> bool:
    return item.classification == MechanicClassification.builtin_config_code_edit


#: Config keys naming a screen blink / fade / black-out. Moving and turning
#: never darken the view that way: teleport and snap turn are already the
#: comfort options, and sliding / smooth turning use the engine vignette.
_BLINK_KEY = re.compile(r"blink|fade|black_?out|flash", re.IGNORECASE)
#: Keys every binding may carry regardless of the built-in's own params.
_ALWAYS_OK_KEYS = frozenset({"control", "hand", "vignette"})

_NO_BLINK_NOTE = (
    "\n\nENGINE RULE (overrides anything above): this mechanic moves or turns"
    " the player, so it must NEVER blink, fade, flash or black out the view,"
    " and must never draw a marker that does. Comfort for sliding and smooth"
    " turning goes through ctx.comfort (the engine vignette); teleport and"
    " snap turn get none."
)


def _strip_blinks(
    item: MechanicPlanItem, role: str, notes: list[str]
) -> MechanicPlanItem:
    """Drop blink/fade from a motion mechanic, and drop the copy with it.

    A ``builtin+config_code_edit`` copy of a motion built-in whose only
    departures from the built-in were a blink (plus params the built-in
    already takes) exists to add exactly what the engine forbids, so it
    becomes the stock built-in. A copy that still asks for something the
    built-in cannot do keeps its code, with the rule written into its task.
    """
    if role not in MOTION_ROLES:
        return item
    cfg = dict(item.config)
    blinks = [k for k in cfg if _BLINK_KEY.search(k)]
    for k in blinks:
        cfg.pop(k)
    if not _is_replacement(item):
        if blinks:
            notes.append(f"'{item.id}': dropped {blinks}; {role} never blinks")
            return item.model_copy(update={"config": cfg})
        return item

    base = item.base_builtin_id or PLAYER_ROLES[role]["builtin"]
    documented = set(getattr(entry(base), "params", {}) or {}) | _ALWAYS_OK_KEYS
    extra = [k for k in cfg if k not in documented]
    if blinks and not extra:
        notes.append(
            f"'{item.id}': the copy of {base} only added {blinks}; {role} never"
            " blinks, so it is the stock built-in"
        )
        return item.model_copy(update={
            "classification": (
                MechanicClassification.builtin_config
                if cfg else MechanicClassification.builtin
            ),
            "builtin_id": base,
            "base_builtin_id": None,
            "config": cfg,
            "code_task": None,
        })
    task = item.code_task
    if task is not None and _NO_BLINK_NOTE not in task.description:
        task = task.model_copy(
            update={"description": task.description + _NO_BLINK_NOTE}
        )
    if blinks:
        notes.append(f"'{item.id}': dropped {blinks}; {role} never blinks")
    return item.model_copy(update={"config": cfg, "code_task": task})


def _prune_dependencies(
    items: list[MechanicPlanItem], notes: list[str]
) -> list[MechanicPlanItem]:
    """A code task waiting on a task the reconcile removed would never run."""
    task_ids = {it.code_task.id for it in items if it.code_task is not None}
    out: list[MechanicPlanItem] = []
    for it in items:
        task = it.code_task
        if task is not None and task.depends_on:
            kept = [d for d in task.depends_on if d in task_ids]
            if len(kept) != len(task.depends_on):
                gone = sorted(set(task.depends_on) - set(kept))
                notes.append(f"'{it.id}': no longer waits on {gone}")
                it = it.model_copy(update={
                    "code_task": task.model_copy(update={"depends_on": kept})
                })
        out.append(it)
    return out


def reconcile_mechanic_plan(plan: MechanicPlan, spec: BuildSpec) -> ReconcileResult:
    """Make the plan fill exactly the player roles the BuildSpec implies."""
    required = required_roles(spec)
    allowed = set(required)
    notes: list[str] = []
    removed: list[str] = []
    added: list[str] = []

    kept: list[MechanicPlanItem] = []
    owner: dict[str, int] = {}  # role -> index into kept
    for item in plan.items:
        role = role_of_plan_item(item)
        if not role or role not in PLAYER_ROLES:
            kept.append(item)
            continue
        item = _strip_blinks(item, role, notes)
        if role not in allowed:
            removed.append(item.id)
            notes.append(
                f"removed '{item.id}': it fills {role}, which this game does "
                "not have (see BuildSpec movement / player_experience)"
            )
            continue
        if role in owner:
            incumbent = kept[owner[role]]
            # A generated replacement beats its base built-in; otherwise the
            # first one planned wins.
            if _is_replacement(item) and not _is_replacement(incumbent):
                removed.append(incumbent.id)
                kept[owner[role]] = item
                notes.append(
                    f"'{item.id}' replaces '{incumbent.id}' for {role}"
                )
            else:
                removed.append(item.id)
                notes.append(
                    f"removed '{item.id}': {role} is already filled by "
                    f"'{incumbent.id}'"
                )
            continue
        # The BuildSpec owns the comfort settings of stock built-ins.
        if item.classification in (
            MechanicClassification.builtin,
            MechanicClassification.builtin_config,
        ):
            cfg = dict(item.config)
            implied = role_config(spec, role)
            if "vignette" in implied:
                prev = cfg.get("vignette")
                merged = dict(prev) if isinstance(prev, dict) else {}
                merged.update(implied["vignette"])  # type: ignore[arg-type]
                cfg["vignette"] = merged
            elif role in ("role:slide_head", "role:slide_controller",
                          "role:smooth_turn"):
                cfg.pop("vignette", None)
            if role == "role:body":
                for k, val in implied.items():
                    cfg.setdefault(k, val)
            if cfg != dict(item.config):
                item = item.model_copy(update={
                    "config": cfg,
                    "classification": (
                        MechanicClassification.builtin_config
                        if cfg else item.classification
                    ),
                })
        owner[role] = len(kept)
        kept.append(item)

    used_ids = {it.id for it in kept}
    for role in required:
        if role in owner:
            continue
        builtin = PLAYER_ROLES[role]["builtin"]
        base_id = _ROLE_ITEM_IDS[role]
        item_id = base_id
        n = 2
        while item_id in used_ids:
            item_id = f"{base_id}_{n}"
            n += 1
        cfg = role_config(spec, role)
        kept.append(
            MechanicPlanItem(
                id=item_id,
                classification=(
                    MechanicClassification.builtin_config
                    if cfg else MechanicClassification.builtin
                ),
                builtin_id=builtin,
                config=cfg,
            )
        )
        used_ids.add(item_id)
        owner[role] = len(kept) - 1
        added.append(item_id)
        notes.append(f"added '{item_id}' ({builtin}) for {role}")

    kept = _prune_dependencies(kept, notes)
    return ReconcileResult(
        plan=plan.model_copy(update={"items": kept}),
        added=added,
        removed=removed,
        notes=notes,
    )


def binding_id_for_item(item: MechanicPlanItem) -> tuple[str, str | None]:
    """(binding id, builtin_id) for a plan item, as scene.json writes it."""
    if item.classification in (
        MechanicClassification.builtin,
        MechanicClassification.builtin_config,
    ):
        bid = item.builtin_id or f"builtin.{item.id}"
        return bid, bid
    task_id = item.code_task.id if item.code_task is not None else item.id
    return f"generated.{task_id}", None


def default_scene_mode(spec: BuildSpec) -> str:
    movement = effective_movement(spec)
    mode = movement.default_mode or (movement.modes[0] if movement.modes else None)
    return SCENE_MODE_NAMES.get(mode, "") if mode is not None else ""


def default_scene_turn(spec: BuildSpec) -> str:
    movement = effective_movement(spec)
    turn = movement.default_turn or (
        movement.turning[0] if movement.turning else None
    )
    return turn.value if turn is not None else "snap"
