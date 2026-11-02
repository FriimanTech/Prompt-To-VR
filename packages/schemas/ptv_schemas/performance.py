"""PerformanceProfile: device-aware rendering budgets and quality tiers.

This module is the SINGLE SOURCE OF TRUTH for how a generated game is sized to
the target headset. The pipeline never hard-caps how many assets a prompt may
ask for; instead the budget allocator scales per-asset detail DOWN as content
volume goes UP (never below a quality floor), and the runtime spends the
remaining budget with culling + LOD + an adaptive governor.

Budget numbers come from Meta's published Quest guidance:
  * Quest 3 / 3S: 1.3M-1.8M triangles per frame.
  * Draw calls: 200-300 (busy simulation) .. 700-1000 (light simulation).
  * 72 Hz minimum, 90 Hz recommended.
See https://developers.meta.com/horizon/documentation/unity/unity-perf/

We deliberately target the LOW end of Meta's triangle range so the runtime
governor has headroom to recover when a scene turns out heavier than the
static estimate.
"""

from __future__ import annotations

from enum import Enum
from typing import List, Optional

from pydantic import Field

from .build_spec import TargetDevice
from .common import StrictModel


class PerformanceTier(str, Enum):
    """How heavy the generated content is, relative to the device.

    Derived deterministically from the target device plus the amount of GLB
    content in the project (see :func:`derive_profile`). The tier only changes
    DEFAULTS -- it never forbids content.
    """

    compact = "compact"
    standard = "standard"
    large = "large"
    massive = "massive"


class AssetImportance(str, Enum):
    """How much of the triangle budget an asset deserves.

    ``hero`` assets are the ones the player looks at up close (characters,
    held/interactive objects). ``background`` assets are set dressing seen at
    a distance. Derived from category/scale/interactivity when not authored.
    """

    hero = "hero"
    standard = "standard"
    background = "background"


class GovernorBounds(StrictModel):
    """Limits the runtime adaptive quality governor may move within.

    The governor escalates in this order when frame time exceeds budget:
    raise ``lod_bias`` -> shrink cull distance -> raise foveation level. It
    relaxes them again (in reverse) once there is headroom. These bounds keep
    it from destroying image quality on a scene that simply cannot hit target.
    """

    enabled: bool = True
    max_lod_bias: float = Field(
        default=2.0, ge=0.0, le=8.0,
        description="Upper bound on the global LOD bias the governor may add.",
    )
    min_cull_distance_m: float = Field(
        default=25.0, gt=0.0,
        description="Governor will never pull the cull distance below this.",
    )
    max_foveation_level: int = Field(
        default=3, ge=0, le=4,
        description="0=off, 1=low, 2=medium, 3=high, 4=top (Meta FFR levels).",
    )
    # Hysteresis: only react after the frame budget is missed/beaten for this
    # many consecutive samples, so the governor does not oscillate.
    escalate_after_bad_frames: int = Field(default=30, ge=1, le=600)
    relax_after_good_frames: int = Field(default=180, ge=1, le=3000)


class LockValue(StrictModel):
    """One user override ("Custom Quest Optimizations" or the main prompt).

    ``value`` "auto" means the engine decides. Anything else is a LOCK: the
    runtime governor never moves that knob, whatever the frame rate does."""

    value: str = "auto"
    level: Optional[int] = Field(default=None, ge=0, le=4)
    source: str = Field(default="", description="custom_box | main_prompt")
    quote: str = ""


class PerformanceLocks(StrictModel):
    ffr: LockValue = Field(default_factory=LockValue)
    resolution: LockValue = Field(default_factory=LockValue)
    lod: LockValue = Field(default_factory=LockValue)
    texture_quality: LockValue = Field(default_factory=LockValue)
    draw_distance: LockValue = Field(default_factory=LockValue)
    room_culling: LockValue = Field(default_factory=LockValue)
    anti_aliasing: LockValue = Field(default_factory=LockValue)
    frame_rate: LockValue = Field(default_factory=LockValue)

    def active(self) -> dict[str, LockValue]:
        return {k: v for k, v in self if isinstance(v, LockValue) and v.value != "auto"}


