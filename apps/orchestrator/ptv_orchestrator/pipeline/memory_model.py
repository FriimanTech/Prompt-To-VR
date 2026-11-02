"""Quest 3 memory model: what a game will cost in PSS, before it runs.

Meta's limit for an immersive Quest 3 / 3S app is 5.75 GiB PSS. The engine
plans against its own, lower targets (``PerformanceProfile``): 4.5 GiB steady
state and a 5.0 GiB peak ceiling. This module is the pure byte math -- no
I/O except the helpers at the bottom -- shared by ``asset_optimization``
(planned sizes, before anything is built), ``scene_compile`` (exact sizes of
what was built) and the tests.

Every category is labelled ``exact`` (counted from real files or fixed
engine sizes) or ``estimate`` (a model). The calibration multiplier --
the worst measured / estimated ratio seen on a headset, never below 1 --
widens every estimate automatically when estimates prove optimistic.

Fit outcomes:
  ``fits``       everything resident is under the target: nothing streams.
  ``residency``  the always-resident set fits: streaming keeps the rest under
                 the target (normal for big worlds, not a warning).
  ``over``       even the always-resident set does not fit: corrections run
                 (``plan_corrections``), least visible first.
"""

from __future__ import annotations

import json
import math
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Optional

MIB = 1024.0 * 1024.0

#: Bytes per triangle of compressed GPU geometry: 3 x 4 B indices plus
#: ~0.6 vertices per triangle at 24 B (static) / 32 B (skinned).
BYTES_PER_TRI_STATIC = 12.0 + 0.6 * 24.0
BYTES_PER_TRI_SKINNED = 12.0 + 0.6 * 32.0
#: GPU allocation granularity textures round up to (conservative).
TEXTURE_ALIGN = 4096
SWAPCHAIN_IMAGES = 3
EYE_W, EYE_H = 1680, 1760
STAGING_MB = 32.0
JOLT_TEMP_MB = 10.0
BYTES_PER_BODY = 1200.0
BYTES_PER_ENTITY = 2048.0
FRAGMENTATION_SHARE = 0.10
FRAGMENTATION_MIN_MB = 32.0


@dataclass
class TextureCost:
    key: str
    bytes: int
    importance: str = "standard"
    closest_view_m: float = 2.0
    size: int = 0


@dataclass
class MeshCost:
    key: str
    lod_bytes: list[int]
    importance: str = "standard"
    skinned: bool = False


@dataclass
class MemoryInputs:
    textures: list[TextureCost] = field(default_factory=list)
    meshes: list[MeshCost] = field(default_factory=list)
    static_geometry_bytes: int = 0
    animated_agents: int = 0
    joints_per_agent: int = 65
    clip_bytes: int = 0
    entities: int = 0
    bodies: int = 0
    largest_glb_bytes: int = 0
    render_scale_ceiling: float = 1.0
    frames_in_flight: int = 2
    max_fx_quads: int = 1024
    max_live_projectiles: int = 64
    max_decals: int = 48
    instances: int = 512


def _mb(b: float) -> float:
    return round(b / MIB, 2)


def _aligned(b: int) -> int:
    return int(math.ceil(b / TEXTURE_ALIGN) * TEXTURE_ALIGN)


