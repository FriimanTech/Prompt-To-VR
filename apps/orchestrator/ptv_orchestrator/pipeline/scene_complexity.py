"""Scene complexity: what a compiled scene costs, and how to start it.

Runs inside ``scene_compile`` on every compiled scene. It does four things,
all deterministic:

1. Counts the content (entities, meshes, primitives, triangles, textures,
   animated characters).
2. Samples what the player can SEE: eye-height points across every walkable
   region, eight headings each, with the Quest 3 field of view, the scene's
   LOD switch distances and cull distance -- giving visible-triangle and
   draw-call percentiles instead of whole-scene totals (a 3M-triangle level
   can be fine if only 300k are ever in view).
3. Estimates frame cost from those samples. The coefficients are a coarse
   model calibrated on Quest 3 measurements (``logs/runtime/measure_*``); the
   on-device ``perf:`` line replaces the estimate in
   ``runtime_verification.json`` whenever a headset ran the game.
4. Decides per-scene starting settings the user did not lock: initial
   foveation level, whether room/portal culling is safe (only rooms the
   geometry actually closes -- walls on at least three sides), the static
   batching cell size, and marks each entity that may be statically batched.

Nothing here can make a scene fail: it only chooses defaults and reports.
"""

from __future__ import annotations

import math
from typing import Any, Iterable, Optional

#: Quest 3 per-eye field of view (approximate, symmetric for sampling).
H_HALF_FOV_RAD = math.radians(52.0)
EYE_HEIGHT_M = 1.6
HEADINGS = 8
MAX_SAMPLES = 160

#: Triangles the engine's primitive generators emit.
PRIMITIVE_TRIS = {"box": 12, "plane": 2, "quad": 2, "sphere": 960, "cylinder": 128,
                  "capsule": 768, "cone": 64}

#: Calibrated coarse cost model (Quest 3, 4x MSAA, native resolution, two
#: views). Glock Laser Deck measured 7.6 ms GPU at ~10k visible triangles and
#: 11 draws: fill dominates small scenes.
GPU_FILL_MS = 6.8
GPU_MS_PER_100K_TRIS = 1.4
GPU_MS_PER_DRAW = 0.012
CPU_BASE_MS = 0.9
CPU_MS_PER_DRAW = 0.004
CPU_MS_PER_AGENT = 0.12

#: Estimated p95 draws (after instancing) from which multiview's halved draw
#: count outweighs its measured GPU overhead.
MULTIVIEW_MIN_DRAWS_P95 = 120

#: Things that move, are interacted with or are templates never batch.
_NO_BATCH_TAGS = {"dynamic", "grab", "grabbable", "interactive", "held", "npc", "enemy",
                  "spawn_template", "template", "hidden", "projectile", "player",
                  "door", "destructible", "pickup", "weapon"}


def _pos(e: dict) -> tuple[float, float, float]:
    p = (e.get("transform") or {}).get("position") or [0.0, 0.0, 0.0]
    if isinstance(p, dict):
        return float(p.get("x", 0.0)), float(p.get("y", 0.0)), float(p.get("z", 0.0))
    return float(p[0]), float(p[1]), float(p[2])


def _scale(e: dict) -> float:
    s = (e.get("transform") or {}).get("scale", 1.0)
    if isinstance(s, (list, tuple)):
        return float(max(s)) if s else 1.0
    if isinstance(s, dict):
        return float(max(s.get("x", 1.0), s.get("y", 1.0), s.get("z", 1.0)))
    return float(s or 1.0)


