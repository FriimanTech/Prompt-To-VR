# Stage 15: Scene Compile

> **Pipeline position:** after `asset_optimization` → before `quest_build` (Quest) / `cmake_build` (PC VR); on Quest it is also re-run by the memory-correction loop after `quest_runtime_verify`  
> **Target:** shared (PC VR and Quest)  
> **Primary code:** [`apps/orchestrator/ptv_orchestrator/pipeline/stage_scene_compile.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/stage_scene_compile.py) plus [`encounters.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/encounters.py), [`game_flow_scene.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/game_flow_scene.py), [`weapon_scene.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/weapon_scene.py), [`projectile_scale.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/projectile_scale.py), [`player_bindings.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/player_bindings.py), [`gameplay_relocation.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/gameplay_relocation.py), [`binding_resolution.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/binding_resolution.py), [`scene_complexity.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/scene_complexity.py), [`memory_model.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/memory_model.py)  
> **Primary artifacts:** `projects/<id>/scene.json`, `projects/<id>/scene.<scene_id>.json`, `projects/<id>/builds/scene/<scene_id>.scene.json`, `scene_complexity.json`, `memory_status.json`

## Main stage summary

### What this stage is

The **scene compile stage** is the deterministic merge that produces what the engine loads. For every scene in the BuildSpec that has a `scene_spec.<scene_id>.json`, it combines the gameplay SceneSpec, the scene's C++-generated environment, the optimized mesh metadata, the asset contract (sockets, holds, model frames), mechanic bindings, the NPC population, the game loop, the weapons and the device performance block into one runtime scene document, then analyses each scene's cost and memory. No LLM runs here.

### Why it exists

Upstream artifacts are partial and planning-shaped. The runtime needs one coherent document per scene: meshes with LOD paths and sizes in metres, clip and socket contracts, navigation, NPC templates, the game director, weapon bindings, a scene index for portals, and performance limits. It is also the only stage that sees all halves of several contracts (sockets, equip slots, bindings vs. what the build registers, game loop vs. world, weapons vs. meshes), so mismatches fail here, loudly and all at once, instead of shipping a game that quietly does nothing.

### Inputs (what enters)

| Input | Location |
|-------|----------|
| BuildSpec | `build_spec.json` (scene list, entry point, player settings, `interaction_rules`) |
| AssetManifest | `asset_manifest.json` |
| MechanicPlan | `mechanic_plan.json` (config, controller bindings, CodeTasks with `required_sockets`) |
| Scene specs | `scene_spec.<scene_id>.json` |
| Normalization report | `asset_normalization.json` (LODs, bounds, `lod0_longest_axis`, `skinned`, animations, `__performance__`) |
| Asset contract | `asset_contract.json` (sockets, holds, model frames, part regions) |
| Worlds | `world.<scene_id>.scene.json`, falling back to `world.scene.json` |
| NPC plan | `npc_plan.json` (optional) |
| Game design | `game_design.json` (optional) |
| Interaction design | `interaction_design.json` (optional) |
| Generated source | `engine/generated/**/<project_id>/` (registered ids; whether code calls `ctx.encounters->start()`) |
| Headset measurements | `runtime_verification.json` (optional; measured frame cost beats the model) |
| Memory calibration | `projects/memory_calibration.json` |

### Outputs (what it produces)

| Output | Location |
|--------|----------|
| Runtime scenes | `projects/<id>/scene.<scene_id>.json` — one per compiled scene, each with the full `scene_index` |
| Entry scene | `projects/<id>/scene.json` — copy of the entry-point scene; `ProjectState.latest_scene_json` points at it |
| Debug copies | `projects/<id>/builds/scene/<scene_id>.scene.json` (not read by the runtime) |
| Complexity summary | `projects/<id>/scene_complexity.json` |
| Memory status | `projects/<id>/memory_status.json` (`estimated_safe_not_device_tested` / `at_risk`; never overwrites a `source: device` status) |

Stale `scene.<id>.json` files from an earlier plan are removed, because the Quest packager ships every one beside `scene.json`.

### Where output goes next

- **`quest_build`** packages `scene.json`, every `scene.<id>.json`, the LOD GLBs, KTX2 textures and shaders into the APK; **`cmake_build`** points `PTV_SCENE_JSON` at `scene.json`.
- **Runtime** — `SceneLoader` reads the document into the ECS (meshes, `CEquipSlots`, navigation, performance, …).
- **`quest_runtime_verify`** — checks the game against what this stage bound.

