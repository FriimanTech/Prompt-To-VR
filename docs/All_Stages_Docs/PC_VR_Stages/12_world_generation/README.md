# Stage 12: World Generation

> **Pipeline position:** after `asset_contract` → before `code_generation`  
> **Target:** shared (PC VR and Quest)  
> **Primary code:** [`apps/orchestrator/ptv_orchestrator/pipeline/stage_world_generation.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/stage_world_generation.py), [`pipeline/world_agent.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/world_agent.py), [`engine/worldgen/`](../../../../engine/worldgen/)  
> **Primary artifacts:** `engine/generated/world/<project_id>/[<scene_id>/]*.{cpp,h,hpp}`, `projects/<id>/world.scene.json` (one environment) or `projects/<id>/world.<scene_id>.scene.json` (several), `projects/<id>/logs/world.log`

## Main stage summary

### What this stage is

The **world generation stage** produces the VR **environment** of every scene that owns one — sky, ground, lights, structural geometry, the player spawn, declared navigation, interactive objects, portals, named spawn points and NPC entrances / posts — as C++ implementing `IWorldBuilder`. Each environment is written by a **world-architect agent**, compiled and run **offline** in the `ptv_worldgen` host, and emitted as JSON. The environment is **not** a Tripo GLB.

### Why it exists

Level layout needs exact, deterministic walkable space and lighting that match gameplay. Authoring it in bounded C++ (only `ptv/api/IWorldBuilder.h` + the C++ standard library + `<glm/...>`) lets an agent build procedural geometry while touching nothing in the engine core: the host links no Vulkan, OpenXR, Jolt, renderer or ECS.

### Why it runs before `code_generation`