def _drawable(e: dict) -> Optional[dict[str, Any]]:
    """Centre, radius, LOD triangle counts and draw key of one entity."""
    x, y, z = _pos(e)
    sc = _scale(e)
    tags = {str(t).lower() for t in (e.get("tags") or [])}
    if e.get("hidden") or "spawn_template" in tags or "template" in tags:
        return None
    mesh = e.get("mesh")
    if isinstance(mesh, dict):
        tris = [int(t) for t in (mesh.get("lod_triangle_counts") or []) if t] or [0]
        lods = list(mesh.get("lod_paths") or [mesh.get("glb_path") or "?"])
        return {"c": (x, y, z), "r": float(mesh.get("bounds_radius_m") or 0.5) * sc,
                "tris": tris, "keys": lods, "skinned": bool(mesh.get("skinned")),
                "kind": "mesh"}
    prim = e.get("primitive")
    if isinstance(prim, dict):
        kind = str(prim.get("kind") or "box")
        size = prim.get("size") or [1.0, 1.0, 1.0]
        if not isinstance(size, (list, tuple)):
            size = [float(size)] * 3
        r = 0.5 * math.sqrt(sum(float(s) ** 2 for s in size[:3])) * sc
        t = PRIMITIVE_TRIS.get(kind, 12)
        return {"c": (x, y, z), "r": r, "tris": [t], "keys": [f"prim:{kind}"],
                "skinned": False, "kind": "primitive", "size": size}
    inline = e.get("mesh_inline")
    if isinstance(inline, dict):
        pos = inline.get("positions") or []
        n = len(inline.get("indices") or []) // 3
        r = 0.0
        for i in range(0, len(pos) - 2, 3):
            r = max(r, math.sqrt(pos[i] ** 2 + pos[i + 1] ** 2 + pos[i + 2] ** 2))
        return {"c": (x, y, z), "r": max(r * sc, 0.05), "tris": [n],
                "keys": [f"inline:{e.get('id')}"], "skinned": False, "kind": "inline"}
    return None


def static_batch_eligible(e: dict) -> bool:
    """Rendering-only merge candidate: never moves, nothing reads it per frame."""
    if e.get("mechanic_bindings") or e.get("attached_to") or e.get("health"):
        return False
    tags = {str(t).lower() for t in (e.get("tags") or [])}
    if tags & _NO_BATCH_TAGS:
        return False
    phys = e.get("physics")
    if isinstance(phys, dict) and str(phys.get("kind") or "static") not in ("static", "none"):
        return False
    mesh = e.get("mesh")
    if isinstance(mesh, dict) and (mesh.get("skinned") or mesh.get("animations")):
        return False
    return bool(e.get("primitive") or e.get("mesh_inline") or isinstance(mesh, dict))


def _regions(doc: dict) -> list[dict]:
    nav = doc.get("navigation") or {}
    out = []
    for r in nav.get("regions") or []:
        mn, mx = r.get("min"), r.get("max")
        if isinstance(mn, list) and isinstance(mx, list) and len(mn) >= 3 and len(mx) >= 3:
            out.append({"id": r.get("id"), "min": mn, "max": mx,
                        "y": float(r.get("floor_y") or mn[1] or 0.0)})
    return out


def _sample_points(regions: list[dict], doc: dict) -> list[tuple[float, float, float]]:
    if not regions:
        sp = (doc.get("player") or {}).get("spawn") or {}
        p = sp.get("position") or [0.0, 0.0, 0.0]
        if isinstance(p, dict):
            p = [p.get("x", 0.0), p.get("y", 0.0), p.get("z", 0.0)]
        return [(float(p[0]), float(p[1]) + EYE_HEIGHT_M, float(p[2]))]
    area = sum(max(0.1, (r["max"][0] - r["min"][0]) * (r["max"][2] - r["min"][2]))
               for r in regions)
    step = max(2.5, math.sqrt(area / MAX_SAMPLES))
    pts: list[tuple[float, float, float]] = []
    for r in regions:
        x0, x1 = r["min"][0], r["max"][0]
        z0, z1 = r["min"][2], r["max"][2]
        nx = max(1, int((x1 - x0) / step))
        nz = max(1, int((z1 - z0) / step))
        for i in range(nx):
            for k in range(nz):
                pts.append((x0 + (i + 0.5) * (x1 - x0) / nx, r["y"] + EYE_HEIGHT_M,
                            z0 + (k + 0.5) * (z1 - z0) / nz))
    if len(pts) > MAX_SAMPLES:
        stride = len(pts) / MAX_SAMPLES
        pts = [pts[int(i * stride)] for i in range(MAX_SAMPLES)]
    return pts


