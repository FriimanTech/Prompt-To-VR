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
| `BuildSpec` | `build_spec.json` | Claude Opus |
| `AssetManifest` | `asset_manifest.json` | Claude Sonnet |
| `MechanicPlan` | `mechanic_plan.json` | Claude Opus |
| `SceneSpec` | `scene_spec.<scene_id>.json`, `scene_spec.json` | Claude Opus (per scene) |
| `CodeTask` (embedded in MechanicPlan) | emitted as `*.codetask.json` next to generated `*.cpp` | Claude Opus |
| `PerformanceProfile` | `performance` block in `scene.json` (+ `__performance__` in `asset_normalization.json`) | Orchestrator (`asset_optimization`, deterministic) |
| `ProjectState` | `project_state.json` | Orchestrator (bookkeeping) |
| `ArtifactMetadata` | returned from `/api/projects/<id>/artifacts` | Orchestrator |
| `AcceptanceTestPlan` | `acceptance_test_plan.json` | (optional) Claude Sonnet |

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

## Built-in mechanic ids (reuse these)

```
builtin.teleport
builtin.locomotion.teleport
builtin.smooth_locomotion
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
