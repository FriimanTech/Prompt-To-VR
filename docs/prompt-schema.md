# Prompt + schemas

The `packages/schemas` package is the single source of truth for every
artifact in the pipeline. Each schema is a Pydantic v2 model and exports a
JSON Schema file under `packages/schemas/json/`.

Regenerate the JSON exports:

```powershell
py -m ptv_schemas.export packages\schemas\json
```

## Input: the user prompt

The user prompt is a single block of free-form text. The orchestrator stores
it at `projects/<id>/prompt.txt` and passes it into the BuildSpec stage.
Good prompts are *concrete*:

- describe the scene(s),
- describe the mechanics (prefer reuse of built-ins),
- list the assets,
- list acceptance tests.

A concrete example prompt lives in the `NewProjectPage` sample text.

## Artifacts

| Schema | File | Produced by |
|---|---|---|
| `BuildSpec` | `build_spec.json` | Claude Opus (via `LlmClient`; OpenAI fallback) |
| `AssetManifest` | `asset_manifest.json` | Claude Sonnet (via `LlmClient`; OpenAI fallback) |
| `NpcPlan` | `npc_plan.json` | OpenAI Agents SDK `gpt-5.6-sol` (no tools); reconciles manifest |
| `MechanicPlan` | `mechanic_plan.json` | OpenAI Agents SDK `gpt-5.6-sol` (no tools) |
| `SceneSpec` | `scene_spec.<scene_id>.json`, `scene_spec.json` | OpenAI Agents SDK `gpt-5.6-sol` (per scene) |
| `CodeTask` (embedded in MechanicPlan) | `*.codetask.json` beside generated `*.cpp` | MechanicPlan stage (executed by codegen agents) |
| Rigging record | `asset_rig.json`, `model_rigged.glb` | Tripo `asset_rigging` stage |
| Animation record | `asset_animation.json`, `model_animated.glb` | Tripo `asset_animation` stage |
| Normalization stamp | `asset_normalization.json` | Orchestrator (`asset_normalization`, deterministic) |
| Socket contract | `asset_contract.json` | Orchestrator (`asset_contract`, deterministic) |
| Codegen ledger | `codegen_board.json` | Orchestrator (`code_generation`) |
| World scene | `world.scene.json` | Offline `ptv_worldgen` (world architect agent) |
| Compiled scene | `scene.json` | Orchestrator (`scene_compile`, deterministic) |
| `PerformanceProfile` | `performance` block in `scene.json` (+ `__performance__` in `asset_normalization.json`) | Orchestrator (`asset_optimization`, deterministic) |
| `ProjectState` | `project_state.json` | Orchestrator (bookkeeping) |
| `ArtifactMetadata` | returned from `/api/projects/<id>/artifacts` | Orchestrator |
| `AcceptanceTestPlan` | `acceptance_test_plan.json` | (optional) Claude Sonnet |

See [`All_Stages_Docs/README.md`](All_Stages_Docs/README.md) for the full
17-stage artifact flow.

## Stage caps (updated for full VR-game generation)

The schemas no longer cap the pipeline to a single-room demo. The
current limits per artifact are:

| Artifact | Field | Old cap | New cap |
|---|---|---|---|
| `BuildSpec` | `scenes` (count) | 6 | **50** |
| `BuildSpec` | `mechanics` (count) | 20 | **unbounded** |
| `BuildSpec` | `assets` (count) | 48 | 48 |
| `BuildSpec` | `acceptance_tests` (count) | 24 | **unbounded** |
| `BuildSpec` | `constraints` (count) | 32 | **50** |
| `BuildSpec` | `SceneEntry.summary` length | 2 000 | **4 000** |
| `BuildSpec` | `MechanicEntry.summary` length | 2 000 | **4 000** |
| `BuildSpec` | `AssetEntry.summary` length | 2 000 | **4 000** |
| `BuildSpec` | `npc_summary` / `hud_summary` length | 2 000 | **4 000** |
| `AssetManifest` | `ReferenceImageRequest.number_of_images` | 1..4 | **pinned to 1** |
| `AssetManifest` | `items` (count) | 48 | 48 |
| `MechanicPlan` | `items` (count) | 20 | **unbounded** |
| `MechanicPlan` / `CodeTask` | `description` length | 4 000 | **10 000** |
| `SceneSpec` | `entities` (count) | 256 | **1 000** |
| `AcceptanceTestPlan` | `checks` (count) | 32 | **unbounded** |

`number_of_images` is intentionally pinned to 1 -- nothing currently
ranks / chooses between multiple candidate reference images, so
generating more than one just burns API credits. Lift the cap when a
real selection step exists.

## Performance hints on `AssetItem` (optional)

The `asset_optimization` stage derives a triangle budget for every asset, so
neither field below is required. Set them only to override the automatic
choice:

- `importance` -- `hero` / `standard` / `background`. Controls the share of the
  triangle budget an asset receives. Derived when omitted: characters and
  anything grabbable / mechanic-bound become `hero`; `fx`/`ui` and very large
  scenery become `background`.
- `max_triangles` -- hard per-asset cap after decimation. Wins over the
  computed budget AND over the quality floor.

Note that asset COUNT is never the limiting factor: a bigger project simply
gets a smaller per-asset budget plus more aggressive runtime culling and LOD.
See `docs/rendering.md` for the tier table.

## Equip slots on `SceneSpec.EntitySpec` (optional)

An `EntitySpec` may declare up to 24 `equip_slots`. Each `EquipSlotSpec` names
a place **on the wearer** where `builtin.equip` can snap an item -- a hip
holster, a back sheath, a wrist mount. It is not the same thing as a mesh
socket: a socket is a point on the ITEM (`grip` on the pistol), a slot is a
place on the BODY. A holster prop in the scene is set dressing; the slot goes
on the rig.

| Field | Type | Meaning |
|---|---|---|
| `name` | `IdStr` | Slot id gameplay refers to, e.g. `hip_holster_r` |
| `body_slot` | `str` | `hand_r`, `hand_l`, `waist`, `wrist_l`, `wrist_r`, `back`, `chest`, `head` |
| `host_item_slot` / `host_socket` | `str` | For a slot carried on an already-equipped item |
| `local_position` | `Vec3` | Offset from the body slot, e.g. `[0.16, -0.05, 0.02]` from `waist` |
| `local_rotation` | `QuatXYZW` | Orientation of the slot |
| `snap_radius_m` | `float` 0..2 | How near an item must come; 0.15-0.25 for a holster |
| `exclusive` | `bool` | One item at a time (default `true`) |
| `accepts_tags` | `List[str]` | Which items fit; empty lets a rifle into a pistol holster |

`scene_compile` copies these into `scene.json` and `SceneLoader` turns them
into `CEquipSlots`. It hard-fails when a `builtin.equip` binding names a slot
no entity declares, or when two entities declare the same slot name.

## Built-in mechanic ids (reuse these)

```
builtin.teleport
builtin.locomotion.teleport
builtin.locomotion.slide.head_relative
builtin.locomotion.slide.controller_relative
builtin.snap_turn
builtin.locomotion.snap_turn
builtin.grab
builtin.throw
```

Anything outside this list must be modeled as a `CodeTask` and is classified
`generate` in the `MechanicPlan`.

## Strictness

All models use `extra="forbid"` (unknown fields raise). This is deliberate:
it catches schema drift early and makes Claude's structured outputs safe to
pass directly to the engine.
