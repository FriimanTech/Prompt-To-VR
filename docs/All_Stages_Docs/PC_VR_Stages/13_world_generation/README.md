# Stage 13: World Generation

> **Pipeline position:** after `code_generation` → before `asset_optimization`  
> **Target:** shared (PC VR and Quest)  
> **Primary code:** [`apps/orchestrator/ptv_orchestrator/pipeline/stage_world_generation.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/stage_world_generation.py)  
> **Primary artifacts:** `engine/generated/world/<project_id>/*`, `projects/<id>/world.scene.json`

## Main stage summary

### What this stage is

The **world generation stage** produces the VR **environment** — sky, ground, lights, room geometry, spawn point, and navigation regions — as C++ code implementing `IWorldBuilder`, compiled offline into `world.scene.json`. The environment is **not** a Tripo GLB.

### Why it exists

Level layout needs deterministic walkable space, nav graphs, and lighting that match gameplay. A single world-architect agent writes C++ constrained to a tight include whitelist, runs in the offline `ptv_worldgen` host (no Vulkan/XR linked), and emits JSON the scene compiler merges with gameplay entities.

### Inputs (what enters)

| Input | Location |
|-------|----------|
| BuildSpec excerpt | `build_spec.json` |
| Author guide | `engine/world_author_guide.md` |
| Object GLB map | `asset_normalization.json` + manifest (for `place_object`) |
| Staging manifest | `builds/worldgen_assets.json` |

### Outputs (what it produces)

| Output | Location |
|--------|----------|
| World builder C++ | `engine/generated/world/<project_id>/*.cpp` |
| World scene JSON | `world.scene.json` |
| Build logs | `logs/world.log`, `builds/worldgen/` |

### Where output goes next

**`scene_compile`** merges `world.scene.json` (environment layer) with gameplay `scene_spec` → final `scene.json`.

### Key concepts and terms

- **IWorldBuilder** — C++ interface world code implements. Methods add meshes, lights, spawn, `nav_region()`, `nav_link()`. Only `IWorldBuilder.h` + std + glm allowed — no ECS, no nlohmann in generated world code.
- **ptv_worldgen** — Offline host in `engine/worldgen/` that compiles and runs the builder, outputting JSON only.
- **nav_region / nav_link** — World author declares walkable areas and portals; deterministic validator checks reachability from spawn. See [`docs/navigation.md`](../../../navigation.md).
- **Compile-and-repair loop** — Agent writes C++ → build worldgen → run → validate scene → feed errors back, up to `world_max_repair_rounds`.
- **Fallback world** — If budget exhausted, emits `builtin.world.fallback` so the project still boots.

### End-to-end flow (plain language)

1. **WorldArchitect** agent (`gpt-5.5`, WebSearch enabled) writes C++ files.
2. Validate includes (`validate_world_includes`).
3. Build and run `ptv_worldgen` subprocess.
4. Validate output JSON (ground, spawn, navigation self-consistency).
5. On failure, repair in same session; else fallback world.

---

## Detailed stage deep dive

### Critical: world C++ is NOT resume-idempotent

Each run **deletes** `engine/generated/world/<project_id>/` entirely before agent starts. Stale `.cpp` files cannot accumulate.

### Execution: `run_with_architect`

1. Truncate `logs/world.log`
2. Wipe generated world dir for this project
3. `_load_object_assets()` from normalization + manifest → `builds/worldgen_assets.json` (for `place_object` in builder)
4. Loop `attempt 1..world_max_repair_rounds` (default 6):
   - `WorldArchitect.run(message)` — persistent session, WebSearch enabled
   - `_write_world_files()` — validate includes via `validate_world_includes()` (whitelist: IWorldBuilder.h + std + glm only)
   - `_build_worldgen()` — cmake target `ptv_worldgen` with `PTV_WORLD_PROJECT_ID`
   - `_run_worldgen()` subprocess → `world.scene.json`
   - `_validate_world_scene()` — ground, spawn, entity count cap
   - `_validate_navigation()` — regions, links, reachability from spawn
   - Success → record succeeded, return
5. On exhaustion: `_emit_fallback_world()` using `builtin.world.fallback` — deterministic bootable level
6. If fallback also fails → RuntimeError

### IWorldBuilder constraints

Generated world code:

- Implements `IWorldBuilder` API only
- Compiled offline in `engine/worldgen/` — **no** Vulkan, OpenXR, ECS, nlohmann linked
- Declares walkable space via `nav_region()` / `nav_link()` for runtime NavGraph

### WorldArchitect agent

OpenAI Agents SDK + WebSearch + SQLiteSession. Settings: `world_model` (gpt-5.5), high/xhigh reasoning, `world_agent_max_turns` (80).

Repair uses `build_repair_message()` feeding compile errors or validation issues back into same session.

### Artifacts

| Write | Purpose |
|-------|---------|
| `engine/generated/world/<pid>/*.cpp` | Builder source |
| `world.scene.json` | Environment layer for scene compile |
| `builds/worldgen/` | Offline build tree |
| `builds/worldgen_assets.json` | Prop id → glb path map |

Uses normalization GLBs, **not** LODs — scene_compile repoints world meshes later.

### SSE

`agent_turn`, `warn`, `bounds_reject`, `compile_error`, `run_error`, `invalid_scene`, `fallback`, `log`

See [`docs/world-generation.md`](../../../world-generation.md), [`docs/navigation.md`](../../../navigation.md).

### Include whitelist enforcement

validate_world_includes rejects any header outside IWorldBuilder.h + std + glm before cmake — prevents generated world code from touching ECS/renderer.

---

## Extend this folder

- `concepts-iworldbuilder.md` — API surface
- `concepts-navigation-declaration.md` — nav_region/link rules

## See also

- [`docs/world-generation.md`](../../../world-generation.md) — authoritative reference
- [`docs/navigation.md`](../../../navigation.md) — runtime nav + validation
- Stage 15 [`15_scene_compile`](../15_scene_compile/README.md)