def estimate(inp: MemoryInputs, *, baseline_mb: float = 260.0, target_mb: float = 4608.0,
             ceiling_mb: float = 5120.0, calibration: float = 1.0) -> dict[str, Any]:
    """Steady and peak PSS for the inputs, per category."""
    cal = max(1.0, float(calibration))
    tex = sum(_aligned(t.bytes) for t in inp.textures)
    mesh_floor = sum(m.lod_bytes[-1] for m in inp.meshes if m.lod_bytes)
    mesh_rest = sum(sum(m.lod_bytes[:-1]) for m in inp.meshes if m.lod_bytes)
    rt = (SWAPCHAIN_IMAGES * 2 * EYE_W * EYE_H * 4 * inp.render_scale_ceiling ** 2
          + 2 * 64 * 64 * 2 * SWAPCHAIN_IMAGES)  # + foveation maps; MSAA/depth lazy: 0
    palettes = inp.animated_agents * inp.joints_per_agent * 64 * inp.frames_in_flight
    per_frame = inp.frames_in_flight * (768 + inp.instances * 112 + inp.max_fx_quads * 6 * 36)
    pools = inp.max_live_projectiles * 256 + inp.max_decals * 256 + 1 * MIB  # + text atlas
    sim = JOLT_TEMP_MB * MIB + inp.bodies * BYTES_PER_BODY + inp.entities * BYTES_PER_ENTITY
    cpu_side = inp.clip_bytes + sim
    frag = max(FRAGMENTATION_MIN_MB * MIB, FRAGMENTATION_SHARE * cpu_side)
    transient = inp.largest_glb_bytes + STAGING_MB * MIB

    categories = {
        "engine_baseline": {"mb": round(baseline_mb, 2), "kind": "exact (measured)"},
        "textures": {"mb": _mb(tex), "kind": "exact" if inp.textures else "estimate"},
        "meshes_lowest_lod": {"mb": _mb(mesh_floor * cal), "kind": "estimate"},
        "meshes_higher_lods": {"mb": _mb(mesh_rest * cal), "kind": "estimate"},
        "static_batches": {"mb": _mb(inp.static_geometry_bytes * cal), "kind": "estimate"},
        "character_palettes": {"mb": _mb(palettes), "kind": "exact"},
        "character_clips": {"mb": _mb(inp.clip_bytes), "kind": "exact"},
        "render_targets": {"mb": _mb(rt), "kind": "exact"},
        "per_frame_buffers": {"mb": _mb(per_frame), "kind": "exact"},
        "fixed_pools": {"mb": _mb(pools), "kind": "exact"},
        "simulation": {"mb": _mb(sim * cal), "kind": "estimate"},
        "heap_fragmentation": {"mb": _mb(frag * cal), "kind": "estimate"},
        "audio": {"mb": 0.0, "kind": "reserved (no audio system)"},
    }
    always = (baseline_mb * MIB + tex + mesh_floor * cal + inp.static_geometry_bytes * cal
              + palettes + inp.clip_bytes + rt + per_frame + pools + sim * cal + frag * cal)
    streamable = mesh_rest * cal
    all_resident = always + streamable
    transient_mb = transient / MIB
    if (all_resident / MIB) <= target_mb and (all_resident + transient) / MIB <= ceiling_mb:
        outcome = "fits"
        steady = all_resident
    elif (always + transient) / MIB <= target_mb:
        outcome = "residency"
        steady = target_mb * MIB
    else:
        outcome = "over"
        steady = all_resident
    peak = steady + transient
    budget_mb = max(64.0, target_mb - always / MIB - transient_mb + tex / MIB)
    return {
        "categories": categories,
        "always_resident_mb": _mb(always),
        "streamable_mb": _mb(streamable),
        "load_transient_mb": round(transient_mb, 2),
        "steady_mb": _mb(steady),
        "peak_mb": _mb(peak),
        "target_mb": target_mb,
        "ceiling_mb": ceiling_mb,
        "calibration": cal,
        "outcome": outcome,
        # Streamable meshes + textures the runtime may keep resident.
        "resident_memory_budget_mb": round(budget_mb, 1),
        "gap_mb": round(max(0.0, (always + transient) / MIB - target_mb), 1)
        if outcome == "over" else 0.0,
    }


_STEPS = [128, 256, 512, 1024, 2048]


def _step_down(t: TextureCost, block_bytes_ratio: float = 0.25) -> bool:
    if t.size and t.size > 256:
        t.size //= 2
        t.bytes = int(t.bytes * block_bytes_ratio)
        return True
    return False


