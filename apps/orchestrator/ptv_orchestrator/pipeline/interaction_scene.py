"""Bind buttons, displays, challenges, destructibles and explosives in a
compiled scene, from the InteractionDesign and the MechanicPlan.

Deterministic, like the weapon binding (``weapon_scene``): whatever the
scene composer remembered to bind, every pressable gets
``builtin.press_button``, every display ``builtin.world_display``, every
destructible ``builtin.destructible`` (with its health), every thrown
explosive ``builtin.explosive`` + grab, and every planned
``builtin.target_challenge`` a host, its target templates, its anchor set
and -- when the game has exactly one button -- that button's signal. A
display with no lines of its own shows the challenge's time, hits and best.
"""

from __future__ import annotations

from typing import Any, Callable, Optional

from .interaction_plan import (
    DESTRUCTIBLE_ID,
    EXPLOSIVE_ID,
    PRESS_BUTTON_ID,
    TARGET_CHALLENGE_ID,
    WORLD_DISPLAY_ID,
    explosive_params,
)

GRAB_ID = "builtin.grab"
THROW_ID = "builtin.throw"
WarnFn = Callable[[str], None]


def _bid(b: dict) -> str:
    return str(b.get("builtin_id") or b.get("id") or "")


def _asset(e: dict) -> str:
    return str((e.get("mesh") or {}).get("asset_id") or "")


def _tags(e: dict) -> list[str]:
    return e.setdefault("tags", [])


def _add_tags(e: dict, *tags: str) -> None:
    have = _tags(e)
    for t in tags:
        if t not in have:
            have.append(t)


def _ensure(e: dict, builtin_id: str, params: dict) -> dict:
    """Bind ``builtin_id`` (once), the design's params under any the scene
    already set."""
    for b in e.setdefault("mechanic_bindings", []):
        if isinstance(b, dict) and _bid(b) == builtin_id:
            merged = dict(params)
            merged.update(b.get("params") or {})
            b["params"] = merged
            b["builtin_id"] = builtin_id
            return b
    b = {"id": builtin_id, "builtin_id": builtin_id, "params": dict(params)}
    e["mechanic_bindings"].append(b)
    return b


def _plan_config(plan: Any, builtin_id: str, asset_id: str = "") -> dict:
    for it in getattr(plan, "items", []) or []:
        if it.builtin_id != builtin_id:
            continue
        cfg = dict(it.config or {})
        if not asset_id or cfg.get("display_asset") in (None, "", asset_id) or \
                it.id.endswith(asset_id):
            return cfg
    return {}


