"""Compile game_design.json into the scene: one game director entity carrying
``builtin.ui_panel`` (the end-screen / pause panel), ``builtin.game_flow``
(the loop) and ``builtin.game_hud`` (the wrist HUD), with the design as their
params -- and check, statically, that every path through the flow can actually
complete in THIS scene.

The static check is what stops a dead end from shipping: an objective that
needs a spawn point the world never declared, a "clear the waves" goal in a
scene with no encounter, an end-screen button the engine does not know.
"""

from __future__ import annotations

from typing import Any, Callable, Optional

from ptv_schemas import GameDesign

FLOW_ID = "builtin.game_flow"
HUD_ID = "builtin.game_hud"
PANEL_ID = "builtin.ui_panel"
DIRECTOR_ENTITY_ID = "ptv_game_director"
FLOW_PANEL = "ptv_game_flow"

_END_BUTTONS = {"try_again", "play_again", "keep_playing", "leave_game"}
_PAUSE_BUTTONS = {"resume", "restart", "leave_game"}

WarnFn = Callable[[str], None]


def _binds(b: Any, mechanic_id: str) -> bool:
    return isinstance(b, dict) and (b.get("id") == mechanic_id
                                    or b.get("builtin_id") == mechanic_id)


def _quat_yaw_deg(q: list[float]) -> float:
    import math

    x, y, z, w = (list(q) + [0.0, 0.0, 0.0, 1.0])[:4]
    fx = -(2.0 * (x * z + w * y))
    fz = -(1.0 - 2.0 * (x * x + y * y))
    return math.degrees(math.atan2(-fx, -fz))


def flow_params(design: GameDesign, doc: dict) -> dict:
    spawn = ((doc.get("player") or {}).get("spawn")) or {}
    markers = {
        str(sp.get("id")): [float(v) for v in (sp.get("position") or [0, 0, 0])[:3]]
        for sp in doc.get("spawn_points") or []
        if isinstance(sp, dict) and sp.get("id")
    }
    screens = {
        s.kind.value: {
            "title": s.title,
            "subtitle": s.subtitle,
            "buttons": [b.value for b in s.buttons],
            "show_stats": s.show_stats,
        }
        for s in design.end_screens
    }
    pv = design.player_vitality
    ol = design.on_life_lost
    return {
        "panel_id": FLOW_PANEL,
        "experience": design.experience.value,
        "has_health": pv.has_health,
        "invulnerable": pv.invulnerable,
        "max_health": float(pv.max_health),
        "regen_per_s": float(pv.regen_per_s),
        "lives": int(design.lives.count) if design.lives.count is not None else -1,
        "free_play": design.free_play,
        "respawn": {
            "at": ol.respawn_at.value,
            "delay_s": float(ol.delay_s),
            "restore_health": ol.restore_health,
            "keep_loadout": ol.keep_loadout,
            "invulnerable_s": float(ol.invulnerable_s),
            "enemy_policy": ol.enemy_policy.value,
            "clear_radius_m": float(ol.clear_radius_m),
        },
        "spawn": {
            "position": [float(v) for v in (spawn.get("position") or [0.0, 0.0, 0.0])[:3]],
            "yaw_deg": _quat_yaw_deg(spawn.get("rotation") or [0, 0, 0, 1]),
        },
        "markers": markers,
        "objectives": [
            {
                "id": o.id,
                "kind": o.kind.value,
                "description": o.description,
                "duration_s": float(o.duration_s or 0.0),
                "count": int(o.count or 0),
                "target_ids": list(o.target_ids),
                "radius_m": float(o.radius_m) if o.radius_m > 0 else 2.0,
            }
            for o in design.objectives
        ],
        "victory": {"kind": design.victory.kind.value,
                    "objective_ids": list(design.victory.objective_ids)},
        "failure": [{"kind": f.kind.value, "objective_ids": list(f.objective_ids)}
                    for f in design.failure],
        "screens": screens,
        "pause": {"enabled": design.pause_menu.enabled,
                  "buttons": [b.value for b in design.pause_menu.buttons]},
        "reset": design.reset_policy.model_dump(),
    }