class PerformanceProfile(StrictModel):
    """Resolved per-project rendering budget, emitted into ``scene.json``.

    Consumed by the C++ runtime (``ScenePerformance`` in ``ptv/scene/Scene.h``)
    to configure MSAA, foveation, culling, LOD switching and the governor.
    """

    tier: PerformanceTier
    target_device: TargetDevice

    target_fps: int = Field(default=72, ge=60, le=120)
    frame_triangle_budget: int = Field(
        ..., gt=0,
        description="Triangles allowed to be VISIBLE in one frame.",
    )
    resident_triangle_budget: int = Field(
        ..., gt=0,
        description="Triangles allowed to be LOADED (GPU-resident) at once.",
    )
    draw_call_budget: int = Field(..., gt=0)

    msaa_samples: int = Field(default=4, ge=1, le=8)
    foveation_level: int = Field(
        default=0, ge=0, le=4,
        description="Initial fixed-foveated-rendering level (0=off .. 4=top).",
    )

    cull_distance_m: float = Field(
        ..., gt=0.0,
        description=(
            "Entities beyond this distance are skipped. Deliberately set"
            " effectively infinite for small scenes so a compact game still"
            " renders its whole environment."
        ),
    )
    lod_switch_distances_m: List[float] = Field(
        ...,
        description=(
            "Distance (metres) at which LOD1, LOD2, LOD3 take over. Length is"
            " one less than the LOD chain length."
        ),
    )

    # --- Animated agents -------------------------------------------------
    #
    # Skinning and AI are CPU work that competes with the render thread, and
    # Meta's own guidance puts a Quest 3 "busy simulation" -- their term for
    # an app with many animated NPCs -- at 200-300 draw calls and 1.3-1.8M
    # triangles per frame. Characters are therefore budgeted separately from
    # geometry: a scene can be well inside its triangle budget and still miss
    # frame rate because thirty skeletons are being posed every frame.
    max_animated_agents: int = Field(
        default=12,
        ge=0,
        le=200,
        description=(
            "Ceiling on simultaneously animated characters. The scene"
            " compiler clamps NPC spawn caps to this."
        ),
    )
    animation_lod_distances_m: List[float] = Field(
        default_factory=lambda: [8.0, 15.0, 40.0],
        description=(
            "[full_rate, half_rate, stop]. Characters nearer than the first"
            " are posed every frame, nearer than the second every other"
            " frame, then every fourth, and beyond the third not at all."
            " Clips still advance at the right speed -- only the sampling"
            " gets coarser, which is invisible at distance."
        ),
    )
    ai_tick_rates_hz: List[float] = Field(
        default_factory=lambda: [30.0, 10.0, 3.0],
        description=(
            "Behaviour update rate at the same three distance bands. An"
            " enemy across the arena does not need to re-decide 72 times a"
            " second."
        ),
    )

    # --- Weapons: projectiles and procedural FX ---------------------------
    #
    # A shot is cheap only if nothing about it allocates or multiplies: the
    # projectile pool, the FX quad ring and the flash lights are fixed-size,
    # and when one is full the OLDEST entry is recycled (a minigun never
    # grows memory or draw calls; its first tracers just disappear early).
    max_live_projectiles: int = Field(
        default=64, ge=1, le=4096,
        description="Projectile pool size; oldest recycled when full.")
    max_projectile_meshes: int = Field(
        default=24, ge=0, le=1024,
        description="Projectiles drawn with their own mesh at once (the rest as tracers).")
    max_fx_quads: int = Field(
        default=1024, ge=64, le=65536,
        description="Camera-facing FX quads per frame (flashes, tracers, sparks, beams).")
    max_beams: int = Field(default=4, ge=0, le=256)
    max_decals: int = Field(default=48, ge=0, le=4096)
    max_flash_lights: int = Field(
        default=2, ge=0, le=16,
        description="Short-lived point lights from muzzle flashes / explosions.")
    projectile_max_triangles: int = Field(
        default=300, ge=32, le=20000,
        description="LOD0 triangle cap for a projectile mesh (rocket, arrow).")

    # --- Textures ----------------------------------------------------------
    #
    # Base-colour textures are ASTC-compressed with mips for Quest (Meta:
    # ASTC, 6x6 as the default block, 4x4 for UI / text, mipmaps on every 3D
    # texture) and sized by the detail the player can actually see -- see
    # ``pipeline/texture_policy.py``. The budget covers every texture the game
    # can have resident at once; going over shrinks the least visible ones
    # first and reports each reduction, it never drops a texture.
    texture_memory_budget_mb: float = Field(
        default=320.0, gt=0.0, le=4096.0,
        description="GPU memory for all base-colour textures (ASTC + mips).")
    texture_max_size: dict[str, int] = Field(
        default_factory=lambda: {"held": 2048, "hero": 2048, "standard": 1024,
                                 "background": 512, "projectile": 256, "ui": 1024},
        description="Texture edge ceiling per importance class (pixels).")
    texture_astc_block: str = Field(
        default="6x6", description="ASTC block for 3D textures (UI uses 4x4).")

    # --- LOD and culling by projected size --------------------------------
    lod_screen_error_px: float = Field(
        default=1.0, gt=0.0, le=16.0,
        description="Switch to a coarser LOD once its simplification error "
                    "projects to fewer pixels than this.")
    lod_hysteresis: float = Field(default=0.2, ge=0.0, le=1.0)
    small_object_cull_px: float = Field(
        default=1.5, ge=0.0, le=16.0,
        description="Rendering-only cull for objects smaller than this on screen.")

    # --- Resolution and refresh -------------------------------------------
    render_scale_ceiling: float = Field(
        default=1.0, ge=0.5, le=1.25,
        description="Swapchain size as a fraction of the runtime-recommended "
                    "eye resolution (Quest 3: 1680x1760 at 1.0).")
    render_scale_floor: float = Field(
        default=0.85, ge=0.5, le=1.25,
        description="Lowest fraction dynamic resolution may submit.")
    refresh_hz: int = Field(default=72, ge=60, le=120)

    # --- Internal toggles (engineering switches, not user settings) -------
    multiview: bool = True
    ffr_fdm: bool = True
    frames_in_flight: int = Field(default=2, ge=1, le=2)
    static_batching: bool = True
    instancing: bool = True
    room_culling: bool = Field(
        default=False,
        description="Room/portal culling from nav regions; decided per scene "
                    "by scene_complexity (indoor, multi-room worlds).")

    # --- Memory (Quest 3 PSS: Meta's limit 5.75 GiB) ----------------------
    memory_limit_mb: float = Field(
        default=5888.0, description="Meta's documented per-app PSS limit.")
    memory_ceiling_mb: float = Field(
        default=5120.0, description="Engine: never planned past (load spikes included).")
    memory_target_mb: float = Field(
        default=4608.0, description="Engine: steady-state residency target.")
    engine_baseline_pss_mb: float = Field(
        default=260.0, description="Measured empty-scene PSS (code, driver, runtime).")
    resident_memory_budget_mb: float = Field(
        default=2048.0, gt=0.0,
        description="Streamable meshes + textures resident at once; set by the memory "
                    "model (target minus always-resident set and transients).")

    locks: PerformanceLocks = Field(default_factory=PerformanceLocks)

    min_triangles_per_asset: int = Field(default=1500, gt=0)
    max_triangles_per_asset: int = Field(default=60000, gt=0)
    lod_ratios: List[float] = Field(
        default_factory=lambda: [1.0, 0.4, 0.15, 0.05],
        description="Triangle ratio of each LOD relative to the LOD0 budget.",
    )

    evict_when_over_budget: bool = Field(
        default=False,
        description="Enable LRU mesh eviction once resident budget is hit.",
    )

    governor: GovernorBounds = Field(default_factory=GovernorBounds)


