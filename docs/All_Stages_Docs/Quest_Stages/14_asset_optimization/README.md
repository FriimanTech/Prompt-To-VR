> **Shared content:** Identical to [PC_VR_Stages/14_asset_optimization](../../PC_VR_Stages/14_asset_optimization/README.md). Update both copies when this stage changes.

# Stage 14: Asset Optimization (LOD, textures, memory fit)

> **Pipeline position:** after `code_generation` → before `scene_compile` (on Quest it is also re-run by the memory-correction loop after `quest_runtime_verify`)  
> **Target:** shared (PC VR and Quest)  
> **Primary code:** [`apps/orchestrator/ptv_orchestrator/pipeline/stage_asset_optimize.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/stage_asset_optimize.py), [`packages/schemas/ptv_schemas/performance.py`](../../../../packages/schemas/ptv_schemas/performance.py), [`pipeline/texture_policy.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/texture_policy.py), [`pipeline/memory_model.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/memory_model.py), [`engine/tools/glb_optimize.cpp`](../../../../engine/tools/glb_optimize.cpp), [`engine/tools/texture_compress.cpp`](../../../../engine/tools/texture_compress.cpp)  
> **Primary artifacts:** `assets/<id>/<id>_lod0..3.glb`, `assets/ptv_textures/*`, updated `asset_normalization.json` (per-asset LOD fields + `__performance__` + `__memory__`)

## Main stage summary

### What this stage is

The **asset optimization stage** fits every generated GLB into the device's budget. It derives a `PerformanceProfile` (tier + triangle / draw-call / texture / memory budgets) from the target device and the amount of content, applies the user's optimization locks, allocates a per-asset triangle budget weighted by importance, plans and compresses base-colour textures (KTX2 ASTC), fits the Quest memory target, and runs `ptv_glb_optimize` (meshoptimizer) to emit a **four-level LOD chain** per asset. Rigged characters take a skin-preserving path so skeletons and clips survive.

### Why it exists

Raw Tripo output is ~250k vertices / ~500k triangles per asset; Meta's whole-frame budget for Quest 3 is 1.3M–1.8M triangles. Without this stage a dozen assets overshoot the frame several times over (multi-second first-frame stalls, sub-target frame rates). There is **no cap on asset count**: more content means a smaller per-asset budget (never below a floor) plus more aggressive runtime culling.

### Inputs (what enters)

| Input | Location | Used for |
|-------|----------|----------|
| Normalization report | `asset_normalization.json` | Usable assets (`status` `ok` / `warn_no_material`), `glb_path` / `glb_source`, `source_triangles` |
| AssetManifest | `asset_manifest.json` | `importance`, `category`, `target_scale_meters`, `max_triangles` |
| Scene specs | `scene_spec.<id>.json` | Mesh-entity count (tier), instance counts, interactivity, positions (closest view) |
| Interaction design | `interaction_design.json` (optional) | Projectile assets (projectile triangle cap, texture class), held items |
| Asset contract | `asset_contract.json` | `part_regions.bowstring` triangles to drop (the engine draws the string) |
| World | `world.scene.json` | Declared nav regions (closest distance the player can reach) |
| Optimization preferences | `optimization_preferences.json` (via `load_preferences`) | User locks applied with `apply_overrides` |
| Memory calibration | `projects/memory_calibration.json` | Measured / estimated ratio from earlier headset runs |
| Target device | `ProjectState.target_device` | Profile derivation; memory fit is Quest only |

### Outputs (what it produces)

| Output | Location |
|--------|----------|
| LOD GLBs | `projects/<id>/assets/<id>/<asset_id>_lod0.glb` … `_lod3.glb` (beside the source) |
| LOD stamp | `<asset_id>_lod.json` (targets, source fingerprint, per-LOD metrics) |
| Texture map | `<asset_id>_textures.json` (image hash → shared PNG/JPEG + KTX2 URIs) |
| Shared textures | `projects/<id>/assets/ptv_textures/<hash>_<size>_astc<b>.ktx2`, `<hash>_<size>.{png,jpg}`, report JSON, `src/` extracts |
| Visual check | `assets/<id>/visual_check/lod{0,1}/` renders + `stamp.json` |
| Updated report | `asset_normalization.json`: `glb_path` → LOD0, `glb_source`, `lod_paths`, `triangle_counts`, `bounds_radius_m`, `lod0_longest_axis`, `triangle_budget`, `lod_errors_rel`, `decimated`, `skinned`, `joint_count`, `textures`, `optimization`; top-level `__performance__` (the full profile) and `__memory__` (memory plan) |

### Where output goes next

**`scene_compile`** reads `glb_path` (LOD0), `lod_paths`, `lod_triangle_counts`, `lod_errors_rel`, `bounds_radius_m`, `unit_scale` (from `lod0_longest_axis`), `skinned` and forwards `__performance__` as each scene's `performance` block; it also fails when a placed mesh references a missing texture file. **`quest_build`** packages the LODs and `assets/ptv_textures/` (no leading underscore: Android's aapt drops `_*` asset directories).

### Models, providers and cost

None. Deterministic Python plus local C++ tools (`ptv_glb_optimize`, `ptv_texture_compress`, `ptv_glb_views`). The tools are found under `engine/build/` (newest wins) or built once into `engine/build/vs2026-x64`.

### Failure behavior

- Missing `asset_normalization.json`, or `ptv_glb_optimize` cannot be built → stage fails.
- Per asset (recorded in `optimization.status`, stage continues): source GLB not found (`missing_source`, with a listing of GLBs present), tool error, a textured source whose LODs lost their texture, a failed visual comparison, a rigged source whose chain has no skin, or LODs that lost animation clips (`failed`).
- `ptv_texture_compress` unavailable → warning; textures ship as the original PNG/JPEG (slower to load, ~4× the GPU memory of ASTC).
- Memory plan `over` → warning; the APK is still built and flagged at risk.

---

## Detailed stage deep dive

### `run()` sequence

1. Load the report, manifest and state; collect usable assets.
2. `derive_profile(target_device, asset_count, mesh_entity_count)`; then `apply_overrides(profile, prefs.to_locks())` when the user set any optimization preference (each becomes a lock the runtime governor never moves).
3. Build an `AssetBudgetRequest` per asset (importance, scale, instance count, source triangles, `max_triangles`; projectiles capped at `projectile_max_triangles`) and `allocate_asset_budgets()`.
4. `_memory_fit()` (Quest only).
5. `_ensure_tool()` → `_plan_textures()`.
6. Per asset: `_decimation_source()` → `_build_lod_chain()` → texture-survival check → `_compare_lods()` → update the report.
7. Write `__performance__` and `__memory__`, save the report (keys sorted).

### Profile and tiers (`performance.py`)

`classify_tier`: `compact` (≤ 12 assets and ≤ 40 mesh entities), `standard` (≤ 24 / ≤ 100), `large` (≤ 250 mesh entities), else `massive`. Quest tier table:

| Tier | Frame tris | Resident tris | Draw calls | MSAA | FFR | Cull | Animated agents | Per-asset tris | Texture MB |
|------|-----------|---------------|-----------|------|-----|------|-----------------|----------------|-----------|
| compact | 900k | 1.8M | 250 | 4 | 0 | unbounded | 16 | 3 000–60 000 | 256 |
| standard | 1.1M | 2.4M | 300 | 4 | 1 | unbounded | 12 | 2 000–35 000 | 320 |
| large | 1.2M | 3.0M | 300 | 2 | 2 | 120 m | 8 | 1 500–18 000 | 384 |
| massive | 1.2M | 3.5M | 300 | 2 | 3 | 80 m | 6 | 1 500–10 000 | 448 |

Each tier also sets `lod_switch_distances_m`, `animation_lod_distances_m` and `ai_tick_rates_hz` (skinning and AI are CPU work, budgeted separately from triangles); `massive` enables LRU eviction. PC VR multiplies frame / resident / per-asset triangles by 5, draw calls by 4, animated agents by 3 and animation LOD distances by 2, doubles the texture budget, uses MSAA 4 with no foveation, raises projectile / FX pools and uses desktop memory targets. `lod_ratios` default `[1.0, 0.4, 0.15, 0.05]`.

### Budget allocation

`importance_weight` = class base (hero 4.0, standard 1.5, background 0.6) × `sqrt(target_scale_meters)` clamped to 0.25–4 × a reuse factor (+5 % per extra instance, up to 2×). Every asset first gets `min_triangles_per_asset`; the rest of `resident_triangle_budget` is shared by weight, capped by the importance ceiling (hero 100 %, standard 45 %, background 18 % of `max_triangles_per_asset`). An author `max_triangles` and the source triangle count are applied last (never upsample).

`_importance_for()` when the manifest gives none: characters and interactive assets (grabbable / interactive / held / weapon / tool tags or any mechanic binding) → hero; fx / ui → background; ≥ 4 m scenery → background; else standard.

### Textures (`_plan_textures`, `texture_policy.py`)

1. `ptv_glb_optimize --list-images --extract-images` lists every base-colour image of each source and extracts it to `ptv_textures/src/<hash>.<ext>`.
2. Each image gets a texture class: `projectile`, `ui`, `held` (has an interaction-design hold or is grabbable / held / weapon / tool / worn), or the importance. Shared images take the most demanding user.
3. Closest view: held 0.4 m, UI 0.6 m, characters 0.9 m, otherwise the distance from the nearest declared walkable region to each placement minus half the asset's size (minimum 0.3 m; 2 m when unplaced).
4. `choose_sizes` against `texture_max_size` (held / hero 2048, standard 1024, background 512, projectile 256, ui 1024) and `fit_budget` against `texture_memory_budget_mb` — over budget, the least visible shrink first, nothing is dropped; each reduction is reported.
5. `ptv_texture_compress --max-size --block --out-ktx2 --out-image --report` writes a KTX2 ASTC with mips (`texture_astc_block` 6×6 for 3D; 4×4 for UI or the user's "high" texture lock) plus a resized PNG/JPEG for PC VR. Outputs are reused while their report carries `TEXTURE_PIPELINE_VERSION` (1).
6. `<asset_id>_textures.json` is passed to every LOD (`--textures`), so a texture used by four LODs and two assets is stored once. A source with no base-colour texture is reported as `untextured_source`, not as an error; a textured material without UVs is a warning.

### LOD chain (`_build_lod_chain`)

- Targets = `budget × lod_ratios`. The stamp is reused when targets, the source fingerprint (path, size, mtime; plus the bowstring region and texture fingerprint), `lod0_longest_axis` and `textured_triangles` all match and every LOD exists.
- `_decimation_source()` always decimates the authored mesh: a `glb_source` / `glb_path` that is one of our own LODs is recovered to the primary GLB beside it (`pick_primary_glb(..., exclude_lods=True)`). Decimating LOD0 again would rebuild it from itself and, for a rigged avatar, permanently strip the skin.
- Per level: `ptv_glb_optimize --input --output --target-tris --report [--sloppy] [--drop-triangles] [--textures]`. `--sloppy` only from LOD2 and only for unskinned meshes (the tool refuses it on skinned meshes anyway: sloppy simplification invents vertex positions with no bone bindings).
- **One size for every LOD**: a sloppy LOD whose longest axis drifts more than 2 % (`LOD_EXTENT_TOLERANCE`) from LOD0 is rebuilt without `--sloppy`. `lod0_longest_axis` becomes `mesh.unit_scale` in `scene_compile`.
- Recorded per LOD: output and textured triangle counts, `simplify_error_rel` (→ `lod_errors_rel`, for projected-error LOD selection at runtime), longest axis; plus unit bounds radius, `skinned`, `joint_count`.
- Rig guards: a source with skins whose chain reports `skinned: false`, or any LOD whose clip count differs from the source's, fails the asset — otherwise a character would ship as a static statue with every stage reporting success.
- Skinned meshes are simplified with `meshopt_simplifyWithAttributes` + `meshopt_SimplifyRegularize`, bindings ride through the vertex remap, clips are copied verbatim and every LOD emits a byte-identical skeleton (see [`rendering.md`](../../../rendering.md)).

### Visual check (`_compare_lods`)

`ptv_glb_views --compare` renders the source and LOD0 / LOD1 from the same cameras (256 px). The asset fails when a textured source renders an untextured LOD, the mean CIE76 ΔE exceeds 6 (LOD0) / 10 (LOD1), white surface grows by more than 5 %, or more than 1 % magenta (missing texture) appears. Cached under the chain's fingerprint; skipped (reported) when the tool is not built.

### Memory fit (`_memory_fit`, Quest only)

Builds `MemoryInputs` (render scale, frames in flight, FX / projectile / decal pools, animated agents, entity / body estimates, a per-asset ASTC texture and skinned-mesh LOD cost) and calls `memory_model.plan_corrections` against the profile's `memory_target_mb` 4608 (4.5 GiB steady), `memory_ceiling_mb` 5120 (5.0 GiB peak) and the 260 MB engine baseline, with the measured calibration. When even the always-resident set would not fit it corrects **least-visible-first** — texture caps, background LOD0 budgets (×0.6, floor half the minimum), a user's high render scale — never removing content and respecting "high" resolution / texture locks. The result is emitted (`memory plan: steady … peak … -- fits/over`) and stored as `__memory__`. Meta's limit, 5.75 GiB PSS (`memory_limit_mb` 5888), is never planned against directly. PC VR records `not_applicable (PC VR)`.

On Quest, when `quest_runtime_verify` measures a memory failure it writes `memory_feedback.json` and updates `projects/memory_calibration.json`; the orchestrator then re-runs this stage, `scene_compile` and `quest_build` (at most `quest_memory_max_correction_rounds`, default 2, env `QUEST_MEMORY_MAX_CORRECTION_ROUNDS`) with the wider estimate.

### Settings

This stage has no settings of its own; it uses `cmake_generator`, `cmake_generator_platform`, `cmake_build_config` and `vcpkg_root` to build the tools, `projects_dir` for the calibration file, and the user's optimization preferences. Policy lives in `performance.py` (tier table, `lod_ratios`, budgets) and `texture_policy.py`.

### SSE events

`running`, `info` (tier and budgets, user overrides, texture totals and reductions, tool builds, untextured sources), `warning` (missing sources, failed assets, compressor missing, memory corrections / over), `log` (tool output, visual-check metrics), `succeeded` / `failed`. The success message is `tier=… optimized=… reused=… failed=… (src -> out tris)`.

### Quest notes

Everything above applies to both targets, but Quest is what the budgets are designed around: ASTC KTX2 textures are stored uncompressed in the APK so the engine can memory-map them, `scene_compile` warns for every Quest texture without a KTX2 twin, and the memory fit plus correction loop exist only for Quest. See [`quest-optimization.md`](../../../quest-optimization.md).

## See also

- [`docs/rendering.md`](../../../rendering.md) — budgets, LOD chain, skinned decimation, runtime LOD and culling
- [`docs/quest-optimization.md`](../../../quest-optimization.md) — user locks, memory model, correction loop
- [`docs/npc-and-ai.md`](../../../npc-and-ai.md) — animation / AI LOD bands
- Next stage: [`15_scene_compile`](../15_scene_compile/README.md)