### Models, providers and cost

None. Pure Python with deterministic validators.

### Failure behavior (hard failures, all violations listed at once)

| Error | Raised when |
|-------|-------------|
| `SceneAssetMismatchError` | A SceneSpec mesh names an asset absent from the manifest (a SceneSpec composed against an older manifest) |
| `SocketContractError` | A bound CodeTask's `required_sockets` are missing on the entity's asset, no asset in the scene supplies a socket a mesh-less binding needs, or a hold cannot be solved |
| `EquipSlotContractError` | A `builtin.equip` binding names a slot nobody declares, or two placed entities declare the same slot (spawn templates exempt) |
| `ModelFrameContractError` | A skinned asset has no measured model frame in `asset_contract.json` |
| `GameFlowContractError` | The game design's loop cannot complete in a scene |
| `WeaponContractError` | A designed weapon cannot work (gun without muzzle, bow without limb tips / grip, melee without strike point, missing template or source, seen projectile without a mesh) |
| `TextureContractError` | A placed mesh's LOD references a texture file that does not exist |
| `MissingNpcError` | An NPC the plan requires has no model to place |
| `UnresolvedBindingError` | A mechanic binding names something the build does not register |
| `PlayerRoleError` | Two different mechanics for one player role, or a locomotion / turn role the BuildSpec `movement` block does not allow |

Warnings (stage continues): over the animated-agent budget, deferred sockets satisfied by other assets, world copies adjusted, projectile rescales, physics repairs, unparseable world documents, affordances denied by `interaction_rules`, Quest textures without ASTC.

---

## Detailed stage deep dive

### `run()` sequence

1. Load BuildSpec, manifest, plan, normalization, contract; entry scene = the BuildSpec scene with `entry_point`, else the first.
2. Load NPC plan, game design, interaction design; `registered_ids()` (what this build registers); whether generated code calls `encounters->start(`.
3. `projectile_scale_corrections()` sizes projectiles to the measured muzzle bore and arrows longer than the bow's draw (sizes the prompt states are kept); `apply_scales()` gives the weapon pass a corrected manifest. `repair_npc_plan()` applies the npc_plan stage's deterministic repairs.
4. For each scene with a SceneSpec:
   1. `compile_scene_json()` — the pure core (below).
   2. NPC population (`apply_npc_population`) when the scene is the entry scene or places an NPC asset: one hidden template per spawned type, companions beside the player, per-type stats and built-in behaviours, the encounter director. `drop_unhosted_npc_ai()` first removes generated AI the build will not host, so that NPC gets the built-in behaviour instead of standing still.
   3. `_load_world(scene_id)` (resolving `reuse_environment_from`) → `_merge_world_into_scene()`.
   4. `place_persistent_npcs()` against the merged world (a matching `npc_anchor` wins; on declared walkable floor; feet on the floor on frame one).
   5. `finalize_encounter_zones()` — the world's `npc_spawn_zones`, or zones derived from walkable regions.
   6. `apply_game_flow()` binds one `ptv_game_director` entity (`builtin.ui_panel` + `builtin.game_flow` + `builtin.game_hud`); `game_flow_violations()` fails the stage if any path cannot complete (e.g. a `reach_location` target the world never declared).
   7. `apply_weapons()` + `weapon_violations()` (see Weapons).
   8. `sanitize_physics()` on the final entity list (a dynamic / kinematic body must have positive mass — Jolt asserts otherwise, a SIGTRAP on the headset).
   9. `bind_affordances()` (see World merge).
   10. `check_animated_agent_budget()` (warning) and `unresolved_bindings()`.
5. `_check_scene_textures()` → `TextureContractError` on missing files; writes `performance.textures`.
6. `_apply_scene_complexity()`.
7. Raise `MissingNpcError` / `UnresolvedBindingError` if collected.
8. Write the scene index and every scene document (atomic writes), the debug copies and `scene.json`.

### `compile_scene_json()` (pure, testable)