def plan_corrections(inp: MemoryInputs, *, baseline_mb: float, target_mb: float,
                     ceiling_mb: float, calibration: float = 1.0,
                     resolution_high_locked: bool = False,
                     texture_high_locked: bool = False) -> tuple[dict[str, Any], list[str]]:
    """Apply the least-visible-first corrections until the game fits.

    Order (each re-estimated; stop as soon as it fits):
      2. textures the player cannot get close to (background / closest view
         beyond 8 m): one size step down;
      4. lower LOD0 budgets for background assets (x0.6);
      5. a user's render scale ``high``: ceiling 1.25x -> 1.0x;
      6. only then hero, near and held textures one step down.
    Never: removing entities, NPCs, encounters or weapons, or replacing a
    texture with a solid colour. Returns (final estimate, compromises)."""
    notes: list[str] = []

    def est() -> dict[str, Any]:
        return estimate(inp, baseline_mb=baseline_mb, target_mb=target_mb,
                        ceiling_mb=ceiling_mb, calibration=calibration)

    e = est()
    if e["outcome"] != "over":
        return e, notes
    far = [t for t in inp.textures
           if t.importance in ("background", "projectile") or t.closest_view_m > 8.0]
    n = sum(1 for t in sorted(far, key=lambda t: -t.closest_view_m) if _step_down(t))
    if n:
        notes.append(f"{n} texture(s) the player cannot get close to: one size step lower")
        e = est()
        if e["outcome"] != "over":
            return e, notes
    bg = [m for m in inp.meshes if m.importance == "background"]
    for m in bg:
        m.lod_bytes = [int(b * 0.6) for b in m.lod_bytes]
    if bg:
        notes.append(f"{len(bg)} background asset(s): LOD0 budgets x0.6")
        e = est()
        if e["outcome"] != "over":
            return e, notes
    if resolution_high_locked and inp.render_scale_ceiling > 1.0:
        inp.render_scale_ceiling = 1.0
        notes.append("render scale high: swapchain ceiling 1.25x -> 1.0x to fit memory")
        e = est()
        if e["outcome"] != "over":
            return e, notes
    near = [t for t in inp.textures if t not in far]
    n = sum(1 for t in near if _step_down(t))
    if n:
        what = "texture quality high kept where possible; " if texture_high_locked else ""
        notes.append(f"{what}{n} near / held texture(s) one size step lower")
        e = est()
    return e, notes


def memory_status_without_device(e: dict[str, Any]) -> str:
    return "at_risk" if e.get("outcome") == "over" else "estimated_safe_not_device_tested"


# --- Inputs from what was built (scene_compile) ----------------------------

def _glb_json(path: Path) -> Optional[dict]:
    try:
        with path.open("rb") as f:
            head = f.read(20)
            if len(head) < 20 or head[:4] != b"glTF":
                return None
            n = int.from_bytes(head[12:16], "little")
            return json.loads(f.read(n))
    except (OSError, ValueError):
        return None


_COMP = {5120: 1, 5121: 1, 5122: 2, 5123: 2, 5125: 4, 5126: 4}
_NCOMP = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}


def clip_bytes_of(glb: Path) -> int:
    """Bytes of every animation sampler's keyframes (CPU-resident)."""
    doc = _glb_json(glb)
    if not doc:
        return 0
    acc = doc.get("accessors") or []
    total = 0
    for anim in doc.get("animations") or []:
        for s in anim.get("samplers") or []:
            for k in ("input", "output"):
                i = s.get(k)
                if isinstance(i, int) and i < len(acc):
                    a = acc[i]
                    total += int(a.get("count") or 0) * _COMP.get(a.get("componentType"), 4) \
                        * _NCOMP.get(a.get("type"), 1)
    return total


