"""Map pressables, displays, challenges, destructibles and explosives onto
their engine built-ins.

The same story as weapons (``weapon_plan``): every one of these was being
written per game by a code-generation agent, and each time it disagreed with
the scene it ran in. The shooting range's red button waited for a hand
collider that did not exist, its timer HUD was folded into the wrist HUD and
deleted, its targets were never destroyable and its grenades never exploded.

So, deterministically and with every change named, for a project with an
InteractionDesign:

* a ``pressable`` item gets ``builtin.press_button``;
* a ``display`` item gets ``builtin.world_display`` (a generated "HUD" item
  for it is replaced -- the wrist HUD is ``builtin.game_hud``, a screen in
  the world is this);
* an item with a ``destructible`` spec gets ``builtin.destructible``;
* a thrown item with an ``explosive`` spec gets ``builtin.explosive``
  (handled with the weapons in ``weapon_plan``);
* a generated timed / score challenge that spawns targets becomes
  ``builtin+config`` of ``builtin.target_challenge``.
"""

from __future__ import annotations

import json
import re
from typing import Any, Optional

PRESS_BUTTON_ID = "builtin.press_button"
WORLD_DISPLAY_ID = "builtin.world_display"
TARGET_CHALLENGE_ID = "builtin.target_challenge"
DESTRUCTIBLE_ID = "builtin.destructible"
EXPLOSIVE_ID = "builtin.explosive"
INTERACTION_BUILTIN_IDS = {PRESS_BUTTON_ID, WORLD_DISPLAY_ID, TARGET_CHALLENGE_ID,
                           DESTRUCTIBLE_ID, EXPLOSIVE_ID}

INTERACTION_BUILTINS_RULE = (
    "INTERACTIONS: these are engine built-ins -- plan them as builtin+config, never "
    "generate them: a button / switch the player presses is builtin.press_button "
    "(index-finger poke or trigger near it; emits a named signal); a scoreboard, timer "
    "or status screen IN THE WORLD is builtin.world_display (text bound to ctx.vars; "
    "builtin.game_hud is only the wrist HUD); a timed or score challenge that spawns "
    "targets at world-declared points and counts hits is builtin.target_challenge "
    "(start signal, duration, spawn anchor set, scoring, publishes "
    "<id>.time_left / score / state to ctx.vars); a target, crate or object that is "
    "shot or blown up and breaks is builtin.destructible; a grenade / flashbang / "
    "explosive the player arms and throws is builtin.explosive (fuse, radial damage, "
    "flash). Wire them with signals (button signal -> challenge start_signal) and "
    "vars (challenge vars -> display text). Generate code only for game logic those "
    "cannot express, and have it READ ctx.signals / ctx.vars / ctx.combat."
)

_CHALLENGE = re.compile(r"challenge|time[\s_-]*trial|timed|score[\s_-]*attack|countdown",
                        re.IGNORECASE)
_TARGETS = re.compile(r"target", re.IGNORECASE)
_SECONDS = re.compile(r"(\d{1,3})\s*[- ]?\s*(?:s\b|sec|second)", re.IGNORECASE)


def _usage(item: Any) -> str:
    u = getattr(item, "usage", None)
    return getattr(u, "value", str(u or ""))


def _text(it: Any) -> str:
    task = getattr(it, "code_task", None)
    title = getattr(task, "title", "") if task is not None else ""
    desc = getattr(task, "description", "") if task is not None else ""
    return f"{it.id} {title} {desc[:1200]}"


def _names_asset(it: Any, asset_id: str) -> bool:
    return bool(re.search(rf"\b{re.escape(asset_id)}\b", _text(it)))


def challenge_config(text: str) -> dict[str, Any]:
    m = _SECONDS.search(text or "")
    return {"duration_s": float(m.group(1)) if m else 30.0}


def explosive_params(spec: Any) -> dict[str, Any]:
    """builtin.explosive params from the design's ExplosiveSpec."""
    return {"kind": spec.kind.value, "activation": spec.activation.value,
            "fuse_s": spec.fuse_s, "radius_m": spec.radius_m,
            "inner_radius_m": spec.inner_radius_m, "damage": spec.damage,
            "impulse": spec.impulse, "flash_radius_m": spec.flash_radius_m,
            "stun_s": spec.stun_s, "hurts_player": spec.hurts_player}