# ---------------------------------------------------------------------------
# Tier derivation + budget table (tune performance policy HERE).
# ---------------------------------------------------------------------------

# Effectively "no distance culling" -- larger than any sane playable world, so
# compact/standard scenes render everything the player can see.
UNBOUNDED_CULL_M = 100000.0

_QUEST_TIERS: dict[PerformanceTier, dict] = {
    PerformanceTier.compact: dict(
        frame_triangle_budget=900_000,
        resident_triangle_budget=1_800_000,
        draw_call_budget=250,
        msaa_samples=4,
        foveation_level=0,
        cull_distance_m=UNBOUNDED_CULL_M,
        lod_switch_distances_m=[18.0, 45.0, 90.0],
        max_animated_agents=16,
        animation_lod_distances_m=[10.0, 20.0, 50.0],
        ai_tick_rates_hz=[30.0, 12.0, 4.0],
        min_triangles_per_asset=3000,
        max_triangles_per_asset=60000,
        evict_when_over_budget=False,
        texture_memory_budget_mb=256.0,
    ),
    PerformanceTier.standard: dict(
        frame_triangle_budget=1_100_000,
        resident_triangle_budget=2_400_000,
        draw_call_budget=300,
        msaa_samples=4,
        foveation_level=1,
        cull_distance_m=UNBOUNDED_CULL_M,
        lod_switch_distances_m=[12.0, 30.0, 65.0],
        max_animated_agents=12,
        animation_lod_distances_m=[8.0, 15.0, 40.0],
        ai_tick_rates_hz=[30.0, 10.0, 3.0],
        min_triangles_per_asset=2000,
        max_triangles_per_asset=35000,
        evict_when_over_budget=False,
        texture_memory_budget_mb=320.0,
    ),
    PerformanceTier.large: dict(
        frame_triangle_budget=1_200_000,
        resident_triangle_budget=3_000_000,
        draw_call_budget=300,
        msaa_samples=2,
        foveation_level=2,
        cull_distance_m=120.0,
        lod_switch_distances_m=[8.0, 20.0, 45.0],
        max_animated_agents=8,
        animation_lod_distances_m=[6.0, 12.0, 30.0],
        ai_tick_rates_hz=[20.0, 8.0, 2.0],
        min_triangles_per_asset=1500,
        max_triangles_per_asset=18000,
        evict_when_over_budget=False,
        texture_memory_budget_mb=384.0,
    ),
    PerformanceTier.massive: dict(
        frame_triangle_budget=1_200_000,
        resident_triangle_budget=3_500_000,
        draw_call_budget=300,
        msaa_samples=2,
        foveation_level=3,
        cull_distance_m=80.0,
        lod_switch_distances_m=[6.0, 15.0, 32.0],
        max_animated_agents=6,
        animation_lod_distances_m=[5.0, 10.0, 24.0],
        ai_tick_rates_hz=[15.0, 6.0, 2.0],
        min_triangles_per_asset=1500,
        max_triangles_per_asset=10000,
        evict_when_over_budget=True,
        texture_memory_budget_mb=448.0,
    ),
}

