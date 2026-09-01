# Stage 14: Asset Optimization (LOD)

> **Pipeline position:** after `world_generation` → before `scene_compile`  
> **Target:** shared (PC VR and Quest)  
> **Primary code:** [`apps/orchestrator/ptv_orchestrator/pipeline/stage_asset_optimize.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/stage_asset_optimize.py)  
> **Primary artifacts:** updates `asset_normalization.json` with `<asset_id>_lod0..3.glb`

## Main stage summary

### What this stage is

The **asset optimization stage** decimates each GLB into a **four-level LOD chain** (`lod0`–`lod3`) using meshoptimizer, weighted by asset importance and the target device's triangle budget. Rigged characters use a **skin-preserving path** so animations survive simplification.

### Why it exists

Raw Tripo output is ~500k triangles per asset. Meta Quest 3 whole-frame budget is ~1.3–1.8M triangles. Without this stage, non-trivial games cannot hold frame rate. More assets means smaller per-asset budgets, not a hard asset count cap.

### Inputs (what enters)

| Input | Location |
|-------|----------|
| Normalization | `asset_normalization.json` |
| Manifest | `asset_manifest.json` |
| Scene specs | instance counts, interactivity |
| Target device | `ProjectState.target_device` → `PerformanceProfile` |

### Outputs (what it produces)

| Output | Location |
|--------|----------|
| LOD GLBs | `assets/<id>/<id>_lod0.glb` … `_lod3.glb` |
| Updated normalization | `lod_paths`, `triangle_counts`, `bounds_radius_m`, `skinned`, `__performance__` |

### Where output goes next

**`scene_compile`** embeds `lod_paths`, performance block, animation LOD distances, and agent budgets into final `scene.json`. Quest build packages LOD files only.

### Key concepts and terms

- **LOD (Level of Detail)** — Multiple mesh versions at decreasing triangle counts; runtime swaps by distance.
- **PerformanceProfile** — Derived tier + triangle/draw-call budgets from device and content volume.
- **ptv_glb_optimize** — C++ tool (meshoptimizer) run as subprocess per LOD level.
- **Skin-preserving path** — Rigged assets: simplify with attribute-aware meshoptimizer, copy skeleton verbatim to every LOD, never use `--sloppy` (invents vertices that break skinning).
- **Idempotent stamp** — `<asset_id>_lod.json` skips re-decimation on resume.

### End-to-end flow (plain language)

1. `derive_profile()` + `allocate_asset_budgets()` from scene + device.
2. For each asset, build LOD chain via `ptv_glb_optimize`.
3. Update normalization records in place.
4. Scene compile reads LOD paths for runtime culling.

---

## Detailed stage deep dive

### Preconditions

Requires `asset_normalization.json` on disk — else RuntimeError.

Runs after codegen so `ptv_glb_optimize` is an incremental CMake target (engine build tree warm).

### Budget derivation

1. Load all `scene_spec*.json` for instance counts / interactivity
2. `derive_profile(target_device, asset_count, mesh_entity_count)` → `PerformanceProfile` tier
3. `allocate_asset_budgets()` — importance-weighted triangle budget per asset (more assets → smaller per-asset share, quality floor enforced)

Quest 3 whole-frame budget ~1.3–1.8M tris; raw Tripo ~500k/asset.

### LOD chain (`_build_lod_chain`)

For each usable asset:

1. Check `<asset_id>_lod.json` stamp — skip if targets unchanged (idempotent)
2. Run `ptv_glb_optimize` subprocess for LOD0..LOD3
3. **Skinned path**: attribute-aware simplify, copy skeleton verbatim to every LOD, **never** `--sloppy` (breaks skin weights)
4. **Static path**: may use sloppy from LOD1 downward

Updates normalization in place:

- Repoint `glb_path` to LOD0
- Add `lod_paths[]`, `triangle_counts`, `bounds_radius_m`, `skinned`, `__performance__`

### Runtime performance block (forwarded by scene_compile)

Includes `max_animated_agents`, `animation_lod_distances_m`, `ai_tick_rates_hz` — CPU/skinning limits separate from triangle budget.

### SSE

`info` (tier, budgets), `log` (tool output), `succeeded`

### Failure modes

Missing normalization → RuntimeError. Tool build fail → RuntimeError. Per-asset optimize fail recorded; stage continues.

### Skin-preserving rule

Never --sloppy on skinned meshes — sloppy invents vertices without valid bone weights, breaking animation at runtime.

---

## Extend this folder

- `concepts-lod.md` — meshoptimizer options
- `artifacts.md` — performance block fields

## See also

- [`docs/rendering.md`](../../../rendering.md) — triangle budgets and runtime LOD
- [`docs/npc-and-ai.md`](../../../npc-and-ai.md) — animation/AI LOD bands
