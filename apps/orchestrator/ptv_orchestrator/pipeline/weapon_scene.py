"""Bind the engine's weapon built-ins into a compiled scene document.

Runs inside ``scene_compile`` once every entity exists. With an
InteractionDesign, the design -- not the SceneSpec agent -- decides which
mechanic runs a weapon, so this pass is deterministic and idempotent:

* each entity showing a designed weapon / bow / melee item / shield gets its
  ``builtin.weapon`` / ``builtin.bow`` / ``builtin.melee_weapon`` /
  ``builtin.shield`` binding with the design's numbers; player-wielded items
  are ``grabbable``;
* stale bindings go: ``builtin.recoil`` (the weapon kicks itself) and
  ``generated.*`` ids the mechanic plan no longer has (a removed
  ``shooting_hub`` / ``pistol_fire``);
* every projectile with a mesh gets ONE hidden template entity (tag
  ``template``) the projectile pool draws its rounds with; a bow's arrow gets
  a grabbable ``arrow`` template plus a ``builtin.item_source`` entity on the
  player's body that draws copies of it;
* an NPC whose plan has ``ranged_attack`` gets
  ``builtin.npc.ranged_attack`` and a weapon: the designed NPC weapon,
  attached to its right hand by its grip socket (``attached_to.socket``), or
  -- when the character was modelled holding its gun -- ``builtin.weapon``
  on the character itself.

``weapon_violations`` then checks the result.
"""

from __future__ import annotations

from typing import Any, Callable, Iterable, Optional

from ptv_schemas import InteractionDesign

from .game_flow_scene import DIRECTOR_ENTITY_ID, HUD_ID
from .weapon_plan import (
    BOW_ID,
    MELEE_ID,
    RANGED_NPC_ID,
    SHIELD_ID,
    SOURCE_ID,
    WEAPON_ID,
    body_slot,
    engine_bindings,
    projectile_params,
)

WarnFn = Callable[[str], None]
MeshFor = Callable[[str], Optional[dict]]

GRAB_ID = "builtin.grab"
_IDENTITY = {"position": [0.0, -50.0, 0.0], "rotation": [0.0, 0.0, 0.0, 1.0], "scale": 1.0}
_ENGINE_IDS = {WEAPON_ID, BOW_ID, MELEE_ID, SHIELD_ID}


def _bid(b: dict) -> str:
    return str(b.get("builtin_id") or b.get("id") or "")


def _tags(e: dict) -> list[str]:
    return list(e.get("tags") or [])


def _is_template(e: dict) -> bool:
    return "template" in _tags(e)


def _asset(e: dict) -> str:
    return str(((e.get("mesh") or {}).get("asset_id")) or "")


def _has_socket(e: dict, name: str) -> bool:
    return name in (((e.get("mesh") or {}).get("sockets")) or {})


def proj_template_id(pid: str) -> str:
    return f"ptv_projectile_{pid}"[:64]


def arrow_template_id(asset_id: str) -> str:
    return f"ptv_arrow_{asset_id}"[:64]