def _region_of(p: tuple[float, float, float], regions: list[dict]) -> Optional[str]:
    for r in regions:
        if (r["min"][0] - 0.25 <= p[0] <= r["max"][0] + 0.25
                and r["min"][2] - 0.25 <= p[2] <= r["max"][2] + 0.25
                and abs(p[1] - r["y"]) < 3.5):
            return str(r["id"])
    return None


def closed_rooms(doc: dict, regions: list[dict]) -> list[str]:
    """Regions the geometry encloses on all four sides (each at least half
    covered, leaving room for doorways) with walls at least 2.4 m tall: only
    these may hide their contents from a player two or more links away."""
    walls = []
    for e in doc.get("entities") or []:
        prim = e.get("primitive")
        if not isinstance(prim, dict) or str(prim.get("kind")) != "box":
            continue
        rot = (e.get("transform") or {}).get("rotation") or [0, 0, 0, 1]
        if isinstance(rot, list) and len(rot) == 4 and (abs(rot[1]) > 0.05 or abs(rot[0]) > 0.05
                                                         or abs(rot[2]) > 0.05):
            continue  # rotated walls: skip rather than mis-measure
        size = prim.get("size") or [1, 1, 1]
        sc = _scale(e)
        sx, sy, sz = (float(size[0]) * sc, float(size[1]) * sc, float(size[2]) * sc)
        if sy < 2.4:
            continue
        x, y, z = _pos(e)
        walls.append((x - sx / 2, x + sx / 2, y - sy / 2, y + sy / 2, z - sz / 2, z + sz / 2))
    rooms = []
    for r in regions:
        x0, x1, z0, z1, fy = r["min"][0], r["max"][0], r["min"][2], r["max"][2], r["y"]
        sides = 0
        for axis, coord, a0, a1 in (("x", x0, z0, z1), ("x", x1, z0, z1),
                                    ("z", z0, x0, x1), ("z", z1, x0, x1)):
            covered = 0.0
            for w in walls:
                if w[2] > fy + 0.5 or w[3] < fy + 2.2:
                    continue
                if axis == "x" and w[0] - 0.7 <= coord <= w[1] + 0.7:
                    covered += max(0.0, min(a1, w[5]) - max(a0, w[4]))
                elif axis == "z" and w[4] - 0.7 <= coord <= w[5] + 0.7:
                    covered += max(0.0, min(a1, w[1]) - max(a0, w[0]))
            if a1 - a0 > 0 and covered / (a1 - a0) >= 0.5:
                sides += 1
        if sides >= 4:
            rooms.append(str(r["id"]))
    return rooms


def neighbourhood(links: dict[str, set[str]], here: str, hops: int = 2) -> set[str]:
    """Regions within ``hops`` links: doorways can line up through a room."""
    seen = {here}
    frontier = {here}
    for _ in range(hops):
        frontier = {n for f in frontier for n in links.get(f, set())} - seen
        seen |= frontier
    return seen


def _percentile(vals: list[float], q: float) -> float:
    if not vals:
        return 0.0
    s = sorted(vals)
    return s[min(len(s) - 1, int(round(q * (len(s) - 1))))]