# PC VR has an order of magnitude more headroom: scale the geometry budgets up
# and switch off the mobile-only quality compromises.
_PCVR_MULTIPLIER = 5.0


def classify_tier(asset_count: int, mesh_entity_count: int) -> PerformanceTier:
    """Deterministically pick a tier from how much GLB content exists.

    ``asset_count`` is the number of distinct GLB assets (unique meshes) and
    ``mesh_entity_count`` is how many scene entities actually reference one
    (the same asset placed 50 times costs 50 draw calls but only one mesh).
    The heavier of the two signals wins.
    """

    if asset_count <= 12 and mesh_entity_count <= 40:
        return PerformanceTier.compact
    if asset_count <= 24 and mesh_entity_count <= 100:
        return PerformanceTier.standard
    if mesh_entity_count <= 250:
        return PerformanceTier.large
    return PerformanceTier.massive


def derive_profile(
    *,
    target_device: TargetDevice,
    asset_count: int,
    mesh_entity_count: int,
    target_fps: int = 72,
    tier_override: Optional[PerformanceTier] = None,
) -> PerformanceProfile:
    """Resolve the full budget for a project.

    This is intentionally pure + deterministic so the same project always
    compiles to the same budget (and so it is unit-testable).
    """

    tier = tier_override or classify_tier(asset_count, mesh_entity_count)
    base = dict(_QUEST_TIERS[tier])

    if target_device == TargetDevice.pcvr:
        base["frame_triangle_budget"] = int(
            base["frame_triangle_budget"] * _PCVR_MULTIPLIER
        )
        base["resident_triangle_budget"] = int(
            base["resident_triangle_budget"] * _PCVR_MULTIPLIER
        )
        base["draw_call_budget"] = int(base["draw_call_budget"] * 4)
        base["max_triangles_per_asset"] = int(
            base["max_triangles_per_asset"] * _PCVR_MULTIPLIER
        )
        # Desktop GPUs do not need the mobile compromises.
        base["msaa_samples"] = 4
        base["foveation_level"] = 0
        base["evict_when_over_budget"] = False
        base["texture_memory_budget_mb"] = base["texture_memory_budget_mb"] * 2.0
        # A PC has CPU headroom the headset does not, and skinning plus AI is
        # CPU work, so more characters can be alive and updated further out.
        base["max_animated_agents"] = int(base["max_animated_agents"] * 3)
        base["animation_lod_distances_m"] = [
            d * 2.0 for d in base["animation_lod_distances_m"]
        ]
        base.update(max_live_projectiles=256, max_projectile_meshes=96,
                    max_fx_quads=4096, max_beams=16, max_decals=192,
                    max_flash_lights=4, projectile_max_triangles=1500)

    governor = GovernorBounds(
        # Never let the governor cull closer than the first LOD switch.
        min_cull_distance_m=max(25.0, base["lod_switch_distances_m"][0] * 2.0),
        max_foveation_level=0 if target_device == TargetDevice.pcvr else 3,
        enabled=True,
    )

    if target_device == TargetDevice.pcvr:
        # Desktop memory is not the Quest's PSS budget.
        base["memory_target_mb"] = 12288.0
        base["memory_ceiling_mb"] = 14336.0
        base["memory_limit_mb"] = 16384.0
        base["ffr_fdm"] = False
    return PerformanceProfile(
        tier=tier,
        target_device=target_device,
        target_fps=target_fps,
        refresh_hz=target_fps,
        governor=governor,
        **base,
    )