def apply_weapons(
    doc: dict,
    design: Optional[InteractionDesign],
    *,
    known_generated: set[str],
    mesh_for: MeshFor,
    npc_plan: Any = None,
    on_warning: Optional[WarnFn] = None,
    drawn_fx_assets: Iterable[str] = (),
) -> dict:
    """Mutates ``doc``; returns a summary (``{}`` when there is no design).

    ``drawn_fx_assets`` are effect meshes the engine draws itself (a mesh of a
    laser beam, a muzzle flash) that an older manifest bought anyway; an
    entity showing one is removed -- placed in front of a gun, it is a wall
    every shot hits."""
    if design is None:
        return {}
    warn = on_warning or (lambda _m: None)
    entities: list[dict] = doc.setdefault("entities", [])
    covered = {i.asset_id: i for i in design.items if engine_bindings(i, design)}
    summary: dict[str, Any] = {"weapons": [], "templates": [], "sources": [],
                               "npc_ranged": [], "stripped": []}

    # 1. Projectile meshes are not props: an auto-placed bullet floating in
    #    the level is a planning artifact.
    proj_assets = {p.asset_id for p in design.projectiles if p.asset_id}
    bow_arrows = {i.bow.arrow_asset_id for i in design.items if i.bow and i.bow.arrow_asset_id}
    drawn_fx = set(drawn_fx_assets)
    keep: list[dict] = []
    for e in entities:
        a = _asset(e)
        if (a in proj_assets and not _is_template(e) and a not in bow_arrows
                and str(e.get("id", "")).startswith("auto_")):
            summary["stripped"].append(e.get("id"))
            continue
        if a in drawn_fx and not (e.get("mechanic_bindings") or []):
            summary["stripped"].append(e.get("id"))
            continue
        keep.append(e)
    entities[:] = keep

    # 2. Stale bindings. Item sources and the weapon built-ins are this
    #    pass's to bind: one the SceneSpec authored has no template / no
    #    design numbers (a quiver source naming template "None").
    for e in entities:
        out = []
        for b in e.get("mechanic_bindings") or []:
            mid = str(b.get("id") or "")
            if covered and _bid(b) == "builtin.recoil":
                summary["stripped"].append(f"{e.get('id')}:{mid}")
                continue
            if _bid(b) == SOURCE_ID and not str(e.get("id", "")).startswith("ptv_source_"):
                summary["stripped"].append(f"{e.get('id')}:{mid}")
                continue
            if _bid(b) in _ENGINE_IDS and _asset(e) not in covered and not e.get("faction"):
                summary["stripped"].append(f"{e.get('id')}:{mid}")
                continue
            if mid.startswith("generated.") and known_generated and \
                    mid.split(".", 1)[1] not in known_generated:
                summary["stripped"].append(f"{e.get('id')}:{mid}")
                continue
            out.append(b)
        e["mechanic_bindings"] = out
    if summary["stripped"]:
        warn(f"removed stale weapon bindings / props {summary['stripped']}: the "
             "engine's weapon built-ins run these items")

    # 3. Projectile visual templates (one per projectile that has a mesh).
    by_id = {e.get("id"): e for e in entities}
    proj_tmpl: dict[str, str] = {}
    for p in design.projectiles:
        if not p.asset_id or p.asset_id in bow_arrows:
            continue
        mesh = mesh_for(p.asset_id)
        if mesh is None:
            warn(f"projectile '{p.id}' names asset '{p.asset_id}' with no mesh; "
                 "it draws as a tracer")
            continue
        tid = proj_template_id(p.id)
        if tid not in by_id:
            ent = {"id": tid, "name": f"{p.id} (projectile template)",
                   "transform": dict(_IDENTITY), "mesh": mesh, "physics": None,
                   "mechanic_bindings": [], "tags": ["template", "projectile"]}
            entities.append(ent)
            by_id[tid] = ent
        proj_tmpl[p.id] = tid
        summary["templates"].append(tid)

    # 4. Arrow templates + quiver sources.
    arrow_tmpl: dict[str, str] = {}
    for item in design.items:
        if item.bow is None or not item.bow.arrow_asset_id:
            continue
        aid = item.bow.arrow_asset_id
        mesh = mesh_for(aid)
        if mesh is None:
            warn(f"bow '{item.asset_id}': arrow asset '{aid}' has no mesh; arrows "
                 "draw as streaks and cannot be drawn from a quiver")
            continue
        tid = arrow_template_id(aid)
        if tid not in by_id:
            ent = {"id": tid, "name": f"{aid} (arrow template)",
                   "transform": dict(_IDENTITY), "mesh": mesh,
                   "physics": {"kind": "dynamic", "collision_shape": "aabb",
                               "mass_kg": 0.03, "friction": 0.5, "restitution": 0.05},
                   "mechanic_bindings": [],
                   "tags": ["template", "arrow", "grabbable", "interactable"]}
            entities.append(ent)
            by_id[tid] = ent
        arrow_tmpl[aid] = tid
        summary["templates"].append(tid)
        src = item.bow.arrow_source
        _add_source(entities, by_id, tid, aid, src, summary)
    for item in design.items:
        if item.item_source is None or item.bow is not None:
            continue
        mesh = mesh_for(item.asset_id)
        if mesh is None:
            continue
        tid = f"ptv_template_{item.asset_id}"[:64]
        if tid not in by_id:
            ent = {"id": tid, "name": f"{item.asset_id} (template)",
                   "transform": dict(_IDENTITY), "mesh": mesh,
                   "physics": {"kind": "dynamic", "collision_shape": "aabb",
                               "mass_kg": 0.3, "friction": 0.6, "restitution": 0.05},
                   "mechanic_bindings": [], "tags": ["template", "grabbable", "interactable"]}
            entities.append(ent)
            by_id[tid] = ent
        summary["templates"].append(tid)
        _add_source(entities, by_id, tid, item.asset_id, item.item_source, summary)

    # 5. The weapons themselves.
    player_items = {a: i for a, i in covered.items()
                    if not i.wielders or "player" in [w.value for w in i.wielders]}
    for e in entities:
        a = _asset(e)
        item = covered.get(a)
        if item is None or _is_template(e) or str(e.get("id", "")).startswith("ptv_"):
            continue
        bindings = [b for b in (e.get("mechanic_bindings") or []) if _bid(b) not in _ENGINE_IDS]
        for builtin_id, params in engine_bindings(item, design):
            params = _with_templates(dict(params), item, proj_tmpl, arrow_tmpl)
            bindings.append({"id": builtin_id, "builtin_id": builtin_id, "params": params})
        if a in player_items and not e.get("attached_to") and \
                not any(_bid(b) == GRAB_ID for b in bindings):
            # The hold solver (sockets, two hands, aim alignment) is
            # builtin.grab's; one player-wide instance, so binding it here
            # only marks this item as one it holds.
            bindings.append({"id": GRAB_ID, "builtin_id": GRAB_ID, "params": {}})
        e["mechanic_bindings"] = bindings
        if a in player_items and not e.get("attached_to"):
            tags = _tags(e)
            for t in ("grabbable", "interactable"):
                if t not in tags:
                    tags.append(t)
            e["tags"] = tags
            if not e.get("physics"):
                e["physics"] = {"kind": "dynamic", "collision_shape": "aabb",
                                "mass_kg": 1.0, "friction": 0.6, "restitution": 0.05}
        summary["weapons"].append(f"{e.get('id')}({a})")

    # 6. NPCs that shoot.
    if npc_plan is not None:
        _bind_ranged_npcs(entities, by_id, design, npc_plan, covered, proj_tmpl,
                          arrow_tmpl, mesh_for, summary, warn)

    # 7. A player weapon with ammo to count shows it on the wrist HUD.
    counted = any(i.weapon is not None and not i.weapon.ammo.infinite
                  and (i.weapon.ammo.magazine > 0 or i.weapon.ammo.heat_per_shot > 0)
                  for i in player_items.values())
    director = by_id.get(DIRECTOR_ENTITY_ID)
    if counted and director is not None and not any(
            _bid(b) == HUD_ID for b in director.get("mechanic_bindings") or []):
        director.setdefault("mechanic_bindings", []).append({
            "id": HUD_ID, "builtin_id": HUD_ID,
            "params": {"health": False, "lives": False, "timer": False, "score": False,
                       "objective": False, "ammo": True}})

    # 8. The projectile table: every designed projectile by id, so code can
    #    launch "the rocket" with the design's flight and look.
    doc["projectiles"] = [{
        "id": p.id, "kind": p.kind.value,
        "template": proj_tmpl.get(p.id) or arrow_tmpl.get(p.asset_id, ""),
        **projectile_params(p),
        "diameter_m": p.diameter_m, "length_m": p.length_m,
        "needs_mesh": p.needs_mesh, "asset_id": p.asset_id,
    } for p in design.projectiles]

    doc["weapons"] = {k: v for k, v in summary.items() if v}
    return summary