def reconcile_interaction_items(mech_plan: Any, design: Optional[Any]) -> tuple[Any, list[str]]:
    """Return ``(plan, notes)``; a no-op without an InteractionDesign."""
    from ptv_schemas import MechanicClassification, MechanicPlanItem

    if design is None:
        return mech_plan, []
    notes: list[str] = []
    pressables = [i for i in design.items if _usage(i) == "pressable"]
    displays = [i for i in design.items if _usage(i) == "display"]
    destructibles = [i for i in design.items if getattr(i, "destructible", None) is not None]

    items = []
    for it in mech_plan.items:
        if it.builtin_id in (PRESS_BUTTON_ID, WORLD_DISPLAY_ID, DESTRUCTIBLE_ID):
            continue  # re-added below from the design
        if it.builtin_id == "builtin.game_hud" and any(_names_asset(it, d.asset_id)
                                                       for d in displays):
            # A screen in the world is not the wrist HUD.
            d = next(d for d in displays if _names_asset(it, d.asset_id))
            items.append(it.model_copy(update={
                "builtin_id": WORLD_DISPLAY_ID,
                "classification": MechanicClassification.builtin_config,
                "config": {**(it.config or {}), "display_asset": d.asset_id}}))
            notes.append(f"'{it.id}' is the in-world display {d.asset_id}: "
                         f"{WORLD_DISPLAY_ID}, not the wrist HUD")
            continue
        task = it.code_task
        if task is not None and it.classification.value == "generate":
            text = _text(it)
            if _CHALLENGE.search(text) and _TARGETS.search(text):
                items.append(it.model_copy(update={
                    "classification": MechanicClassification.builtin_config,
                    "builtin_id": TARGET_CHALLENGE_ID, "base_builtin_id": None,
                    "code_task": None,
                    "config": {**challenge_config(text), **(it.config or {})}}))
                notes.append(f"'{it.id}' is a timed target challenge: {TARGET_CHALLENGE_ID}")
                continue
            hit = next((d for d in displays if _names_asset(it, d.asset_id)), None)
            if hit is not None and re.search(r"hud|display|screen|scoreboard|panel|timer",
                                             text, re.IGNORECASE):
                items.append(it.model_copy(update={
                    "classification": MechanicClassification.builtin_config,
                    "builtin_id": WORLD_DISPLAY_ID, "base_builtin_id": None,
                    "code_task": None, "config": {"display_asset": hit.asset_id}}))
                notes.append(f"'{it.id}' shows text on {hit.asset_id}: {WORLD_DISPLAY_ID}")
                continue
        items.append(it)

    have = {i.id for i in items}
    for p in pressables:
        pid = f"press_button_{p.asset_id}"[:64]
        if pid in have:
            continue
        press = getattr(p, "press", None)
        cfg: dict[str, Any] = {"signal": getattr(press, "signal", "") or f"{p.asset_id}.pressed"}
        if press is not None:
            cfg.update({"press_with": press.press_with.value, "travel_m": press.travel_m})
        items.append(MechanicPlanItem(id=pid, classification=MechanicClassification.builtin_config,
                                      builtin_id=PRESS_BUTTON_ID, config=cfg))
        notes.append(f"'{pid}': {PRESS_BUTTON_ID} for {p.asset_id}")
    for d in displays:
        if any(i.builtin_id == WORLD_DISPLAY_ID and (i.config or {}).get("display_asset")
               == d.asset_id for i in items):
            continue
        pid = f"world_display_{d.asset_id}"[:64]
        items.append(MechanicPlanItem(id=pid, classification=MechanicClassification.builtin_config,
                                      builtin_id=WORLD_DISPLAY_ID,
                                      config={"display_asset": d.asset_id}))
        notes.append(f"'{pid}': {WORLD_DISPLAY_ID} for {d.asset_id}")
    for t in destructibles:
        pid = f"destructible_{t.asset_id}"[:64]
        spec = t.destructible
        items.append(MechanicPlanItem(
            id=pid, classification=MechanicClassification.builtin_config,
            builtin_id=DESTRUCTIBLE_ID,
            config={"max_health": spec.max_health, "respawn_after_s": spec.respawn_after_s,
                    "effect": spec.effect.value}))
        notes.append(f"'{pid}': {DESTRUCTIBLE_ID} for {t.asset_id}")

    if sorted(json.dumps(i.model_dump(mode="json"), sort_keys=True) for i in items) == \
            sorted(json.dumps(i.model_dump(mode="json"), sort_keys=True) for i in mech_plan.items):
        return mech_plan, []
    return mech_plan.model_copy(update={"items": items}), notes
