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
| `ProjectState` | `project_state.json` | Orchestrator (bookkeeping) |
| `ArtifactMetadata` | returned from `/api/projects/<id>/artifacts` | Orchestrator |
| `AcceptanceTestPlan` | `acceptance_test_plan.json` | (optional) Claude Sonnet |

## Built-in mechanic ids (reuse these)

```
builtin.teleport
builtin.smooth_locomotion
builtin.snap_turn
builtin.grab
builtin.throw
builtin.use_interact
builtin.push_pull
builtin.trigger_zone
builtin.spawner
builtin.inventory
builtin.dialogue_trigger
builtin.health_damage
builtin.enemy_ai_hook
builtin.projectile_hook
builtin.hud_anchor
```

Anything outside this list must be modeled as a `CodeTask` and is classified
`generate` in the `MechanicPlan`.

## Strictness

All models use `extra="forbid"` (unknown fields raise). This is deliberate:
it catches schema drift early and makes Claude's structured outputs safe to
pass directly to the engine.
