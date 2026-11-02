# Quest optimization and memory safety

Every generated Quest game is optimized automatically, measured on the
headset when one is connected, and protected against running out of memory.
A user can override a defined set of quality / performance settings; nothing
else changes, and memory safety is never overridable.

Rendering details (textures, multiview, FFR, culling, LOD, batching, the
governor) are in [rendering.md](rendering.md); the verification passes in
[quest-build.md](quest-build.md#on-headset-verification).

## Build time versus runtime

**Build time** (orchestrator, deterministic):

- `asset_optimization` — LOD chains with measured simplification errors,
  texture sizes from the closest viewing distance, ASTC KTX2, the
  `PerformanceProfile` (tier defaults, then the user's locks), the planned
  memory fit and its corrections.
- `scene_compile` → `scene_complexity` — counts, visibility sampling from the
  walkable regions (eye height, 8 headings, the Quest 3 field of view, LOD
  switch and cull distances) giving p50 / p95 visible triangles and draws, a
  coarse frame-cost estimate, closed-room detection, static-batch flags, the
  exact memory estimate from the built files, and the per-scene starting
  settings. Written to `scene_complexity.json` and each scene's
  `performance` block.

**Runtime** (engine, every APK):

- exact two-eye frustum, room, distance and small-object culling;
- projected-error LOD with hysteresis and speed-aware prefetch;
- merged static batches and instancing;
- `QualityGovernor`: FFR level, render resolution, LOD bias, draw distance;
- `MemoryGuard`: residency, eviction, heap purges.

## Applied automatically to every game

| Kind | What |
|---|---|
| Never lowers visible quality | instancing, static batching, texture dedupe by hash, ASTC with mips, two frames in flight, exact two-eye culling, room culling where the world has closed rooms, residency streaming with an always-resident lowest LOD |
| Chosen per scene from measurements | multiview (on only above 120 estimated p95 draws: it measured ~10% more GPU time on both test games at 17-64 draws, CPU unchanged); initial FFR level 1, or 2 when the scene's estimate is heavy (the density map costs ~1 ms at level 0, level 1 removed ~40% of GPU time on Glock Laser Deck) |
| Trades quality only when measured frames require it | the governor's ladders: FFR, render scale (floor 0.85), LOD bias, draw distance |

Per-game starting values come from `scene_complexity`, not game size alone: a
large world whose visible cost is low starts exactly like a small game. Once a
headset has run the game, the next `scene_compile` uses the measured GPU / CPU
time from `runtime_verification.json` instead of the estimate.

## What users can override

The New Project page (Quest target) has a collapsed **Custom Quest
Optimizations** box, empty by default. The same wording in the main prompt
counts too; **the box wins** when they disagree. The
`optimization_preferences` stage (right after `build_spec`) maps the text
onto these settings:

| Setting | Values | Effect |
|---|---|---|
| FFR (fixed foveated rendering) | `auto`, `off`, `fixed:low/medium/high`, `max:low/medium/high` | `off` = level 0 always and no density map; `fixed` pins a level; `max` caps the dynamic ladder |
| Dynamic resolution | `auto`, `off`, `high` | `off` = always native (1680x1760 per eye); `high` = 1.25x, never reduced |
| Model detail (LOD) | `auto`, `detail`, `performance` | `detail` = 0.5 px error threshold, LOD bias locked at 0, 1.5x LOD0 ceilings |
| Texture quality | `auto`, `high`, `memory_saver` | `high` = one size step up, ASTC 4x4, budget x2 within memory safety |
| Draw distance | `auto`, `far`, `performance` | `far` = the governor never shortens it, small-object cull at 0.5 px |
| Room culling | `auto`, `off` | |
| Anti-aliasing | `auto`, `high` | 4x MSAA always (a rebuild setting) |
| Frame rate | `auto` (72 Hz), `90`, `120` | budget 11.1 / 8.3 ms, through `XR_FB_display_refresh_rate` |

Not user-controllable, because they save work without lowering visible
quality: multiview, instancing, batching, dedupe, frames in flight, ASTC, mips,
frustum culling and residency. "Disable texture compression" is reported as
unsupported (memory safety needs it; the closest setting, texture quality
`high`, is suggested). Eye-tracked foveation is unsupported (no eye tracking on
Quest 3; FFR is offered).

**How the stage works.** A keyword gate (foveation, resolution, LOD / detail,
texture, draw distance, culling, fps / Hz, anti-aliasing, graphics or visual
quality, performance) decides whether to call a model at all; no match means
empty preferences and no call. Otherwise one light-tier structured-output
call maps the wording, and a deterministic validator keeps only settings whose
**quote appears verbatim** in its source text, applies box-wins, clamps
requests past the hard limits and records unsupported ones (eye tracking, no
compression and turning memory protection off are detected without the model
as well). The BuildSpec prompt is told to ignore optimization wording, so it
never shapes gameplay. Output: `optimization_preferences.json`
(`OptimizationPreferences`).

**Locks.** `asset_optimization` applies the preferences on top of the
automatic profile (`apply_overrides`) and every override becomes a lock in
`performance.locks` with the value, source and quote. `SceneLoader` reads the
locks and `QualityGovernor` builds its ladders only from unlocked knobs; when
only locked knobs remain it logs `governor: over budget, blocked by user
settings`. Verification checks the locks held on the device and reports a
user-caused frame miss as a **warning**, never a failure.

**Warnings never block.** The New Project page previews the parsed overrides
(`POST /api/optimization-preferences/preview`, the same function and cache as
the stage) and, when anything differs from automatic, shows the overrides,
unsupported requests and clamps above: "Warning: Your custom Quest
optimization settings differ from the VR engine's recommended settings.
Disabling or modifying certain optimizations may reduce performance, cause
unstable frame rates, or prevent your game from running smoothly on the Quest
3." Create stays enabled. The project page shows the resolved overrides with
the scene's estimated p95 GPU time, and the measured result after verify.

### Hard limits (clamped and reported, never overridable)

- memory: the 4.5 GiB steady-state target and the 5.0 GiB peak ceiling, both
  under Meta's 5.75 GiB; MemoryGuard always on;
- texture size at most 2048; render scale at most 1.25 and `maxImageRect`;
  MSAA at most 4x; refresh rate at most what the runtime reports (120 Hz).

## Memory safety

### Meta's limit versus the engine's targets

- **Meta's limit:** an immersive Quest 3 / 3S app may use **5.75 GiB PSS**
  (CPU and GPU share it). An app over it can survive a quiet session and still
  be killed under pressure (`lowmemorykiller`).
- **Engine targets** (engineering choices, `PerformanceProfile`):
  **4.5 GiB steady state** (about 78% of the limit) and a **5.0 GiB peak
  ceiling** (87%) nothing the engine plans may cross. Lower would waste about
  1 GiB a large game can use for detail; higher would eat the margin for
  estimate error and OS spikes.
- PC VR is unaffected (desktop GPUs have their own memory).

### The memory model (`pipeline/memory_model.py`)

Each category is labelled exact or estimate: engine baseline (measured),
textures (KTX2 bytes rounded to 4 KB), meshes per LOD (24 / 32 B vertices,
lowest LOD always resident, the rest streamable), static batches, character
palettes and clip data (one copy per rig), render targets (swapchain images x
2 layers x render scale; MSAA / depth are lazily allocated and cost nothing),
per-frame buffers x frames in flight, fixed pools, simulation, load
transients (largest GLB + the 32 MB staging buffer), a heap fragmentation
allowance and a reserved audio slot. Outputs: always-resident set, streamable
set, steady and peak estimates, and `resident_memory_budget_mb` for the
runtime.

**Calibration:** every verified run records measured versus estimated peak in
`projects/memory_calibration.json`; estimates are multiplied by the worst
ratio seen (never below 1), so an optimistic model widens its own margin.

### Fit decision and automatic corrections

- **Fits:** everything resident is under the target — nothing streams.
- **Residency:** only the always-resident set fits — streaming keeps the rest
  under the target. Normal for large worlds, not a warning.
- **Over:** corrections run least-visible-first, re-estimating after each:
  textures the player cannot get close to one size step down; background LOD0
  budgets x0.6; a user's render scale `high` back to 1.0x; only then near and
  held textures. Never: removing entities, NPCs, encounters or weapons, or a
  solid colour instead of a texture. If it still does not fit, the APK is
  still built and flagged `at_risk` with the contributors and the gap.
  Compromises are listed in `asset_normalization.json` (`__memory__`) and on
  the project page.

### The load path

- `glb`, `ktx2` and `spv` are stored uncompressed and memory-mapped from the
  APK (`AAsset_getBuffer`); KTX2 levels upload straight from the mapping.
- One 32 MB staging buffer; textures of 4 MB or more get dedicated
  allocations; MSAA colour and depth are lazily allocated transient
  attachments.
- Skeleton clip data is shared (copy-on-write `ClipList`) by every entity
  loaded with the same rig, scale and model frame (the loader bakes the frame
  pivot into the root joint's channels, so only identical loads may share)
  and by every runtime clone.
- `mallopt(M_PURGE_ALL)` returns freed heap pages after a scene load, a scene
  reload and a red-level eviction.

### `MemoryGuard` (runtime, always on)

Samples `/proc/self/smaps_rollup` PSS, the KGSL driver's per-process unmapped
GPU memory (`/sys/class/kgsl/kgsl/proc/<pid>/gpumem_unmapped`; the engine's
own GPU accounting stands in when unreadable) and `MemAvailable` once a
second on a background thread, adds queued loads, and acts on the
**projection**:

| Level | Projected | Action |
|---|---|---|
| green | < 4.25 GiB | normal |
| yellow | 4.25-4.5 GiB | no speculative prefetch; evict long-unused streamable meshes |
| red | 4.5-4.8 GiB, or `MemAvailable` < 512 MB, or `APP_CMD_LOW_MEMORY` | evict every invisible mesh above the floor, defer high-detail loads, purge the heap, log the top consumers |
| critical | >= 4.8 GiB | also draw visible meshes at their coarsest resident LOD |

Levels fall back only 128 MB under their threshold. A failed GPU buffer
allocation is recoverable: the renderer waits for the GPU, frees retired and
invisible streamable meshes (never the floor), purges the heap and retries
once; if that fails too the mesh stays at its lower LOD and the failure is
logged, never an abort. The guard reacts to
memory only (frame time is the governor's), and gameplay — entities,
colliders, navigation, AI, game state — is never freed. The 1 Hz `mem:` line
reports it. Not implemented: dropping a texture's top mips under pressure
(textures are shared across the LOD chain and stay resident at full size;
measured texture memory in the test games is a few MB).

### Verification, status and the APK download

The headset memory pass (peak, steady state, kill scan, restart leaks,
soak, pressure) is described in
[quest-build.md](quest-build.md#on-headset-verification). Its result is
`memory_status`, in `runtime_verification.json` and `memory_status.json`:

| Status | Meaning |
|---|---|
| `verified_safe` | measured on a connected Quest 3 and every memory check passed |
| `estimated_safe_not_device_tested` | the estimate fits, no headset ran it — never shown as verified |
| `at_risk` | the estimate is still over the target after corrections, no headset |
| `failed_on_device` | a measured failure remains after the correction rounds |

Overall verification passes only with every check green, memory included; a
memory failure is listed first. **The APK is always downloadable**
(`GET /projects/{id}/apk`, the Download APK button on the project page), with
the memory badge beside it. Quest system warnings are never suppressed.

## Measured results (Quest 3, 1 November 2026)

Before = the engine before this work (white assets, one pass per eye, no
density map); after = automatic settings. GPU from timestamps, PSS from
`dumpsys meminfo`; all at 72 fps.

| Game | GPU ms before → after | CPU ms | Draw calls | PSS MB | Memory status |
|---|---|---|---|---|---|
| Glock Laser Deck | 9.3 → 5.6 (p95 6.1) | 0.9 → 1.2 | 386 → 34 | 261 → 200 | verified_safe |
| Navy SEAL Raid | 6.0 → 2.8 (p95 3.7) | 1.9 → 1.8 | 426 → 22 | 273 → 204 | verified_safe |
| Fun Goblin Shooter | (no baseline) 3.6 (p95 5.6) | 1.4 | 20 | 202 | verified_safe |
| Navy SEAL Raid, max quality¹ | 3.2 (p95 4.6) | 1.5 | 22 | 248 | verified_safe |

¹ "no FFR, no resolution reduction, high textures, detailed models far away":
FFR level 0 and render scale 1.0 held in every `perf:` line; ASTC 4x4 at
2048 px; 0.5 px LOD threshold.

A/B on Glock Laser Deck (GPU ms): multiview + density map 10.2 at FFR 0;
without the density map 8.9; without multiview 9.0; FFR 1 / 2 / 3: 5.95 /
5.4 / 4.6. Navy SEAL Raid: multiview 4.1 vs 3.7 without (CPU 1.8 both).

Static batching on top of instancing did not reduce draw calls where the
merged geometry was already instanced primitives (Glock Laser Deck: 11 → 34
draws, inside the 300 budget, GPU and CPU unchanged); it helps scenes with
many distinct inline meshes. Peak PSS stayed near 200 MB for every test
game, far under the 4.5 GiB target; the memory model's estimates (383-427 MB)
were conservative by about 2x.

## Honest limits

- No configuration guarantees 72 Hz for every prompt; the engine measures,
  adapts and reports the bottleneck.
- Horizon OS decides memory kills; the engine minimises them by budgeting
  under Meta's limit, acting before spikes and testing on the device.
- Breachers, Tactical Assault VR and Ghosts of Tabor also rely on baked
  lighting and normal / metal-roughness materials, which this engine does not
  have yet.
