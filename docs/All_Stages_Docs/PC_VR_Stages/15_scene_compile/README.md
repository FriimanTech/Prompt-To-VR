# Stage 15: Scene Compile

> **Pipeline position:** after `asset_optimization` → before device build (stage 16)  
> **Target:** shared (PC VR and Quest)  
> **Primary code:** [`apps/orchestrator/ptv_orchestrator/pipeline/stage_scene_compile.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/stage_scene_compile.py)  
> **Primary artifacts:** `projects/<project_id>/scene.json`, `builds/scene/<scene_id>.scene.json`

## Main stage summary

### What this stage is

The **scene compile stage** is the deterministic merge step: it combines the C++ **world** layer (`world.scene.json`), gameplay **SceneSpec** entities, optimized **GLB metadata**, **mechanic bindings**, and **socket contracts** into the final runtime `scene.json` the engine loads.

No AI runs here — pure Python logic with hard validation failures.

### Why it exists

Upstream stages produce partial artifacts optimized for planning or tooling. The runtime needs one coherent JSON document: meshes with LOD paths, animations map, sockets in metres, navigation graph, performance limits, and entity mechanics list.

This stage is also the **socket validator**: it is the only place that sees both codegen `required_sockets` and asset-derived sockets, so mismatches fail loudly with all violations listed.

### Inputs (what enters)

| Input | Location |
|-------|----------|
| BuildSpec | `build_spec.json` |
| AssetManifest | `asset_manifest.json` |
| MechanicPlan | `mechanic_plan.json` |
| Scene specs | `scene_spec.<scene_id>.json` |
| Normalization | `asset_normalization.json` (LODs, bounds, skinned) |
| Asset contract | `asset_contract.json` |
| World | `world.scene.json` (entry scene) |

### Outputs (what it produces)

| Output | Location |
|--------|----------|
| Per-scene JSON | `builds/scene/<scene_id>.scene.json` |
| Canonical scene | `scene.json` (entry point) |
| State pointer | `ProjectState.latest_scene_json` |

### Where output goes next

- **`cmake_build`** — `PTV_SCENE_JSON` points at `scene.json`
- **`quest_build`** — copies `scene.json` + LOD GLBs into APK assets
- **Runtime** — `SceneLoader` reads JSON into ECS

### Key concepts and terms

- **Deterministic merge** — Same inputs always produce same output; no LLM.
- **World + gameplay layers** — `_merge_world_into_scene()` adds environment under gameplay entities.
- **Asset safety-net** — `_default_entity_for_asset()` spawns manifest assets the SceneSpec agent forgot.
- **SocketContractError** — Hard fail if a mechanic requires `"muzzle"` but contract lacks it.
- **EquipSlotContractError** — Hard fail if a `builtin.equip` binding names a slot nothing declares.
- **SceneAssetMismatchError** — Hard fail if a mesh names an asset absent from the manifest.
- **Performance block** — `max_animated_agents`, LOD distances, AI tick rates forwarded to runtime.

### End-to-end flow (plain language)

1. For each scene in BuildSpec, load SceneSpec + world (entry scene only).
2. `compile_scene_json()` merges entities, mechanics, meshes, sockets, navigation.
3. Repoint world prop meshes to optimized LOD paths.
4. Validate required sockets; warn on animated agent budget.
5. Write `scene.json` files.

---

## Detailed stage deep dive

### Pure deterministic merge — no LLM

`compile_scene_json()` is a testable function merging upstream JSON into runtime scene format.

### `run()` sequence

1. Load build_spec, manifest, mechanic_plan, normalization, contract, optional `world.scene.json`
2. Determine entry scene id from BuildSpec
3. For each scene with `scene_spec.<id>.json`:
   - **`compile_scene_json()`** — entities, mechanics, mesh refs, bindings
   - If entry scene: **`_merge_world_into_scene()`** — sky, ground, lights, spawn, navigation block from world
   - **`_repoint_world_meshes_to_lods()`** — world props use optimized LOD paths
   - **`check_animated_agent_budget()`** — warn if over `max_animated_agents`
   - Write `builds/scene/<id>.scene.json`
4. Copy entry scene → top-level `scene.json`; set `state.latest_scene_json`

### Key merge behaviors

| Function | Role |
|----------|------|
| `_default_entity_for_asset()` | Safety-net: manifest asset missing from SceneSpec → auto entity by category |
| `_mesh_render_fields()` | Attach `lod_paths`, `skinned`, `animations`, `animation_speeds` |
| `_sockets_in_meters()` | Scale unit-space contract by `target_scale_meters` |
| `_check_required_sockets()` | Read codegen sidecars `required_sockets` vs contract — **SocketContractError** lists ALL violations |
| `_check_equip_slots()` | Every `builtin.equip` binding names a slot some entity declares — **EquipSlotContractError** |
| `_equip_slot_json()` | Flatten `EquipSlotSpec` into the `equip_slots[]` the runtime parses into `CEquipSlots` |
| Materialize bindings | Copy mechanic_plan controller bindings + params into scene entities |

### Socket validator (hard fail)

Only stage seeing both codegen requirements AND asset_contract. Mismatch fails loudly before build.

### Animated agent budget

Warning only — does not fail stage. Over-cap scenes may miss frame rate on Quest.

### Artifacts

Writes `builds/scene/*.scene.json`, canonical `scene.json` — consumed by cmake_build / quest_build and runtime SceneLoader.

### SSE

`warning` (animated budget), `succeeded`

### `_check_required_sockets()` algorithm

For each entity with a bound mechanic that has a `code_task`:

1. Load `required_sockets[]` from mechanic's `.codetask.json` sidecar (written at codegen)
2. Load derived socket names from `asset_contract.json` for entity's mesh asset
3. For each required name not in contract → append violation string
4. After all entities scanned, if any violations → raise `SocketContractError` with **full list**

This is the only pipeline point that sees both halves of the socket contract.

**Entities with no mesh are checked differently.** A shooting hub is bound to the player's hand, and a hand has no mesh by design — it reads the muzzle off whatever weapon is held *at runtime*, guarded by `ctx.sockets->has_socket(weapon, "muzzle")`. Demanding the socket sit on the bound entity's own asset turned every correctly written hub into a build failure. So:

| Bound entity | Rule |
|--------------|------|
| has a mesh | the socket must exist on **that asset**, or hard fail (the original protection, unchanged) |
| has no mesh | the socket must exist on **some asset referenced by the scene** — satisfied emits a `warning` naming the providers, unsatisfiable is still a hard fail |

The guarantee "this socket exists somewhere in this game" survives; the false positive does not. `MECHANIC_PLAN_SYSTEM` states the matching rule for planners: `required_sockets` means sockets read on `self`, and a hub that resolves its target at runtime leaves it empty.

### Weapons (`pipeline/weapon_scene.py`, `pipeline/projectile_scale.py`)

With an `interaction_design.json`, after the game flow is bound, `apply_weapons` binds every designed weapon deterministically: `builtin.weapon` / `builtin.bow` / `builtin.melee_weapon` / `builtin.shield` with the design's numbers and socket names (player weapons become `grabbable`); strips `builtin.recoil` and `generated.*` bindings the plan no longer has; emits a hidden projectile template per projectile with a mesh, an arrow template plus a `builtin.item_source` quiver entity for bows; arms NPCs whose plan has `ranged_attack` (the NPC weapon attached to `hand_r` by its grip socket via `attached_to.socket`, or `builtin.weapon` on a character modelled holding its gun); writes the scene-level `projectiles` table; and adds an ammo-only `builtin.game_hud` when a weapon counts ammo. Before that, `projectile_scale_corrections` sizes each projectile mesh to the measured muzzle bore (0.6-1.3x, energy bolts 1-3x) and each arrow longer than its bow's draw, reporting every check and never changing a size the prompt states. `weapon_violations` then fails the stage (`WeaponContractError`, all problems listed) on a gun with no muzzle socket, a bow without limb tips / grip, a melee weapon with no strike point, a missing template or source, or a seen projectile with no mesh. See [docs/weapons.md](../../../weapons.md).
### Equip slots (`_check_equip_slots()`)

An equip **slot** is a place *on a wearer* (`hip_holster_r` on the player rig); a **socket** is a point *on an item* (`grip` on the pistol). They are not interchangeable, and the slot never belongs to the holster prop — the holster is set dressing, the slot is on the body.

SceneSpec entities carry `equip_slots[]`, which this stage copies into `scene.json` and `SceneLoader` turns into `CEquipSlots`. Without that, `EquipMechanic::pick_slot_near()` finds no candidate and an authored holster silently accepts nothing. Two hard failures guard it:

- a `builtin.equip` binding whose `params.slots` names a slot no entity in the scene declares
- two entities declaring the same slot name

### Meshes must name a real asset

A mesh whose `asset_id` is absent from the current `AssetManifest` used to fall through to the SceneSpec's own `glb_path` and emit a dangling path into `scene.json`. That is now a `SceneAssetMismatchError` naming the offending `scene_spec.<scene_id>.json`, because the usual cause is a scene composed against a **previous** manifest.

### Asset safety-net (`_default_entity_for_asset`)

If SceneSpec LLM forgot a manifest asset, compile injects a default entity:

| Category | Default behavior |
|----------|------------------|
| environment | Static world mesh at origin |
| character | NPC capsule; role from style_notes heuristics (enemy/player_avatar/npc) |
| prop | Grabbable if style_notes match grab keywords, else static dressing |
| fx | Non-physics impostor near spawn |
| ui | HUD panel in front of spawn |

Uses golden-angle xz spread (`137.508°`) so multiple forgotten props do not stack at one point.

### `_merge_world_into_scene()` details

Entry-point scene only:

- Prepends world entities (sky, ground, static geometry) from `world.scene.json`
- Copies `navigation` block `{ regions, links }` for runtime NavGraph
- Applies `_repoint_world_meshes_to_lods()` so world props use `_lod0.glb` paths from optimization
- Gameplay entities from SceneSpec remain authoritative for id conflicts on gameplay layer

### Player roles (`pipeline/player_bindings.py`)

`bind_player_roles()` runs after the asset safety net:

- Hands Only removes every `player_avatar` entity.
- Each required role gets exactly one binding: `role:body` on the
  `player_avatar` entity (with the BuildSpec's coverage / head visibility /
  leg animation), everything else on `player_rig` (created meshless and tagged
  `player_system` when the SceneSpec has none). Plan config and controller
  bindings carry through.
- The same mechanic bound twice collapses to one (warning). Two DIFFERENT
  mechanics for one role, counted across ALL entities, or a locomotion / turn
  role the BuildSpec `movement` block does not allow, raise
  `PlayerRoleError` -- hard fail.
- The `player` block gains `hands.source` (`avatar` only when a body is
  bound), `default_locomotion_mode`, `default_turn_mode` and
  `locomotion_menu: false`.

### Sizes, grips and world copies

- `_mesh_render_fields()` writes `unit_scale = target_scale_meters /
  lod0_longest_axis` for every optimized mesh.
- `_repoint_world_meshes_to_lods()` takes `target_scale_meters` from the
  manifest, so a world-placed copy matches the gameplay one.
- `reconcile_world_copies()` gives a world copy of an interactive asset the
  gameplay entity's bindings, physics and interaction tags, and drops a copy
  within 0.5 m of the gameplay entity (the same object placed twice).
- `_apply_hold_offsets()` forwards the manifest's `hold_rotation_offset_deg`
  into that asset's `builtin.grab` params.

---

## Extend this folder

- `artifacts.md` — scene.json top-level blocks
- `failure-modes.md` — socket validation messages

## See also

- [`docs/architecture.md`](../../../architecture.md) — data flow + socket contract
- Stage 16 PC [`16_cmake_build`](../16_cmake_build/README.md) / Quest [`16_quest_build`](../../Quest_Stages/16_quest_build/README.md)
