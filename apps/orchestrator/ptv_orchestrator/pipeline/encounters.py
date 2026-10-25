"""Compile the NPC plan's population into the scene.

The NPC plan says which NPC types exist, how many of each start in the scene
(a companion), and which arrive in waves. At runtime that is all the
engine's: ``builtin.encounter_director`` clones hidden templates, and the
built-in behaviours make them follow or fight. This module is the bridge:

* every NPC type that is spawned gets ONE hidden ``template`` entity, with
  its health, faction and per-type behaviour parameters;
* persistent NPCs (a companion) are placed beside the player spawn;
* the encounter plan becomes the director's binding params, with spawn
  zones from the world -- or, when the world declared none, zones derived
  from its walkable regions.
"""

from __future__ import annotations

import copy
import math
from typing import Any, Callable, Iterable, Optional

#: Spawn zones are at least this far from the player spawn.
MIN_ZONE_DISTANCE_M = 12.0
#: How many zones to derive from nav regions when the world declared none.
MAX_DERIVED_ZONES = 6
#: Templates are parked this far below the floor: hidden, not simulated.
TEMPLATE_PARK_Y = -50.0
DIRECTOR_ENTITY_ID = "game_director"
DIRECTOR_ID = "builtin.encounter_director"
FOLLOW_ID = "builtin.npc.follow"
MELEE_ID = "builtin.npc.melee_chase"
LOCOMOTION_ID = "builtin.character_locomotion"

WarnFn = Callable[[str], None]


def _tags(e: dict) -> list[str]:
    return e.setdefault("tags", [])


def _binding(e: dict, mechanic_id: str) -> Optional[dict]:
    for b in e.get("mechanic_bindings") or []:
        if isinstance(b, dict) and (b.get("id") == mechanic_id
                                    or b.get("builtin_id") == mechanic_id):
            return b
    return None


def _ensure_binding(e: dict, mechanic_id: str) -> dict:
    b = _binding(e, mechanic_id)
    if b is None:
        b = {"id": mechanic_id, "builtin_id": mechanic_id, "params": {}}
        e.setdefault("mechanic_bindings", []).append(b)
    b.setdefault("params", {})
    return b


def _behaviour_values(npc: Any) -> set[str]:
    return {getattr(b, "value", str(b)) for b in (npc.behaviors or [])}


def _apply_npc_stats(e: dict, npc: Any, generated_ai: set[str]) -> list[str]:
    """Health, faction and the built-in behaviours for one NPC entity, with
    THIS type's numbers. Returns the behaviours bound."""

    if not e.get("health"):
        e["health"] = {"max": float(npc.combat.max_health), "invuln_after_hit_s": 0.2}
    if not e.get("faction"):
        e["faction"] = {"id": npc.faction, "hostile_to": list(npc.hostile_to)}
    if (e.get("physics") or {}).get("kind") in (None, "static", "dynamic") and \
            npc.movement_style.value != "stationary":
        phys = dict(e.get("physics") or {})
        phys.update({"kind": "kinematic", "collision_shape": "capsule"})
        phys.setdefault("mass_kg", 70.0)
        phys.setdefault("friction", 0.6)
        phys.setdefault("restitution", 0.0)
        e["physics"] = phys

    # An NPC already driven by a generated AI behaviour keeps it: two
    # mechanics writing one locomotion intent fight each frame.
    has_ai = any(
        isinstance(b, dict) and str(b.get("id")) in generated_ai
        for b in e.get("mechanic_bindings") or []
    )
    behaviours = _behaviour_values(npc)
    bound: list[str] = []
    moves = npc.movement_style.value in ("animated_locomotion", "slide_no_animation")
    if moves:
        loco = _ensure_binding(e, LOCOMOTION_ID)["params"]
        loco.setdefault("walk_speed_mps", float(npc.locomotion.walk_speed_mps))
        loco.setdefault("run_speed_mps", float(npc.locomotion.run_speed_mps))
        loco.setdefault("turn_rate_deg_s", float(npc.locomotion.turn_rate_deg_s))
        loco.setdefault("animated", npc.movement_style.value == "animated_locomotion")
    if has_ai or not moves:
        return bound
    if "follow_leader" in behaviours and not behaviours & {"chase", "melee_attack"}:
        p = _ensure_binding(e, FOLLOW_ID)["params"]
        p.update({
            "walk_speed_mps": float(npc.locomotion.walk_speed_mps),
            "run_speed_mps": float(max(npc.locomotion.run_speed_mps,
                                       npc.locomotion.walk_speed_mps)),
        })
        bound.append(FOLLOW_ID)
    elif behaviours & {"chase", "melee_attack"}:
        p = _ensure_binding(e, MELEE_ID)["params"]
        target = "player" if list(npc.hostile_to) == ["player"] else ""
        p.update({
            "target_tag": p.get("target_tag", target),
            "chase_speed_mps": float(npc.locomotion.run_speed_mps),
            "walk_speed_mps": float(min(npc.locomotion.walk_speed_mps,
                                        npc.locomotion.run_speed_mps)),
            "attack_range_m": float(max(0.5, npc.combat.attack_range_m or 1.6)),
            "attack_cooldown_s": float(max(0.2, npc.combat.attack_cooldown_s or 1.5)),
            "damage": float(npc.combat.damage),
            "sight_range_m": float(max(5.0, npc.perception.sight_range_m)),
        })
        bound.append(MELEE_ID)
    return bound