def inputs_from_scene(doc: dict, repo_root: Path, importance_of: Optional[dict] = None) -> MemoryInputs:
    """Exact-where-possible inputs from a compiled scene and its files."""
    from .texture_refs import glb_texture_refs

    perf = doc.get("performance") or {}
    inp = MemoryInputs(render_scale_ceiling=float(perf.get("render_scale_ceiling") or 1.0),
                       frames_in_flight=int(perf.get("frames_in_flight") or 2),
                       max_fx_quads=int(perf.get("max_fx_quads") or 1024),
                       max_live_projectiles=int(perf.get("max_live_projectiles") or 64),
                       max_decals=int(perf.get("max_decals") or 48))
    seen_tex: set[str] = set()
    seen_mesh: set[str] = set()
    seen_rig: set[str] = set()
    largest = 0
    agents = 0
    for e in doc.get("entities") or []:
        if not isinstance(e, dict):
            continue
        inp.entities += 1
        if isinstance(e.get("physics"), dict):
            inp.bodies += 1
        mesh = e.get("mesh")
        if not isinstance(mesh, dict):
            if e.get("primitive"):
                inp.static_geometry_bytes += int(12 * BYTES_PER_TRI_STATIC)
            elif isinstance(e.get("mesh_inline"), dict):
                inp.static_geometry_bytes += int(len(e["mesh_inline"].get("indices") or []) // 3
                                                 * BYTES_PER_TRI_STATIC)
            continue
        paths = list(mesh.get("lod_paths") or [mesh.get("glb_path")])
        key = paths[0]
        skinned = bool(mesh.get("skinned"))
        if skinned:
            agents += 1
        if key and key not in seen_mesh:
            seen_mesh.add(key)
            tris = [int(t) for t in mesh.get("lod_triangle_counts") or []]
            bpt = BYTES_PER_TRI_SKINNED if skinned else BYTES_PER_TRI_STATIC
            inp.meshes.append(MeshCost(key=key, lod_bytes=[int(t * bpt) for t in tris] or [0],
                                       importance=(importance_of or {}).get(
                                           str(mesh.get("asset_id")), "standard"),
                                       skinned=skinned))
            for p in paths:
                glb = repo_root / p
                try:
                    largest = max(largest, glb.stat().st_size)
                except OSError:
                    pass
                for ref in glb_texture_refs(glb) if glb.is_file() else []:
                    ktx = ref.get("ktx2")
                    f = (glb.parent / ktx) if ktx else None
                    tkey = str(f or ref.get("uri") or ref.get("hash"))
                    if tkey in seen_tex:
                        continue
                    seen_tex.add(tkey)
                    size = 0
                    if f is not None and f.exists():
                        size = f.stat().st_size
                    elif ref.get("uri"):
                        img = glb.parent / str(ref["uri"])
                        size = int(img.stat().st_size * 4 * 1.33) if img.exists() else 0
                    inp.textures.append(TextureCost(key=tkey, bytes=size))
            if skinned and key not in seen_rig:
                seen_rig.add(key)
                inp.clip_bytes += clip_bytes_of(repo_root / key)
    inp.largest_glb_bytes = largest
    from .scene_complexity import encounter_peak

    inp.animated_agents = agents + encounter_peak(doc)
    return inp


def load_calibration(path: Path) -> float:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
        return max(1.0, float(data.get("multiplier") or 1.0))
    except (OSError, ValueError):
        return 1.0


def update_calibration(path: Path, project_id: str, estimated_mb: float,
                       measured_mb: float) -> float:
    """Record a measured run; the multiplier is the worst ratio seen (>= 1)."""
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        data = {"runs": []}
    runs = [r for r in data.get("runs") or [] if r.get("project_id") != project_id]
    if estimated_mb > 0 and measured_mb > 0:
        runs.append({"project_id": project_id, "estimated_mb": round(estimated_mb, 1),
                     "measured_mb": round(measured_mb, 1),
                     "ratio": round(measured_mb / estimated_mb, 3)})
    mult = max([1.0] + [float(r.get("ratio") or 1.0) for r in runs])
    data = {"multiplier": round(mult, 3), "runs": runs}
    path.write_text(json.dumps(data, indent=2), encoding="utf-8")
    return mult