- Plan items are indexed by both plan id and CodeTask id, so `builtin.<x>` and `generated.<task_id>` bindings resolve. For `builtin` / `builtin+config` items the binding gets `builtin_id`, and a stale `generated.<id>` binding whose item is now a built-in is renamed.
- `params` = item `config` overlaid by per-entity params; the plan's controller `binding` is materialized as `params.control` / `params.hand` (per-entity values win).
- Meshes resolve `glb_path` through the normalization report (LOD0) and gain `_mesh_render_fields()`:
  - `lod_paths`, `lod_triangle_counts`, `lod_errors_rel`, `bounds_radius_m` (unit radius × `target_scale_meters`);
  - `unit_scale = target_scale_meters / lod0_longest_axis`;
  - `skinned` + `joint_count`;
  - `animations` (intent → real clip name) and `animation_speeds` (0.78 × height m/s walk, 2.2 × height run; Uthana `animation_speed_overrides` win);
  - for framed characters: `scale_basis: "height"`, `unit_scale = height / raw height`, `foot_offset_m = height / 2`, `model_yaw_offset_rad`, `model_frame {sole_y, height, yaw_offset_rad, method}`; a skinned asset without a frame raises `ModelFrameContractError`;
  - `sockets` converted from unit space to metres, and the `hold` profile.
- Entities carry transform, mesh, physics, bindings, tags, `health`, `faction`, `attached_to`, `equip_slots` (`_equip_slot_json`).
- **Asset safety net**: every manifest asset with a GLB on disk that no entity references gets `_default_entity_for_asset()` (environment → static mesh at origin; character → NPC with a role from style-notes heuristics; prop → grabbable or static dressing; fx → impostor; ui → panel), spread on a golden-angle spiral.
- `bind_player_roles()` — Hands Only removes `player_avatar` entities; each required role gets exactly one binding (`role:body` on the avatar, the rest on `player_rig`, created meshless and tagged `player_system` when absent); duplicates of one mechanic collapse with a warning; conflicts raise `PlayerRoleError`. `player_block_fields()` adds `hands.source`, `default_locomotion_mode`, `default_turn_mode`, `locomotion_menu: false` to the `player` block.
- `_apply_hold_offsets()` forwards `hold_rotation_offset_deg` into `builtin.grab` params.
- Socket checks (`_check_required_sockets`) read `required_sockets` from the plan item's CodeTask. A bound entity **with** a mesh must carry every required socket; one **without** a mesh (a hand resolving the held weapon at runtime, guarded by `has_socket`) is deferred to `_check_deferred_socket_needs()`: satisfied by some asset in the scene → warning naming the providers, unsatisfiable → violation. `hold_violations()` adds unsolvable holds (errors) and suspicious ones (warnings).
- `_check_equip_slots()` — a slot is a place on a WEARER (`hip_holster_r` on the player rig), a socket a point on an ITEM.
- The returned document: `version`, `prompt_id`, `scene_id`, `title`, `name`, `player` (height, locomotion, comfort, role fields, spawn), `sky_color`, `ambient_intensity`, `lights`, `entities`, and `performance` = the normalization report's `__performance__`.

### World merge (`_merge_world_into_scene`)

Every scene gets its own environment (not just the entry point):