def apply_interactions(doc: dict, design: Any, plan: Any,
                       on_warning: Optional[WarnFn] = None) -> dict:
    """Bind every interaction built-in; returns a summary."""
    summary: dict[str, list] = {"press_buttons": [], "displays": [], "destructibles": [],
                                "explosives": [], "challenges": []}
    if design is None:
        return summary
    items = {i.asset_id: i for i in design.items}
    entities = [e for e in doc.get("entities") or [] if isinstance(e, dict)]
    signals: list[str] = []
    displays: list[dict] = []

    for e in entities:
        item = items.get(_asset(e))
        if item is None:
            continue
        usage = getattr(item.usage, "value", str(item.usage))
        template = "template" in _tags(e)
        if usage == "pressable" and not template:
            press = item.press
            signal = (press.signal if press is not None and press.signal
                      else f"{item.asset_id}.pressed")
            params = {"signal": signal}
            if press is not None:
                params.update({"press_with": press.press_with.value,
                               "travel_m": press.travel_m})
            params.update(_plan_config(plan, PRESS_BUTTON_ID, item.asset_id))
            b = _ensure(e, PRESS_BUTTON_ID, params)
            signals.append(str(b["params"].get("signal") or signal))
            _add_tags(e, "interactable", "button")
            e.setdefault("physics", {"kind": "static", "collision_shape": "aabb",
                                     "mass_kg": 1.0})
            summary["press_buttons"].append(f"{e.get('id')} -> {signals[-1]}")
        elif usage == "display" and not template:
            _ensure(e, WORLD_DISPLAY_ID, _plan_config(plan, WORLD_DISPLAY_ID, item.asset_id))
            displays.append(e)
            e["static_batch"] = False
            summary["displays"].append(str(e.get("id")))
        if item.destructible is not None:
            spec = item.destructible
            _ensure(e, DESTRUCTIBLE_ID, {"effect": spec.effect.value,
                                         "respawn_after_s": spec.respawn_after_s,
                                         "explosion_damage": spec.explosion_damage})
            if not e.get("health"):
                e["health"] = {"max": float(spec.max_health), "invuln_after_hit_s": 0.0}
            _add_tags(e, "destructible", "target")
            e.setdefault("physics", {"kind": "static", "collision_shape": "mesh",
                                     "mass_kg": 5.0})
            e["static_batch"] = False
            summary["destructibles"].append(str(e.get("id")))
        if item.explosive is not None and not template:
            _ensure(e, EXPLOSIVE_ID, explosive_params(item.explosive))
            # Held by grab, thrown by throw (grab alone just drops it).
            for bid in (GRAB_ID, THROW_ID):
                if not any(_bid(b) == bid for b in e.get("mechanic_bindings") or []):
                    e["mechanic_bindings"].append({"id": bid, "builtin_id": bid,
                                                   "params": {}})
            _add_tags(e, "grabbable", "interactable", "explosive")
            phys = dict(e.get("physics") or {})
            phys.update({"kind": "dynamic"})
            phys.setdefault("collision_shape", "mesh")
            phys.setdefault("mass_kg", 0.4)
            phys.setdefault("friction", 0.7)
            phys.setdefault("restitution", 0.15)
            e["physics"] = phys
            summary["explosives"].append(str(e.get("id")))

    # Challenges: a host, targets, points, a start signal.
    for it in getattr(plan, "items", []) or []:
        if it.builtin_id != TARGET_CHALLENGE_ID:
            continue
        cfg = dict(it.config or {})
        host = next((e for e in entities for b in e.get("mechanic_bindings") or []
                     if isinstance(b, dict) and (_bid(b) == TARGET_CHALLENGE_ID
                                                 or b.get("id") == it.id)), None)
        if host is None:
            host = {"id": f"ptv_challenge_{it.id}"[:64], "name": f"{it.id} (challenge)",
                    "transform": {"position": [0, 0, 0], "rotation": [0, 0, 0, 1], "scale": 1.0},
                    "mechanic_bindings": [], "tags": ["challenge"]}
            doc["entities"].append(host)
            entities.append(host)
        templates = [e for e in entities if "template" in _tags(e) and (
            "challenge_target" in _tags(e)
            or (isinstance(e.get("placement"), dict) and e["placement"].get("anchor_set")))]
        anchor_set = cfg.get("anchor_set") or next(
            (t["placement"]["anchor_set"] for t in templates
             if isinstance(t.get("placement"), dict) and t["placement"].get("anchor_set")), "")
        for t in templates:
            _add_tags(t, "challenge_target")
            # Spawned targets hang or stand where they are put: never dynamic.
            phys = dict(t.get("physics") or {})
            if phys.get("kind") in (None, "dynamic"):
                phys.update({"kind": "kinematic", "mass_kg": max(1.0, float(phys.get("mass_kg") or 1))})
                phys.setdefault("collision_shape", "aabb")
                t["physics"] = phys
        params = {"id": cfg.get("id") or it.id, "anchor_set": anchor_set,
                  "templates": [str(t.get("id")) for t in templates]}
        if len(set(signals)) == 1:
            params["start_signal"] = signals[0]
        for key in ("duration_s", "countdown_s", "max_active", "spawn_interval_s",
                    "target_health", "score_per_hit", "start_signal"):
            if key in cfg:
                params[key] = cfg[key]
        b = _ensure(host, TARGET_CHALLENGE_ID, params)
        b["params"].update({k: v for k, v in params.items()
                            if k in ("anchor_set", "templates") and v})
        cid = b["params"]["id"]
        if not b["params"].get("start_signal") and on_warning:
            on_warning(f"challenge '{cid}' has no start signal (no single button to start it)")
        if not templates and on_warning:
            on_warning(f"challenge '{cid}' has no target template")
        summary["challenges"].append(f"{host.get('id')}: {cid} on '{anchor_set}' "
                                     f"({len(templates)} template(s))")
        # A display with nothing of its own to say shows this challenge.
        for d in displays:
            for db in d.get("mechanic_bindings") or []:
                if _bid(db) == WORLD_DISPLAY_ID and not (db.get("params") or {}).get("lines"):
                    db.setdefault("params", {}).update({
                        "title": f"{{{cid}.state}}",
                        "lines": [f"TIME {{{cid}.time_left:0.0}}",
                                  f"HITS {{{cid}.score}}   BEST {{{cid}.best}}"]})
    return summary