def _with_templates(params: dict, item: Any, proj_tmpl: dict, arrow_tmpl: dict) -> dict:
    if item.weapon is not None and item.weapon.projectile_id in proj_tmpl:
        params.setdefault("projectile", {})["visual_template"] = proj_tmpl[item.weapon.projectile_id]
    if item.bow is not None and item.bow.arrow_asset_id in arrow_tmpl:
        params["arrow_template"] = arrow_tmpl[item.bow.arrow_asset_id]
    return params


def _add_source(entities: list, by_id: dict, template_id: str, asset_id: str,
                src: Any, summary: dict) -> None:
    sid = f"ptv_source_{asset_id}"[:64]
    if sid in by_id:
        return
    params: dict[str, Any] = {"template": template_id, "radius_m": src.radius_m,
                              "count": src.count, "body_slot": body_slot(src.body_slot)}
    if not src.body_slot and src.world_tag:
        anchor = next((e for e in entities if src.world_tag in _tags(e)), None)
        if anchor is not None:
            params["world_point"] = list((anchor.get("transform") or {}).get(
                "position", [0.0, 0.0, 0.0]))
    ent = {"id": sid, "name": f"{asset_id} source",
           "transform": {"position": [0.0, 0.0, 0.0], "rotation": [0.0, 0.0, 0.0, 1.0],
                         "scale": 1.0},
           "mesh": None, "physics": None, "tags": ["item_source", "player_system"],
           "mechanic_bindings": [{"id": SOURCE_ID, "builtin_id": SOURCE_ID, "params": params}]}
    entities.append(ent)
    by_id[sid] = ent
    summary["sources"].append(sid)