def sample_visibility(drawables: list[dict], points: Iterable[tuple[float, float, float]],
                      lod_dists: list[float], cull_m: float, *,
                      regions: Optional[list[dict]] = None,
                      rooms: Optional[set[str]] = None,
                      links: Optional[dict[str, set[str]]] = None) -> dict[str, float]:
    """Visible triangles / draw calls per (point, heading)."""
    tris_s: list[float] = []
    draws_s: list[float] = []
    ent_region = []
    if regions and rooms:
        for d in drawables:
            rid = _region_of((d["c"][0], d["c"][1] + 0.5, d["c"][2]), regions)
            ent_region.append(rid if rid in rooms else None)
    for p in points:
        here = _region_of(p, regions) if regions and rooms else None
        near = neighbourhood(links or {}, here) if here else None
        for h in range(HEADINGS):
            yaw = 2.0 * math.pi * h / HEADINGS
            fx, fz = -math.sin(yaw), -math.cos(yaw)
            tris = 0
            keys: set[str] = set()
            for i, d in enumerate(drawables):
                if ent_region and ent_region[i] is not None and near is not None \
                        and ent_region[i] not in near:
                    continue
                dx, dy, dz = d["c"][0] - p[0], d["c"][1] - p[1], d["c"][2] - p[2]
                dist = math.sqrt(dx * dx + dy * dy + dz * dz)
                if dist - d["r"] > cull_m:
                    continue
                if dist > d["r"]:
                    flat = math.hypot(dx, dz)
                    if flat > 1e-3:
                        cosang = (dx * fx + dz * fz) / flat
                        ang = math.acos(max(-1.0, min(1.0, cosang)))
                        slack = math.asin(min(1.0, d["r"] / max(flat, d["r"])))
                        if ang - slack > H_HALF_FOV_RAD:
                            continue
                lod = 0
                for k, sw in enumerate(lod_dists):
                    if dist > sw:
                        lod = k + 1
                lod = min(lod, len(d["tris"]) - 1)
                tris += d["tris"][lod]
                keys.add(d["keys"][min(lod, len(d["keys"]) - 1)])
            tris_s.append(tris)
            draws_s.append(len(keys))
    return {"visible_tris_p50": _percentile(tris_s, 0.5),
            "visible_tris_p95": _percentile(tris_s, 0.95),
            "visible_tris_max": max(tris_s) if tris_s else 0.0,
            "draws_p50": _percentile(draws_s, 0.5),
            "draws_p95": _percentile(draws_s, 0.95),
            "samples": float(len(tris_s))}


def estimate_frame_ms(vis: dict[str, float], perf: dict, agents: int) -> dict[str, float]:
    scale = float(perf.get("render_scale_ceiling") or 1.0)
    msaa = int(perf.get("msaa_samples") or 4)
    fill = GPU_FILL_MS * scale * scale * (1.0 if msaa >= 4 else 0.82)
    gpu = (fill + GPU_MS_PER_100K_TRIS * vis["visible_tris_p95"] / 100_000.0
           + GPU_MS_PER_DRAW * vis["draws_p95"])
    cpu = CPU_BASE_MS + CPU_MS_PER_DRAW * vis["draws_p95"] + CPU_MS_PER_AGENT * agents
    return {"gpu_ms_p95": round(gpu, 2), "cpu_ms_p95": round(cpu, 2)}


def encounter_peak(doc: dict) -> int:
    """NPCs the encounter director(s) may have alive at once."""
    peak = 0
    for e in doc.get("entities") or []:
        for b in (e.get("mechanic_bindings") or []) if isinstance(e, dict) else []:
            bid = str(b.get("builtin_id") or b.get("id") or "")
            if bid == "builtin.encounter_director":
                peak += int((b.get("params") or {}).get("max_concurrent") or 0)
    return peak


def analyse(doc: dict, *, peak_agents: Optional[int] = None) -> dict[str, Any]:
    """Count, sample and estimate one compiled scene (pure)."""
    if peak_agents is None:
        peak_agents = encounter_peak(doc)
    perf = doc.get("performance") or {}
    entities = [e for e in doc.get("entities") or [] if isinstance(e, dict)]
    drawables = [d for d in (_drawable(e) for e in entities) if d]
    regions = _regions(doc)
    links: dict[str, set[str]] = {}
    for ln in (doc.get("navigation") or {}).get("links") or []:
        a, b = str(ln.get("from")), str(ln.get("to"))
        links.setdefault(a, set()).add(b)
        links.setdefault(b, set()).add(a)
    rooms = closed_rooms(doc, regions)
    lod_d = [float(x) for x in perf.get("lod_switch_distances_m") or [12.0, 30.0, 65.0]]
    cull = float(perf.get("cull_distance_m") or 1e5)
    pts = _sample_points(regions, doc)
    vis = sample_visibility(drawables, pts, lod_d, cull)
    vis_rooms = (sample_visibility(drawables, pts, lod_d, cull, regions=regions,
                                   rooms=set(rooms), links=links) if rooms else None)
    skinned = sum(1 for d in drawables if d["skinned"])
    agents = skinned + peak_agents
    area = sum((r["max"][0] - r["min"][0]) * (r["max"][2] - r["min"][2]) for r in regions)
    batchable = [e.get("id") for e in entities if static_batch_eligible(e)]
    counts = {
        "entities": len(entities),
        "mesh_entities": sum(1 for d in drawables if d["kind"] == "mesh"),
        "primitives": sum(1 for d in drawables if d["kind"] == "primitive"),
        "inline_meshes": sum(1 for d in drawables if d["kind"] == "inline"),
        "unique_meshes": len({d["keys"][0] for d in drawables if d["kind"] == "mesh"}),
        "triangles_lod0_total": sum(d["tris"][0] for d in drawables),
        "skinned_entities": skinned,
        "peak_animated_agents": agents,
        "walkable_area_m2": round(area, 1),
        "regions": len(regions),
        "closed_rooms": len(rooms),
        "static_batch_candidates": len(batchable),
    }
    est = estimate_frame_ms(vis, perf, agents)
    out: dict[str, Any] = {"counts": counts, "visibility": vis, "estimate": est,
                           "closed_rooms": rooms, "static_batch_ids": batchable,
                           "estimate_note": "coarse model; on-device perf replaces it"}
    if vis_rooms is not None:
        out["visibility_with_room_culling"] = vis_rooms
        out["estimate_with_room_culling"] = estimate_frame_ms(vis_rooms, perf, agents)
    return out