def apply_game_flow(doc: dict, design: Optional[GameDesign],
                    on_warning: Optional[WarnFn] = None) -> Optional[dict]:
    """Bind the loop onto one director entity. Returns the flow params, or
    ``None`` when the project has no game design (old projects keep the
    run flow their generated code provides)."""
    if design is None:
        return None
    warn = on_warning or (lambda _m: None)
    entities = doc.setdefault("entities", [])

    # Exactly one of each, on the director: two flows would each spend a life.
    director = next((e for e in entities if e.get("id") == DIRECTOR_ENTITY_ID), None)
    for e in entities:
        if e is director:
            continue
        before = len(e.get("mechanic_bindings") or [])
        e["mechanic_bindings"] = [
            b for b in (e.get("mechanic_bindings") or [])
            if not (_binds(b, FLOW_ID) or _binds(b, HUD_ID)
                    or (_binds(b, PANEL_ID) and (b.get("params") or {}).get("panel_id")
                        == FLOW_PANEL))
        ]
        if len(e["mechanic_bindings"]) != before:
            warn(f"moved the game loop binding off '{e.get('id')}' onto the game director")
    if director is None:
        director = {
            "id": DIRECTOR_ENTITY_ID,
            "name": "Game Director",
            "transform": {"position": [0.0, 0.0, 0.0],
                          "rotation": [0.0, 0.0, 0.0, 1.0], "scale": 1.0},
            "mesh": None,
            "physics": None,
            "mechanic_bindings": [],
            "tags": ["game_director", "player_system"],
        }
        entities.append(director)
    bindings = [b for b in (director.get("mechanic_bindings") or [])
                if not (_binds(b, FLOW_ID) or _binds(b, HUD_ID)
                        or (_binds(b, PANEL_ID)
                            and (b.get("params") or {}).get("panel_id") == FLOW_PANEL))]
    params = flow_params(design, doc)
    bindings.append({"id": PANEL_ID, "builtin_id": PANEL_ID, "params": {
        "panel_id": FLOW_PANEL, "open_on_start": False, "width_m": 0.56,
        "row_height_m": 0.12, "distance_m": 1.4, "below_eye_m": 0.15,
        "hit_radius_m": 0.08}})
    bindings.append({"id": FLOW_ID, "builtin_id": FLOW_ID, "params": params})
    hud = design.hud
    if hud.health or hud.lives or hud.timer or hud.score or hud.objective:
        bindings.append({"id": HUD_ID, "builtin_id": HUD_ID, "params": {
            "health": hud.health, "lives": hud.lives, "timer": hud.timer,
            "score": hud.score, "objective": hud.objective}})
    director["mechanic_bindings"] = bindings
    doc["game_flow"] = {"director": DIRECTOR_ENTITY_ID, "experience": params["experience"],
                        "lives": params["lives"], "has_health": params["has_health"],
                        "objectives": [o["id"] for o in params["objectives"]]}
    return params


def game_flow_violations(doc: dict, params: Optional[dict]) -> list[str]:
    """Every path through the loop must be completable in this scene."""
    if params is None:
        return []
    out: list[str] = []
    markers = params.get("markers") or {}
    entities = [e for e in doc.get("entities") or [] if isinstance(e, dict)]
    assets = {str((e.get("mesh") or {}).get("asset_id")) for e in entities if e.get("mesh")}
    has_director = any(_binds(b, "builtin.encounter_director")
                       for e in entities for b in e.get("mechanic_bindings") or [])
    for o in params.get("objectives") or []:
        kind, oid, targets = o["kind"], o["id"], o.get("target_ids") or []
        if kind == "reach_location":
            missing = [t for t in targets if t not in markers]
            if not targets or missing:
                out.append(f"objective '{oid}' must reach spawn point(s) "
                           f"{missing or targets or '(none named)'} that the world does "
                           f"not declare (declared: {sorted(markers) or 'none'})")
        elif kind == "clear_waves" and not has_director:
            out.append(f"objective '{oid}' clears waves, but this scene has no encounter")
        elif kind == "collect":
            missing = [t for t in targets if t not in assets]
            if missing:
                out.append(f"objective '{oid}' collects {missing}, which are not in the scene")
        elif kind == "survive_time" and not o.get("duration_s"):
            out.append(f"objective '{oid}' survives for no time at all")
    for kind, screen in (params.get("screens") or {}).items():
        bad = [b for b in screen.get("buttons") or [] if b not in _END_BUTTONS]
        if bad:
            out.append(f"{kind} screen has buttons the engine cannot act on: {bad}")
        if not screen.get("buttons"):
            out.append(f"{kind} screen has no buttons; the player would be stuck on it")
    pause = params.get("pause") or {}
    bad = [b for b in pause.get("buttons") or [] if b not in _PAUSE_BUTTONS]
    if bad:
        out.append(f"pause menu has buttons the engine cannot act on: {bad}")
    can_die = params.get("has_health") and not params.get("invulnerable")
    if can_die and params.get("lives", -1) != -1 and "game_over" not in (params.get("screens") or {}):
        out.append("the player can run out of lives but there is no game_over screen")
    leave = any("leave_game" in (s.get("buttons") or [])
                for s in (params.get("screens") or {}).values()) or (
        pause.get("enabled") and "leave_game" in (pause.get("buttons") or []))
    if not leave:
        out.append("nothing in the flow offers Leave Game")
    return out