def _npc_weapon_params(npc: Any, design: InteractionDesign, item: Any,
                       proj_tmpl: dict, arrow_tmpl: dict) -> dict:
    if item is not None:
        for builtin_id, params in engine_bindings(item, design):
            if builtin_id == WEAPON_ID:
                return _with_templates(dict(params), item, proj_tmpl, arrow_tmpl)
    c = npc.combat
    return {"delivery": "hybrid", "fire_mode": "auto", "rpm": 240.0, "pellets": 1,
            "spread_deg": 1.5, "range_m": max(10.0, c.attack_range_m * 2.0),
            "damage": c.damage, "ammo": {"infinite": True}, "visual": "tracer",
            "color_rgb": [1.0, 0.35, 0.2], "impact": "sparks", "muzzle_flash": True,
            "recoil": {"kick_back_m": 0.0, "kick_up_deg": 0.0}, "haptic_amplitude": 0.0}


def _bind_ranged_npcs(entities: list, by_id: dict, design: InteractionDesign, npc_plan: Any,
                      covered: dict, proj_tmpl: dict, arrow_tmpl: dict, mesh_for: MeshFor,
                      summary: dict, warn: WarnFn) -> None:
    npc_items = [i for i in covered.values() if i.weapon is not None
                 and "npc" in [w.value for w in i.wielders]]
    for npc in getattr(npc_plan, "npcs", []) or []:
        behaviours = [getattr(b, "value", str(b)) for b in (npc.behaviors or [])]
        if "ranged_attack" not in behaviours:
            continue
        hosts = [e for e in entities if _asset(e) == npc.asset_id
                 and not str(e.get("id", "")).startswith("ptv_")]
        if not hosts:
            continue
        c = npc.combat
        ranged = {"preferred_range_m": round(max(3.0, c.attack_range_m * 0.8), 2),
                  "min_range_m": round(max(1.5, c.attack_range_m * 0.3), 2),
                  "sight_m": npc.perception.sight_range_m,
                  "move_speed_mps": npc.locomotion.walk_speed_mps,
                  "burst_pause_s": max(0.4, c.attack_cooldown_s)}
        chosen = npc_items[0] if npc_items else None
        for host in hosts:
            bindings = [b for b in (host.get("mechanic_bindings") or [])
                        if _bid(b) not in (RANGED_NPC_ID, "builtin.npc.melee_chase")]
            bindings.append({"id": RANGED_NPC_ID, "builtin_id": RANGED_NPC_ID,
                             "params": dict(ranged)})
            host["mechanic_bindings"] = bindings
            carried = [e for e in entities
                       if (e.get("attached_to") or {}).get("entity_id") == host.get("id")
                       and any(_bid(b) == WEAPON_ID for b in e.get("mechanic_bindings") or [])]
            if carried:
                summary["npc_ranged"].append(f"{host['id']}->{carried[0]['id']}")
                continue
            if chosen is not None and mesh_for(chosen.asset_id) is not None:
                wid = f"ptv_gun_{host['id']}"[:64]
                if wid not in by_id:
                    grip = chosen.hold.primary_socket or "grip"
                    gun = {"id": wid, "name": f"{chosen.asset_id} ({host['id']})",
                           "transform": dict(host.get("transform") or _IDENTITY),
                           "mesh": mesh_for(chosen.asset_id), "physics": None,
                           "tags": ["npc_weapon"] + (["template"] if _is_template(host) else []),
                           "attached_to": {"entity_id": host["id"], "anchor": "hand_r",
                                           "socket": grip,
                                           "position": [0.0, 0.0, 0.0],
                                           "rotation": [0.0, 0.0, 0.0, 1.0]},
                           "mechanic_bindings": [{
                               "id": WEAPON_ID, "builtin_id": WEAPON_ID,
                               "params": _npc_weapon_params(npc, design, chosen, proj_tmpl,
                                                            arrow_tmpl)}]}
                    gun["mechanic_bindings"][0]["params"].setdefault("ammo", {})["infinite"] = True
                    entities.append(gun)
                    by_id[wid] = gun
                summary["npc_ranged"].append(f"{host['id']}->{wid}")
                continue
            # The character IS the gun: a turret, a drone, or a soldier
            # modelled holding its rifle. Without a muzzle socket the shot
            # leaves the front of its bounds -- it must still shoot, or it
            # never threatens anyone and its wave never ends.
            host["mechanic_bindings"].append({
                "id": WEAPON_ID, "builtin_id": WEAPON_ID,
                "params": _npc_weapon_params(npc, design, None, proj_tmpl, arrow_tmpl)})
            if not _has_socket(host, "muzzle"):
                warn(f"NPC '{host['id']}' ({npc.asset_id}) shoots but has no weapon in the "
                     "design and no muzzle socket: it fires from the front of its body")
            summary["npc_ranged"].append(f"{host['id']}->self")