_TEXTURE_STEPS = [256, 512, 1024, 2048]


def _step(size: int, delta: int) -> int:
    if size not in _TEXTURE_STEPS:
        return size
    i = max(0, min(len(_TEXTURE_STEPS) - 1, _TEXTURE_STEPS.index(size) + delta))
    return _TEXTURE_STEPS[i]


def apply_overrides(profile: PerformanceProfile, locks: PerformanceLocks) -> tuple[
        PerformanceProfile, list[str]]:
    """Apply user overrides on top of the automatic profile.

    Each override both CHANGES the starting value and LOCKS the knob, so the
    runtime governor never moves it back. Anything the user did not mention
    stays automatic. Returns the new profile and human-readable notes of what
    was applied (and clamped -- memory safety is never overridable; see the
    Phase 13 memory model, which may clamp further)."""
    p = profile.model_copy(deep=True)
    p.locks = locks
    notes: list[str] = []
    g = p.governor
    ffr = locks.ffr
    if ffr.value == "off":
        p.foveation_level = 0
        g.max_foveation_level = 0
        p.ffr_fdm = False
        notes.append("FFR off: no foveation at any time")
    elif ffr.value == "fixed":
        lvl = max(1, min(3, ffr.level or 2))
        p.foveation_level = lvl
        g.max_foveation_level = lvl
        notes.append(f"FFR fixed at level {lvl}")
    elif ffr.value == "max":
        lvl = max(0, min(3, ffr.level if ffr.level is not None else 1))
        g.max_foveation_level = lvl
        p.foveation_level = min(p.foveation_level, lvl)
        notes.append(f"FFR dynamic, never above level {lvl}")
    res = locks.resolution
    if res.value == "off":
        p.render_scale_floor = 1.0
        p.render_scale_ceiling = 1.0
        notes.append("dynamic resolution off: always the native eye resolution")
    elif res.value == "high":
        p.render_scale_ceiling = 1.25
        p.render_scale_floor = 1.25
        notes.append("resolution high: 1.25x the recommended eye resolution, never reduced")
    lod = locks.lod
    if lod.value == "detail":
        p.lod_screen_error_px = 0.5
        g.max_lod_bias = 0.0
        p.max_triangles_per_asset = int(p.max_triangles_per_asset * 1.5)
        notes.append("model detail prioritised: finer LOD threshold, governor may not coarsen LODs")
    elif lod.value == "performance":
        p.lod_screen_error_px = 2.0
        g.max_lod_bias = max(g.max_lod_bias, 3.0)
        notes.append("model detail: performance (coarser LODs sooner)")
    tex = locks.texture_quality
    if tex.value == "high":
        p.texture_max_size = {k: _step(v, +1) for k, v in p.texture_max_size.items()}
        p.texture_memory_budget_mb = p.texture_memory_budget_mb * 2.0
        p.texture_astc_block = "4x4"
        notes.append("texture quality high: one size step up, ASTC 4x4")
    elif tex.value == "memory_saver":
        p.texture_max_size = {k: _step(v, -1) for k, v in p.texture_max_size.items()}
        p.texture_memory_budget_mb = p.texture_memory_budget_mb * 0.6
        notes.append("texture quality: memory saver (one size step down)")
    dist = locks.draw_distance
    if dist.value == "far":
        g.min_cull_distance_m = max(g.min_cull_distance_m, p.cull_distance_m)
        p.small_object_cull_px = 0.5
        notes.append("draw distance far: the governor never shortens it")
    elif dist.value == "performance":
        p.cull_distance_m = min(p.cull_distance_m, 120.0)
        g.min_cull_distance_m = 20.0
        notes.append("draw distance: performance (may be shortened more)")
    if locks.room_culling.value == "off":
        p.room_culling = False
        notes.append("room/portal culling off")
    if locks.anti_aliasing.value == "high":
        p.msaa_samples = 4
        notes.append("anti-aliasing high: 4x MSAA always")
    fr = locks.frame_rate
    if fr.value in ("90", "120"):
        p.target_fps = int(fr.value)
        p.refresh_hz = int(fr.value)
        notes.append(f"frame rate {fr.value} Hz (frame budget {1000.0 / int(fr.value):.1f} ms)")
    return p, notes