def decide_initial_settings(perf: dict, analysis: dict) -> list[str]:
    """Per-scene starting values for knobs the user did not lock."""
    locks = perf.get("locks") or {}

    def locked(name: str) -> bool:
        v = locks.get(name)
        return isinstance(v, dict) and v.get("value", "auto") != "auto"

    notes: list[str] = []
    budget_ms = 1000.0 / float(perf.get("refresh_hz") or perf.get("target_fps") or 72)
    est = analysis.get("estimate_with_room_culling") or analysis["estimate"]
    quest_fdm = perf.get("target_device", "quest") == "quest" and perf.get("ffr_fdm", True)
    if not locked("ffr") and quest_fdm:
        # Measured on Quest 3 (Glock Laser Deck): the fragment density map
        # costs ~1 ms at level 0 and level 1 already removes ~40% of GPU time
        # (10.2 -> 5.95 ms), so automatic FFR starts at the least visible
        # level, or medium when the scene is estimated heavy.
        heavy = est["gpu_ms_p95"] > 0.8 * budget_ms
        cap = int((perf.get("governor") or {}).get("max_foveation_level", 3))
        perf["foveation_level"] = min(cap, 2 if heavy else 1)
        notes.append(f"initial FFR level {perf['foveation_level']} (estimated GPU "
                     f"{est['gpu_ms_p95']:.1f} ms of {budget_ms:.1f} ms; the governor "
                     f"may raise it to {cap})")
    # Multiview halves draw calls (a CPU saving) but measured ~10% MORE GPU
    # time on Quest 3 at these loads (Glock Laser Deck 8.9 vs 8.1 ms, Navy
    # SEAL Raid 4.1 vs 3.7 ms, CPU unchanged at 17-64 draws). It is chosen
    # only where the draw count makes the CPU saving matter.
    draws = float((analysis.get("visibility") or {}).get("draws_p95") or 0.0)
    perf["multiview"] = draws >= MULTIVIEW_MIN_DRAWS_P95
    notes.append(f"multiview {'on' if perf['multiview'] else 'off'} "
                 f"(estimated p95 {draws:.0f} draws; threshold {MULTIVIEW_MIN_DRAWS_P95})")
    rooms = analysis.get("closed_rooms") or []
    if not locked("room_culling"):
        perf["room_culling"] = len(rooms) >= 2
        if perf["room_culling"]:
            notes.append(f"room culling on for {len(rooms)} closed room(s)")
    perf["rooms"] = rooms if perf.get("room_culling") else []
    area = float(analysis["counts"].get("walkable_area_m2") or 0.0)
    perf["static_batch_cell_m"] = 16.0 if area > 2500.0 else 8.0
    return notes


def apply_static_batch_flags(doc: dict, ids: Iterable[str]) -> int:
    wanted = set(ids)
    n = 0
    for e in doc.get("entities") or []:
        if isinstance(e, dict) and e.get("id") in wanted:
            e["static_batch"] = True
            n += 1
    return n