def _spawn_frame(doc: dict, spec_yaw: Optional[float]) -> tuple[list[float], float]:
    spawn = ((doc.get("player") or {}).get("spawn")) or {}
    pos = [float(v) for v in (spawn.get("position") or [0.0, 0.0, 0.0])[:3]]
    if spec_yaw is None:
        q = spawn.get("rotation") or [0, 0, 0, 1]
        x, y, z, w = [float(v) for v in q[:4]]
        fx = -(2.0 * (x * z + w * y))
        fz = -(1.0 - 2.0 * (x * x + y * y))
        spec_yaw = math.atan2(-fx, -fz)
    floor = pos[1] if pos[1] < 1.0 else 0.0
    return [pos[0], floor, pos[2]], spec_yaw


def _beside_spawn(spawn: list[float], yaw: float, k: int) -> list[float]:
    """The k-th companion slot: to the player's right and a little behind,
    then to the left, then further back -- never in front of their view."""
    fwd = (-math.sin(yaw), -math.cos(yaw))
    right = (-fwd[1], fwd[0])
    slots = [(1.4, -0.8), (-1.4, -0.8), (0.0, -1.8), (2.2, -1.8), (-2.2, -1.8)]
    rx, back = slots[k % len(slots)]
    back -= 1.2 * (k // len(slots))
    return [spawn[0] + right[0] * rx + fwd[0] * back, spawn[1],
            spawn[2] + right[1] * rx + fwd[1] * back]


def apply_npc_population(
    doc: dict,
    npc_plan: Any,
    generated_ai_ids: Iterable[str] = (),
    spec_yaw: Optional[float] = None,
    on_warning: Optional[WarnFn] = None,
) -> dict[str, Any]:
    """Make the compiled scene hold exactly the population the NPC plan asks
    for. Mutates ``doc``; returns a summary for logging and tests."""

    warn = on_warning or (lambda _m: None)
    summary: dict[str, Any] = {"templates": {}, "persistent": {}, "behaviours": {}}
    if npc_plan is None or not getattr(npc_plan, "npcs", None):
        return summary
    entities: list[dict] = doc.setdefault("entities", [])
    generated_ai = set(generated_ai_ids)
    npcs = {n.asset_id: n for n in npc_plan.npcs}
    spawned = list(npc_plan.spawned_asset_ids())
    spawn, yaw = _spawn_frame(doc, spec_yaw)
    ids = {e.get("id") for e in entities}

    def by_asset(aid: str) -> list[dict]:
        return [e for e in entities
                if ((e.get("mesh") or {}).get("asset_id") == aid)
                and "player_avatar" not in (e.get("tags") or [])]

    def unique_id(base: str) -> str:
        k, out = 1, base
        while out in ids:
            k += 1
            out = f"{base}_{k}"
        ids.add(out)
        return out

    drop: list[dict] = []
    summary["missing"] = []
    for aid, npc in npcs.items():
        ents = by_asset(aid)
        if not ents:
            # The asset safety net already instantiates every asset whose GLB
            # exists, so reaching here means the model itself is missing.
            if int(npc.persistent_count or 0) > 0 or aid in spawned:
                summary["missing"].append(aid)
            warn(f"NPC '{aid}' has no entity in the scene (its model may have "
                 "failed); it will not appear")
            continue
        is_spawned = aid in spawned
        want = int(npc.persistent_count or 0)
        tmpl: Optional[dict] = None
        if is_spawned:
            tmpl = next((e for e in ents if "template" in _tags(e)), ents[0])
            tags = [t for t in _tags(tmpl) if t != "pooled"]
            if "template" not in tags:
                tags.append("template")
            tmpl["tags"] = tags
            pos = ((tmpl.setdefault("transform", {})).get("position")) or [0, 0, 0]
            tmpl["transform"]["position"] = [float(pos[0]), TEMPLATE_PARK_Y, float(pos[2])]
            summary["templates"][aid] = tmpl["id"]
        live = [e for e in ents if e is not tmpl and "template" not in _tags(e)]
        # Pre-placed copies beyond what the plan places: the director spawns
        # them now, and a second unmanaged copy would be an enemy that never
        # counts toward its wave.
        for extra in live[want:]:
            drop.append(extra)
            warn(f"removed pre-placed '{extra.get('id')}': the encounter plan "
                 f"spawns '{aid}' (persistent copies wanted: {want})")
        live = live[:want]
        while len(live) < want:
            source = live[0] if live else (tmpl or ents[0])
            copy_e = copy.deepcopy(source)
            copy_e["id"] = unique_id(f"{aid}_{len(live) + 1}")
            copy_e["tags"] = [t for t in _tags(copy_e) if t not in ("template", "pooled")]
            entities.append(copy_e)
            live.append(copy_e)
        for k, e in enumerate(live):
            e["tags"] = [t for t in _tags(e) if t != "pooled"]
            if npc.role.value in ("ally", "neutral") and not e.get("attached_to"):
                p = _beside_spawn(spawn, yaw, k)
                t = e.setdefault("transform", {})
                t["position"] = p
                t.setdefault("rotation", [0.0, 0.0, 0.0, 1.0])
        if live:
            summary["persistent"][aid] = [e["id"] for e in live]
        for e in ([tmpl] if tmpl else []) + live:
            bound = _apply_npc_stats(e, npc, generated_ai)
            if bound:
                summary["behaviours"][e["id"]] = bound

    if drop:
        doc["entities"] = [e for e in entities if all(e is not d for d in drop)]
        entities = doc["entities"]

    enc = getattr(npc_plan, "encounters", None)
    if enc is not None and summary["templates"]:
        waves = []
        for w in enc.waves:
            groups = [{"template": summary["templates"][g.asset_id], "count": int(g.count)}
                      for g in w.groups if g.asset_id in summary["templates"]]
            if groups:
                waves.append({"groups": groups})
        params = {
            "mode": enc.mode.value,
            "start": {"kind": enc.start.value, "delay_s": float(enc.start_delay_s),
                      "tag": enc.start_tag},
            "max_concurrent": int(enc.max_concurrent),
            "spawn_interval_s": float(enc.spawn_interval_s),
            "inter_wave_delay_s": float(enc.inter_wave_delay_s),
            "min_player_distance_m": float(enc.min_player_distance_m),
            "waves": waves,
            "zones": [],
        }
        # One director per scene: reuse an entity the SceneSpec already bound
        # it to, and strip any second binding (two directors would each run
        # the waves).
        holders = [e for e in entities if _binding(e, DIRECTOR_ID) is not None]
        director = next((e for e in holders if e.get("id") == DIRECTOR_ENTITY_ID),
                        holders[0] if holders else None)
        for e in holders:
            if e is not director:
                e["mechanic_bindings"] = [
                    b for b in e["mechanic_bindings"]
                    if not (isinstance(b, dict) and (b.get("id") == DIRECTOR_ID
                                                     or b.get("builtin_id") == DIRECTOR_ID))]
                warn(f"removed a second encounter director binding from '{e.get('id')}'")
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
        _ensure_binding(director, DIRECTOR_ID)["params"] = params
        doc["encounters"] = {"mode": enc.mode.value, "waves": len(waves),
                             "templates": dict(summary["templates"]),
                             "director": director["id"]}
        summary["director"] = params
    return summary


def _region_at(regions: list[dict], pos: list[float]) -> Optional[dict]:
    for r in regions:
        lo, hi = r.get("min") or [0, 0, 0], r.get("max") or [0, 0, 0]
        if lo[0] <= pos[0] <= hi[0] and lo[2] <= pos[2] <= hi[2]:
            return r
    return None


def _nearest_inside(regions: list[dict], pos: list[float],
                    margin: float = 0.6) -> tuple[Optional[dict], list[float]]:
    """The closest point that lies inside a region, ``margin`` from its edge."""
    best: tuple[float, Optional[dict], list[float]] = (math.inf, None, pos)
    for r in regions:
        lo, hi = r.get("min") or [0, 0, 0], r.get("max") or [0, 0, 0]
        mx = min(margin, 0.5 * (float(hi[0]) - float(lo[0])))
        mz = min(margin, 0.5 * (float(hi[2]) - float(lo[2])))
        x = min(max(pos[0], float(lo[0]) + mx), float(hi[0]) - mx)
        z = min(max(pos[2], float(lo[2]) + mz), float(hi[2]) - mz)
        d = math.hypot(x - pos[0], z - pos[2])
        if d < best[0]:
            best = (d, r, [x, pos[1], z])
    return best[1], best[2]


def _yaw_quat(facing: Any) -> list[float]:
    """Quaternion [x, y, z, w] turning -Z (forward) toward ``facing``."""
    try:
        fx, fz = float(facing[0]), float(facing[2])
    except (TypeError, IndexError, ValueError):
        return [0.0, 0.0, 0.0, 1.0]
    if math.hypot(fx, fz) < 1e-6:
        return [0.0, 0.0, 0.0, 1.0]
    yaw = math.atan2(-fx, -fz)
    return [0.0, math.sin(0.5 * yaw), 0.0, math.cos(0.5 * yaw)]


def place_persistent_npcs(
    doc: dict,
    persistent: dict[str, list[str]],
    npc_plan: Any,
    on_warning: Optional[WarnFn] = None,
) -> list[str]:
    """Put every NPC placed at load somewhere valid IN THE MERGED WORLD.

    The population step places a companion beside the SceneSpec's spawn, and
    the relocation carries it to the world's spawn. Only now, with the world
    merged, can it be checked against the world itself:

    * a world ``npc_anchor`` that names this NPC (its asset id, family or
      role in ``tags``, or ``asset_id``) wins -- the guard stands at the
      gate the architect built for it;
    * it must stand on declared walkable floor, or it is moved to the
      nearest point that is;
    * its origin is set to floor height plus its foot offset, so the feet
      touch the floor on frame one. That matters most for a STATIONARY NPC,
      which has no locomotion to snap it down later.

    Returns notes describing every change."""

    warn = on_warning or (lambda _m: None)
    notes: list[str] = []
    if not persistent or npc_plan is None:
        return notes
    ents = {e.get("id"): e for e in doc.get("entities") or []}
    regions = [r for r in ((doc.get("navigation") or {}).get("regions") or [])
               if isinstance(r, dict)]
    spawn = (((doc.get("player") or {}).get("spawn")) or {}).get("position") or [0, 0, 0]
    anchors = [a for a in doc.get("npc_anchors") or [] if isinstance(a, dict)
               and isinstance(a.get("position"), list) and len(a["position"]) >= 3]
    used: set[str] = set()
    npcs = {n.asset_id: n for n in npc_plan.npcs}

    for aid, ids in persistent.items():
        npc = npcs.get(aid)
        keys = {aid}
        if npc is not None:
            keys.add(npc.role.value)
            if npc.family:
                keys.add(npc.family)
        for eid in ids:
            e = ents.get(eid)
            if e is None:
                continue
            t = e.setdefault("transform", {})
            pos = [float(v) for v in (t.get("position") or [0, 0, 0])[:3]]
            anchor = next(
                (a for a in anchors if str(a.get("id")) not in used and (
                    a.get("asset_id") == aid
                    or keys & set(a.get("tags") or []))),
                None)
            if anchor is not None:
                used.add(str(anchor.get("id")))
                pos = [float(v) for v in anchor["position"][:3]]
                t["rotation"] = _yaw_quat(anchor.get("facing"))
                notes.append(f"{eid}: placed at world anchor '{anchor.get('id')}'")
            floor = pos[1]
            if regions:
                region = _region_at(regions, pos)
                if region is None:
                    region, moved = _nearest_inside(regions, pos)
                    notes.append(
                        f"{eid}: was off the walkable floor at "
                        f"({pos[0]:.1f}, {pos[2]:.1f}); moved to "
                        f"({moved[0]:.1f}, {moved[2]:.1f})")
                    pos = moved
                if region is not None:
                    lo = region.get("min") or [0, 0, 0]
                    floor = float(region.get("floor_y", lo[1] if len(lo) > 1 else 0.0))
            mesh = e.get("mesh") or {}
            height = float(mesh.get("target_scale_meters") or 1.8)
            foot = float(mesh.get("foot_offset_m") or 0.0) or 0.5 * height
            pos[1] = floor + foot
            t["position"] = pos
            if (npc is not None and npc.role.value == "ally" and anchor is None
                    and math.hypot(pos[0] - float(spawn[0]),
                                   pos[2] - float(spawn[2])) > 8.0):
                warn(f"companion '{eid}' starts {math.hypot(pos[0] - float(spawn[0]), pos[2] - float(spawn[2])):.1f} m "
                     "from the player spawn; the player may not see it")
    return notes


def finalize_encounter_zones(doc: dict, on_warning: Optional[WarnFn] = None) -> int:
    """Give the director its spawn zones once the world is merged: the
    world's declared ``npc_spawn_zones``, or zones derived from its walkable
    regions. Returns how many zones it got."""

    b = next((x for e in doc.get("entities") or []
              for x in [_binding(e, DIRECTOR_ID)] if x is not None), None)
    if b is None:
        return 0
    zones = [z for z in (doc.get("npc_spawn_zones") or []) if isinstance(z, dict)]
    source = "world"
    if not zones:
        regions = list(((doc.get("navigation") or {}).get("regions")) or [])
        spawn = (((doc.get("player") or {}).get("spawn")) or {}).get("position") or [0, 0, 0]
        zones = fallback_spawn_zones(regions, spawn)
        source = "derived from nav regions" if zones else "none (ring around the player)"
        if on_warning:
            on_warning(f"the world declared no npc_spawn_zones; using {source}")
    b.setdefault("params", {})["zones"] = zones
    if isinstance(doc.get("encounters"), dict):
        doc["encounters"]["zones"] = len(zones)
        doc["encounters"]["zones_source"] = source
    return len(zones)


def fallback_spawn_zones(
    regions: list[dict],
    spawn: list[float],
    min_distance_m: float = MIN_ZONE_DISTANCE_M,
) -> list[dict]:
    """Spawn zones from the world's walkable regions, far from the player.

    Each candidate is a region's centre or an inset corner; the farthest
    ones from the spawn are kept, spread so two zones are never on top of
    each other."""

    candidates: list[tuple[float, list[float], float, str]] = []
    sx, sz = float(spawn[0]), float(spawn[2])
    for r in regions:
        lo = r.get("min") or [0.0, 0.0, 0.0]
        hi = r.get("max") or [0.0, 0.0, 0.0]
        fy = float(r.get("floor_y", lo[1] if len(lo) > 1 else 0.0))
        w = float(hi[0]) - float(lo[0])
        d = float(hi[2]) - float(lo[2])
        if w < 1.0 or d < 1.0:
            continue
        radius = max(0.5, min(3.0, 0.25 * min(w, d)))
        inset = min(radius + 0.5, 0.5 * min(w, d))
        cx = 0.5 * (float(lo[0]) + float(hi[0]))
        cz = 0.5 * (float(lo[2]) + float(hi[2]))
        points = [
            (cx, cz),
            (float(lo[0]) + inset, float(lo[2]) + inset),
            (float(hi[0]) - inset, float(lo[2]) + inset),
            (float(lo[0]) + inset, float(hi[2]) - inset),
            (float(hi[0]) - inset, float(hi[2]) - inset),
        ]
        for k, (x, z) in enumerate(points):
            dist = math.hypot(x - sx, z - sz)
            if dist >= min_distance_m:
                candidates.append((dist, [x, fy, z], radius, f"{r.get('id', 'region')}_{k}"))

    candidates.sort(key=lambda c: -c[0])
    chosen: list[dict] = []
    for dist, pos, radius, zid in candidates:
        if any(math.hypot(pos[0] - z["position"][0], pos[2] - z["position"][2]) < 8.0
               for z in chosen):
            continue
        chosen.append({"id": f"derived_{zid}", "position": pos,
                       "radius": radius, "tags": ["derived"]})
        if len(chosen) >= MAX_DERIVED_ZONES:
            break
    return chosen