class AssetBudgetRequest(StrictModel):
    """What the allocator needs to know about one asset."""

    importance: AssetImportance = AssetImportance.standard
    target_scale_meters: float = 1.0
    instance_count: int = Field(default=1, ge=0)
    source_triangles: Optional[int] = Field(
        default=None, ge=0,
        description="Triangle count of the source GLB; never upsample past it.",
    )
    max_triangles: Optional[int] = Field(
        default=None, ge=1,
        description="Explicit author override from AssetItem.max_triangles.",
    )


# Fraction of ``max_triangles_per_asset`` each importance class may reach.
# Importance must matter even when there is budget to spare: a distant piece
# of set dressing should not consume hero-level triangles (and load time)
# just because the project is small.
_IMPORTANCE_CEILING: dict[AssetImportance, float] = {
    AssetImportance.hero: 1.0,
    AssetImportance.standard: 0.45,
    AssetImportance.background: 0.18,
}


def importance_weight(
    *,
    importance: AssetImportance,
    target_scale_meters: float,
    instance_count: int = 1,
) -> float:
    """Relative share of the triangle budget an asset should receive.

    Bigger objects cover more pixels and deserve more detail; hero assets are
    inspected up close. Assets reused many times are shared (one mesh, many
    draws), so their per-mesh detail is slightly boosted -- the cost is
    amortised across every placement.
    """

    base = {
        AssetImportance.hero: 4.0,
        AssetImportance.standard: 1.5,
        AssetImportance.background: 0.6,
    }[importance]
    # Scale contributes sub-linearly: a 4 m object is not 4x as detailed.
    scale_factor = max(0.25, min(4.0, float(target_scale_meters) ** 0.5))
    reuse_factor = 1.0 + min(1.0, max(0, instance_count - 1) * 0.05)
    return base * scale_factor * reuse_factor


def asset_triangle_ceiling(
    profile: PerformanceProfile, importance: AssetImportance
) -> int:
    """Upper triangle bound for one asset of the given importance class."""
    return max(
        profile.min_triangles_per_asset,
        int(profile.max_triangles_per_asset * _IMPORTANCE_CEILING[importance]),
    )


def allocate_asset_budgets(
    profile: PerformanceProfile,
    requests: dict[str, AssetBudgetRequest],
) -> dict[str, int]:
    """Split the resident triangle budget across assets.

    Guarantees, in priority order:
      1. Never exceed the source mesh (decimation only, never upsampling).
      2. Never exceed the importance-scaled ceiling.
      3. Never drop below ``min_triangles_per_asset`` -- so a project with a
         very large asset count degrades gracefully instead of turning small
         props into unrecognisable blobs.

    Within those bounds the remaining pool is shared proportionally to
    :func:`importance_weight`, so detail shifts toward the objects the player
    actually looks at as the project grows.

    Returns asset_id -> LOD0 triangle budget.
    """

    if not requests:
        return {}

    weights = {
        asset_id: importance_weight(
            importance=req.importance,
            target_scale_meters=req.target_scale_meters,
            instance_count=req.instance_count,
        )
        for asset_id, req in requests.items()
    }
    total_weight = sum(weights.values()) or 1.0

    # Reserve the floor for everyone first, then distribute what is left by
    # weight. This keeps the allocation stable as asset count grows.
    floor = profile.min_triangles_per_asset
    pool = max(profile.resident_triangle_budget - floor * len(requests), 0)

    out: dict[str, int] = {}
    for asset_id, req in requests.items():
        ceiling = asset_triangle_ceiling(profile, req.importance)
        share = (weights[asset_id] / total_weight) * pool
        budget = int(max(floor, min(float(ceiling), floor + share)))

        # Hard caps are applied LAST so they win over the quality floor: an
        # explicit author cap and the source mesh size are both intentional
        # limits, whereas the floor only guards against allocator starvation.
        if req.max_triangles is not None:
            budget = min(budget, req.max_triangles)
        if req.source_triangles is not None:
            # Decimation only: a 500-triangle prop stays 500 triangles.
            budget = min(budget, req.source_triangles)
        out[asset_id] = max(budget, 0)
    return out