def weapon_violations(doc: dict, design: Optional[InteractionDesign]) -> tuple[list[str], list[str]]:
    """(errors, warnings) for the weapon bindings in a compiled scene."""
    errors: list[str] = []
    warnings: list[str] = []
    if design is None:
        return errors, warnings
    entities = doc.get("entities") or []
    by_id = {e.get("id"): e for e in entities}
    for e in entities:
        for b in e.get("mechanic_bindings") or []:
            bid = _bid(b)
            p = b.get("params") or {}
            if bid == WEAPON_ID:
                npc_self = any(_bid(x) == RANGED_NPC_ID for x in e.get("mechanic_bindings") or [])
                if not _has_socket(e, p.get("muzzle_socket", "muzzle")) and e.get("mesh"):
                    (warnings if npc_self else errors).append(
                        f"'{e['id']}' is a builtin.weapon with no "
                        f"'{p.get('muzzle_socket', 'muzzle')}' socket on its mesh"
                        + (" (an NPC firing from its body)" if npc_self else ""))
                tmpl = (p.get("projectile") or {}).get("visual_template")
                if tmpl and tmpl not in by_id:
                    errors.append(f"'{e['id']}' projectile template '{tmpl}' does not exist")
            elif bid == BOW_ID:
                for s in ("limb_tip_top", "limb_tip_bottom", "grip"):
                    if not _has_socket(e, s):
                        errors.append(f"bow '{e['id']}' has no '{s}' socket")
                if p.get("arrow_template") and p["arrow_template"] not in by_id:
                    errors.append(f"bow '{e['id']}' arrow template "
                                  f"'{p['arrow_template']}' does not exist")
            elif bid == MELEE_ID:
                tip = p.get("strike_tip_socket", "strike_tip")
                face = p.get("strike_face_socket", "strike_face")
                if e.get("mesh") and not _has_socket(e, tip) and not _has_socket(e, face):
                    errors.append(f"melee '{e['id']}' has neither a '{tip}' nor a "
                                  f"'{face}' socket: nothing can strike")
            elif bid == SOURCE_ID:
                if p.get("template") not in by_id:
                    errors.append(f"item source '{e['id']}' names missing template "
                                  f"'{p.get('template')}'")
            elif bid == "builtin.recoil":
                errors.append(f"'{e['id']}' still binds builtin.recoil next to the "
                              "engine weapon (double recoil)")
    for item in design.items:
        if item.weapon is None and item.bow is None:
            continue
        if not any(_asset(e) == item.asset_id for e in entities):
            warnings.append(f"designed weapon '{item.asset_id}' is not in this scene")
    in_scene = {_asset(e) for e in entities}
    fired = {i.weapon.projectile_id for i in design.items
             if i.weapon is not None and i.weapon.projectile_id and i.asset_id in in_scene}
    fired |= {i.bow.arrow_projectile_id for i in design.items
              if i.bow is not None and i.asset_id in in_scene}
    for p in doc.get("projectiles") or []:
        if p.get("id") in fired and p.get("needs_mesh") and not p.get("template"):
            errors.append(f"projectile '{p['id']}' must be seen (needs_mesh) but asset "
                          f"'{p.get('asset_id') or '-'}' has no mesh to draw it with")
    perf = doc.get("performance") or {}
    if fired and perf and "max_live_projectiles" not in perf:
        warnings.append("the performance block has no projectile / FX caps (asset_optimization "
                        "predates them); the runtime uses its defaults")
    elif fired and perf:
        need = 0.0
        for i in design.items:
            w = i.weapon
            if w is None or w.projectile_id not in fired or w.fire_system.value != "projectile":
                continue
            p = design.projectile(w.projectile_id)
            life = min(p.lifetime_s if p else 3.0, 3.0)
            need = max(need, w.rpm / 60.0 * w.pellets * life)
        if need > float(perf.get("max_live_projectiles", 64)):
            warnings.append(f"sustained fire can keep ~{need:.0f} projectiles alive but the "
                            f"pool holds {perf['max_live_projectiles']}: the oldest are "
                            "recycled early")
    return errors, warnings


__all__ = ["apply_weapons", "weapon_violations", "projectile_params",
           "proj_template_id", "arrow_template_id"]