This is the reverse of the original order (see the comment in [`orchestrator.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/orchestrator.py)). Gameplay code that opens a door, waits on a lever, routes an NPC through a building or teleports the player must name things the world architect declared, and until this stage has run nobody knows those ids. Generating C++ first meant the codegen agent invented plausible names that resolved to nothing at runtime. There is no dependency the other way: a world builder links no ECS and never calls a mechanic. `code_generation` now reads every `world*.scene.json` and hands its agents a summary of the declared vocabulary.

### Inputs (what enters)

| Input | Location | Used for |
|-------|----------|----------|
| BuildSpec | `build_spec.json` | Environment list (`scenes[]`), scene summaries, links, nesting, cross-scene graph check, game-brief excerpt |
| NPC plan | `npc_plan.json` (optional) | Brief: NPC spawn zones for arriving NPCs, clear space for companions, `npc_anchor` posts for NPCs placed at load |
| Game design | `game_design.json` (optional) | Brief: exact spawn-point ids for `reach_location` objectives |
| Asset normalization | `asset_normalization.json` | GLB paths for optional `place_object()` |
| AssetManifest | `asset_manifest.json` | Placeable object list with `target_scale_meters` (environment-category items are skipped) |
| Scene specs | `scene_spec*.json` | Which assets are `gameplay_owned` (placed as interactive), so the world must not place copies |
| Author guide | `engine/world_author_guide.md` | The `IWorldBuilder` / `WorldContext` reference embedded in the first message |
| Project state | `ProjectState.target_device` | System prompt `world_builder_system(target_device)` names the target |

### Outputs (what it produces)

| Output | Location |
|--------|----------|
| World builder C++ | `engine/generated/world/<project_id>/*.{cpp,h,hpp}` (single environment) or `engine/generated/world/<project_id>/<scene_id>/` (multi-environment) |
| Environment JSON | `projects/<id>/world.scene.json` (single) or `projects/<id>/world.<scene_id>.scene.json` (one per environment) |
| Staged asset map | `projects/<id>/builds/worldgen_assets.json` (`asset_id → {glb_path, target_scale_meters}`) |
| Build trees | `projects/<id>/builds/worldgen/[<scene_id>/]`, `builds/worldgen_fallback/` |
| Log | `projects/<id>/logs/world.log` (full agent history, compiler and host output, truncated once per run) |

### Where output goes next

- **`code_generation`** — `_load_world_context()` summarizes every `world*.scene.json` (interactive ids, kinds, state keys, spawn points, portals) into each CodeTask agent's context.
- **`asset_optimization`** — reads the declared nav regions of `world.scene.json` to compute the closest distance the player can get to each asset (texture sizing).
- **`scene_compile`** — merges each scene's environment (`world.<scene_id>.scene.json`, falling back to `world.scene.json`, resolving `reuse_environment_from`) under the gameplay entities, repoints world props to LODs and binds affordance mechanics.

### Models, providers and cost

- OpenAI Agents SDK, Responses API, model `world_model` = `gpt-5.6-sol`, `reasoning.effort` = `world_reasoning_effort` (default `high`; `xhigh` for maximum quality), `verbosity` = `medium`.
- Tool: hosted `WebSearchTool` (no call cap). Persistent in-memory `SQLiteSession` per environment, so repair turns keep the agent's memory of what it wrote.
- Cost: one high-effort agent run per environment plus repair turns; no Tripo / image spend. Environments are capped (`world_max_environments`).

### Failure behavior

- An environment whose agent exhausts its repair budget (or errors out) gets a **deterministic fallback world** (`builtin.world.fallback`) for that environment only; the stage still succeeds and its message names which environments fell back.
- The stage **fails** only when no environment could be produced at all (every environment failed, including its fallback).
- Cross-scene graph problems (`validate_environment_graph`) are reported as warnings before any agent runs; they are planner problems, not architect problems.

---

## Detailed stage deep dive

### Files and functions

| File | Role |
|------|------|
| `stage_world_generation.py` | `run()` → `run_with_architect()`; environment jobs, briefs, write / build / run / validate loop, fallback, validators |
| `world_agent.py` | `WorldArchitect` (Agents SDK agent + session), `WorldAgentResult` schema (`files[]`, `builder_id`, `plan`, `notes`), `build_initial_message`, `build_repair_message`, `build_validation_repair_message` |
| `bounds.py` | `validate_world_includes`, `validate_world_header_content`, `filter_world_cpp_only`, `assert_world_path_allowed` |
| `prompts.py` | `world_builder_system(target_device)` |
| `engine/worldgen/` | Standalone CMake project for the `ptv_worldgen` host (`WorldGen.cpp` writes `builder_id`, `navigation`, entities, etc.) |
| `engine/world_author_guide.md` | API guide handed to the agent |

### Environment jobs (`_environment_jobs`)

The BuildSpec scene list **is** the environment list. A scene is skipped when it names `reuse_environment_from` or sets `needs_unique_environment: false`. Each job carries the scene id, name, summary (2000 chars), kind, whether it is **nested** (`environment_mode == "nested"`, with host asset, anchor slot and `fit_to_host_extent_m`, default 0.22 m), `authoring_scale_m`, its `links` (`to_scene`, kind, `spawn_point_id`) and `entry_point`. Over `world_max_environments` the list is capped, always keeping the entry point.

- **One environment** keeps the historical layout: builder id `generated.world.<project_id>`, sources directly in `engine/generated/world/<project_id>/`, output `world.scene.json`, build tree `builds/worldgen/`.
- **Several environments** each get builder id `generated.world.<project_id>.<scene_id>`, directory `.../<project_id>/<scene_id>/`, output `world.<scene_id>.scene.json`, build tree `builds/worldgen/<scene_id>/`, an agent session `ptv-world-<project_id>-<scene_id>`, and must wrap their code in `namespace ptv_world_<scene_id>` (inside `ptv::world`) so helper names do not collide. They run in parallel on a `ThreadPoolExecutor` of `min(world_max_parallel_workers, jobs)` workers.

Agents never talk to each other: each gets its full contract up front in a deterministic brief, and cross-environment consistency is checked deterministically.

### The brief

`build_initial_message()` contains: project id, output subdir, the builder id to register, a 3-step process (read the guide; optionally web-search, rejecting Unity / Unreal / Godot / WebXR; implement one `IWorldBuilder` registered exactly once with `PTV_REGISTER_WORLD_BUILDER` in a `.cpp`, guaranteeing sky + a light + ground with collision + a player spawn on the ground), extra instructions, the author guide, a BuildSpec excerpt (title, pitch, game type, locomotion, player height, constraints, the first 6 scene summaries) and the placeable asset list (`id`, `name`, `category`, `target_scale_meters`, `gameplay_owned`, notes).

Extra instructions are assembled from:

- `_env_brief()` — "one environment of N", its role, summary, authoring scale; nested environments are authored at **human scale** and scaled down by the engine, with real openings (eye sockets, viewing ports); a portal plus a named `spawn_point` for each exit and arrival (`menu_select` exits need no portal geometry).
- `_npc_brief()` — when NPCs arrive during play: at least 3 `npc_spawn_zone` entrances, each ≥ 12 m from the player spawn, on walkable floor inside a nav region; companions need 3 m of clear floor around the spawn; NPCs placed at load get a post via `w.npc_anchor(NpcAnchorDesc{id, position, facing, asset_id, tags})`.
- `_objective_brief()` — `reach_location` objectives from the game design need `w.spawn_point` with exactly those ids (scene_compile fails the game loop if they are missing).

### Per-environment loop (`_generate_one`)

The whole `engine/generated/world/<project_id>/` tree is deleted once up front, and each environment's directory again before its first attempt, so stale `.cpp` files never compile. The world C++ is **not** resume-idempotent: every run re-authors it.

For `attempt` in `1..world_max_repair_rounds` (default 6):

1. `WorldArchitect.run(message)`.
   - `WorldAgentOutputError` (schema mismatch or truncated output): the next message names the bad fields, or asks for fewer / shorter files; the round is spent but the environment is not abandoned.
   - Any other `WorldAgentError`: stop and fall back.
2. `_write_world_files()` — 1–24 files, `.cpp` / `.h` / `.hpp` only, no duplicate or traversing paths, `assert_world_path_allowed`, `validate_world_includes` (same-task headers allowed by bare filename), `validate_world_header_content` for headers, then `filter_world_cpp_only`. Nothing is written if any file violates; violations go back to the agent (`bounds_reject`).
3. `_build_worldgen()` — `configure_and_build` of `engine/worldgen` with `PTV_WORLD_PROJECT_ID=<project_id>` and `PTV_WORLD_ENV_ID=<scene_id or "">` (narrowing the source glob to one environment). Compile errors (last 12 000 chars) go back via `build_repair_message` (`compile_error`).
4. `_run_worldgen()` — `ptv_worldgen.exe --out <json> --seed <seed> --assets builds/worldgen_assets.json --builder <id>` (120 s timeout). The seed is an FNV-1a hash of `<project_id>/<label>`, so two environments do not share procedural variation. Failures go back to the agent (`run_error`).
5. `_validate_world_scene()` — failures go back via `build_validation_repair_message` (`invalid_scene`).

On exhaustion, `_emit_fallback_world()` builds a clean host into `builds/worldgen_fallback/` with **no** project sources and runs `--builder builtin.world.fallback`.

### Validators (`_validate_world_scene`)

| Check | Rule |
|-------|------|
| Basics | JSON parses; `entities` non-empty and ≤ 20 000; an entity tagged `ground`; `player.spawn.position` present |
| `_validate_entity_ids` | Every entity has an id, all unique (ids key saved game state) |
| `_validate_navigation` | Optional block; if present: regions well-formed, unique, ≥ 0.5 × 0.5 m; links name real regions and have a crossing point within 1.5 m of both regions; the player spawn lies inside a region; every region reachable from the spawn region; every `npc_spawn_zone` / `npc_anchor` inside a region. See [`navigation.md`](../../../navigation.md) |
| `_validate_interactives` | ≤ 512 interactives; unique ids that match an entity; kind ∈ `door`, `switchable`, `container`, `mover`, `breakable`, `harvestable`, `movable`; doors / containers need a hinge range or slide distance; movers ≥ 2 positions; harvestables a `yields_asset_id`; nav links `gated_by` a declared interactive |
| `_validate_portals` | Portals have unique ids and a `to_scene`; spawn-point ids are unique (the target spawn is checked cross-environment, not here) |
| `_validate_barriers` | Floor-standing boxes named / tagged wall, barrier, fence, partition or boundary (≥ 1.5 m tall and long, not dressing like trim or signs) need collision and ≥ 0.2 m thickness (the agent is told 0.3 m). Tag `walkthrough` only when the prompt asks for a passable wall |
| Grey primitives | `default_color_primitives()` counts shapes left at the default colour `(0.8, 0.8, 0.82)` — a warning in the success message, not a failure |

### Cross-scene graph (`validate_environment_graph`)

Run once before any agent: links to unknown scenes, `reuse_environment_from` naming an unknown scene or itself, `needs_unique_environment: false` without a source, nested scenes without a host, and scenes unreachable from the entry point (nested scenes count as reachable through their host). Emitted as warnings and logged with a `graph:` prefix.

### Placing assets

`_load_object_assets()` lists every non-environment manifest item with its normalized GLB path and `target_scale_meters` (the host applies that scale; it used to hard-code 1.0). The world uses the **normalization** GLBs, not LODs — `asset_optimization` runs later and `scene_compile` repoints world meshes to the LOD chain. Items already placed as interactive by a SceneSpec (mechanic bindings or `grabbable` / `weapon` / `interactable` / `wearable` tags) are marked `gameplay_owned: true` and must not be copied by the world.

### Settings (`apps/orchestrator/ptv_orchestrator/settings.py`)

Environment variables are the upper-cased field names (read from the repo-root `.env`).

| Setting | Env var | Default |
|---------|---------|---------|
| `world_model` | `WORLD_MODEL` | `gpt-5.6-sol` |
| `world_reasoning_effort` | `WORLD_REASONING_EFFORT` | `high` (keep `high` or `xhigh`) |
| `world_verbosity` | `WORLD_VERBOSITY` | `medium` |
| `world_max_repair_rounds` | `WORLD_MAX_REPAIR_ROUNDS` | 6 (1–20) |
| `world_agent_max_turns` | `WORLD_AGENT_MAX_TURNS` | 80 |
| `world_max_parallel_workers` | `WORLD_MAX_PARALLEL_WORKERS` | 3 |
| `world_max_environments` | `WORLD_MAX_ENVIRONMENTS` | 12 |
| `world_max_interactives_per_env` | `WORLD_MAX_INTERACTIVES_PER_ENV` | 512 (the validator currently uses its own constant of 512) |
| `cmake_generator` / `cmake_generator_platform` / `cmake_build_config` / `vcpkg_root` | `CMAKE_GENERATOR` … | `Visual Studio 18 2026` / `x64` / `Release` / unset |

`OPENAI_API_KEY` is bridged from settings into the process environment for the Agents SDK.

### SSE events

`running`, `warning` (graph problems, environment cap), `info` (fan-out, files written), `agent_turn`, `warn` (agent / output errors), `bounds_reject`, `compile_error`, `run_error`, `invalid_scene`, `fallback`, `log` (CMake lines), `succeeded` / `failed`.

### Quest notes

The world is built and run on the Windows host for both targets — `ptv_worldgen` is never packaged. Only its JSON reaches the APK, through `scene_compile`. The system prompt names the target device so a Quest world is authored against a mobile GPU budget (Meta puts a busy Quest 3 scene at roughly 200–300 draw calls, which is also why interactives are capped). `scene_complexity` in `scene_compile` later decides room culling and static batching from the declared nav regions.

## See also

- [`docs/world-generation.md`](../../../world-generation.md) — authoritative reference
- [`docs/navigation.md`](../../../navigation.md) — nav declaration and runtime NavGraph
- [`docs/npc-and-ai.md`](../../../npc-and-ai.md) — NPC spawn zones and anchors
- [`docs/game-flow.md`](../../../game-flow.md) — objective destinations
- Next stage: [`13_code_generation`](../13_code_generation/README.md); merge: [`15_scene_compile`](../15_scene_compile/README.md)
