# ptv-schemas

Single source of truth for every artifact that flows through the
Prompt-To-Native-VR pipeline.

Each schema is a Pydantic v2 model and an exported JSON Schema. The engine and
the orchestrator both read the JSON form; only the orchestrator uses the
Pydantic models.

## Schemas

| Schema | Purpose |
|---|---|
| `BuildSpec` | Top-level game plan: scenes, mechanics, assets list, tests |
| `AssetManifest` | Per-asset reference-image and Tripo request details |
| `MechanicPlan` | Classifies every mechanic as builtin / config / generate |
| `SceneSpec` | Deterministic entity / component layout for a single scene |
| `CodeTask` | Single bounded C++ module to be generated |
| `ProjectState` | Per-project pipeline bookkeeping |
| `ArtifactMetadata` | Per-file metadata (path, sha256, stage) |
| `AcceptanceTestPlan` | Human-runnable acceptance tests |

## Exporting JSON Schema

```powershell
ptv-export-schemas packages\schemas\json
```

Produces one `<name>.schema.json` per model.