1. If the world's player spawn differs from the SceneSpec's, `relocate_gameplay_layer()` moves the whole gameplay layer into the world spawn frame (using the SceneSpec's spawn yaw).
2. `resolve_overlaps()` moves gameplay entities out of world geometry and puts characters on declared walkable floor.
3. `_repoint_world_meshes_to_lods()` — world props were placed with normalization GLBs (world generation runs before optimization); they are repointed to LOD0 + render fields, sized from the **manifest** so a world copy matches the gameplay one.
4. `reconcile_world_copies()` — a world copy within 0.5 m of a gameplay entity of the same asset is dropped; a farther copy of an interactive asset inherits its non-player bindings, physics and interaction tags.
5. World entities are prepended; world `sky` / `sky_color` / `ambient_intensity` / lights / `navigation` / player spawn replace or extend the SceneSpec's; `interactives`, `spawn_points`, `portals`, `npc_spawn_zones` and `npc_anchors` travel with the environment; `environment_builder_id` records the builder.

`bind_affordances()` then turns each declared interactive into a binding from a fixed table — `door` → `builtin.door`, `switchable` → `builtin.switchable`, `container` → `builtin.container`, `mover` → `builtin.mover`, `breakable` → `builtin.breakable`, `harvestable` → `builtin.harvestable` (`reach_m` 0.5); `movable` is declaration-only. A table rather than an agent, so equivalent objects behave the same in every environment; a BuildSpec `interaction_rules` entry that denies the kind or a tag in that scene sets `interaction_allowed: false` (the binding stays, the runtime refuses the interaction and hides the cue).

### Weapons (`weapon_scene.py`, `projectile_scale.py`)

With an `interaction_design.json`, `apply_weapons()` binds every designed weapon deterministically: `builtin.weapon` / `builtin.bow` / `builtin.melee_weapon` / `builtin.shield` with the design's numbers and socket names (player weapons become `grabbable`); removes `builtin.recoil` and `generated.*` weapon bindings the plan no longer has; removes entities showing an effect mesh the engine draws itself (a beam, a muzzle flash); emits a hidden projectile template per projectile with a mesh, and an arrow template plus a `builtin.item_source` quiver for bows; arms NPCs whose plan has a ranged attack (`builtin.npc.ranged_attack`, the weapon seated in `hand_r` by its grip socket via `attached_to.socket`); writes the scene-level `projectiles` table; and adds an ammo `builtin.game_hud` when a weapon counts ammo. `weapon_violations()` returns errors (→ `WeaponContractError`) and warnings. See [`weapons.md`](../../../weapons.md).

### Scene complexity and memory (`_apply_scene_complexity`)

Per scene, `scene_complexity.analyse()` samples visibility from the walkable regions (p50 / p95 visible triangles and draws), estimates GPU / CPU frame cost, detects rooms closed on all four sides and finds entities that may be static-batched (nothing moves, holds, binds or reads them). For the entry scene a previous `runtime_verification.json` replaces the GPU / CPU estimate with the Quest 3 measurement. `decide_initial_settings()` then sets, for knobs the user did not lock:

- **FFR** (Quest with FDM): level 1, or 2 when estimated GPU p95 > 80 % of the frame budget, capped by the governor's `max_foveation_level`;
- **multiview**: on only at ≥ 120 estimated p95 draws (it measured ~10 % more GPU on Quest 3 at low draw counts);
- **room culling**: on with ≥ 2 closed rooms (`perf.rooms` lists them);
- **static batch cell**: 16 m above 2 500 m² of walkable area, else 8 m.

`apply_static_batch_flags()` marks batchable entities when `static_batching` is on. The memory model (`memory_model.estimate(inputs_from_scene(...))`, exact from the built files where possible, calibrated by `projects/memory_calibration.json`) writes `performance.memory_estimate` (steady, peak, always-resident, streamable, load transient, outcome, gap) and `performance.resident_memory_budget_mb`, against the 4 608 MB target / 5 120 MB ceiling / 260 MB baseline. `scene_complexity.json` gets the per-scene analysis; `memory_status.json` gets `at_risk` if any scene is over, else `estimated_safe_not_device_tested` — never "verified" from here.

### Scene index

`scene_index = {scene_id: "scene.<scene_id>.json"}` for every compiled scene, written into **every** document (a menu or a generated mechanic may reach a scene no portal points at). Paths are relative to the document so the same layout works on disk and inside the APK.

### Settings

No stage-specific settings. `repo_root` / `projects_dir` locate artifacts and the calibration; everything else is read from the upstream artifacts.

### SSE events

`running`, `info` (NPC plan repairs, encounter zones, game flow bound, weapons, affordances, complexity / memory per scene, persistent NPC placement, removed stale scenes), `warning` (see above), `failed` (game flow / weapons), `succeeded`.

### Quest notes

The compiled documents are identical for both targets; Quest differences are in the data: the `performance` block comes from the Quest profile and user locks, FFR / multiview / room culling are decided for the Quest GPU, `_check_scene_textures` warns for every texture without an ASTC KTX2 twin (it would be decoded on the headset), and `memory_status.json` is the provisional status `quest_runtime_verify` later replaces with measurements. A headset memory failure re-runs `asset_optimization` → `scene_compile` → `quest_build` → verify (at most `quest_memory_max_correction_rounds`).

## See also

- [`docs/architecture.md`](../../../architecture.md) — data flow and contracts
- [`docs/weapons.md`](../../../weapons.md), [`docs/game-flow.md`](../../../game-flow.md), [`docs/npc-and-ai.md`](../../../npc-and-ai.md), [`docs/navigation.md`](../../../navigation.md), [`docs/sockets.md`](../../../sockets.md)
- [`docs/rendering.md`](../../../rendering.md), [`docs/quest-optimization.md`](../../../quest-optimization.md)
- Next: Quest [`16_quest_build`](../../Quest_Stages/16_quest_build/README.md) / PC VR [`16_cmake_build`](../../PC_VR_Stages/16_cmake_build/README.md)
