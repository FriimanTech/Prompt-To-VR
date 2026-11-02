> **Shared content:** Identical to [PC_VR_Stages/17_engine_promotion](../../PC_VR_Stages/17_engine_promotion/README.md). Update both copies when this stage changes.

# Stage 17: Engine Promotion

> **Pipeline position:** after `cmake_build` (PC VR) or `quest_runtime_verify` (Quest) → end of pipeline  
> **Target:** shared (PC VR and Quest)  
> **Primary code:** [`apps/orchestrator/ptv_orchestrator/pipeline/stage_engine_promotion.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/stage_engine_promotion.py)  
> **Primary artifacts:** `engine/promoted/registry.json`

## Main stage summary

### What this stage is

The engine promotion stage indexes the generated gameplay modules this project produced into a **repo-wide registry**, so that the mechanic planner of every later project sees them as reusable `promoted.<id>` catalog entries. It writes metadata only. Sources are not copied; they stay in `engine/generated/<kind>/<project_id>/`.

### Why it exists

Each game should leave the engine more capable. Without an index, a new project would regenerate a mechanic another game already wrote and compiled. The registry makes that code visible to `mechanic_plan`, and the build can link it into the new game.

### Inputs

| Input | Location |
|-------|----------|
| CodeTask sidecars | `engine/generated/{mechanics,ai,dialogue}/<project_id>/*.codetask.json` |
| Paired C++ | the `.cpp` with the same base name (`foo.codetask.json` → `foo.cpp`) |
| Existing registry | `engine/promoted/registry.json` (missing or malformed → starts as `{"version": 1, "items": []}`) |

`engine/generated/world/` (world builders) is **not** scanned. World builders are not planner mechanics.

### Outputs

| Output | Location |
|--------|----------|
| Registry (rewritten with appended items, 2-space JSON) | `engine/promoted/registry.json` |

Each new item has these fields:

| Field | Value |
|-------|-------|
| `id` | `promoted.<sanitised CodeTask id>` (non `[a-zA-Z0-9_.-]` → `_`, lowercased, ≤ 96 chars) |
| `summary` | sidecar `description`, else `summary`, else `Generated <kind> from project <id>` (≤ 600 chars) |
| `category` | `mechanic` / `ai` / `dialogue` |
| `kind` | `mechanics` / `ai` / `dialogue` |
| `source_path` | repo-relative path of the `.cpp` |
| `code_task_id` | the CodeTask id |
| `project_id` | the producing project |
| `promoted_at` | UTC ISO timestamp |
| `params` | sidecar `params` when it is an object, else `{}` |

### Where output goes next

- **`mechanic_plan` of later projects:** `engine_catalog.as_planner_text()` reads the registry through `promoted_catalog()` and lists promoted entries in the planner's `ENGINE CATALOG`.
- **Builds of a project whose plan reuses `promoted.*`** (`builtin_id` or `base_builtin_id`): `promoted_sources_for_plan()` resolves every `<code_task_id>*.cpp` beside the registered source and passes them as `PTV_GENERATED_EXTRA_SOURCES`. The CMake build gets them as a cache variable and Quest through the `@PTV_GENERATED_EXTRA_SOURCES@` Gradle token. Entries from the same project are excluded, since they are already compiled.

### Failure behavior

The stage is best-effort and idempotent:

- A sidecar without a `.cpp`, with malformed JSON, or without an `id` is **skipped** and counted in the message (`skipped N`). It does not produce a warning event.
- An id already in the registry is skipped silently, so re-runs never duplicate entries. A changed sidecar does **not** update the existing entry.
- The stage records `succeeded` in normal operation. Only an unexpected I/O error writing the registry would raise.

It only runs if every earlier stage succeeded. On Quest, a skipped `quest_runtime_verify` counts as success, but a failed verification stops the pipeline before promotion.

---

## Detailed stage deep dive

### Execution trace (`stage_engine_promotion.run`)

1. Emit `running`, `record_stage(running)`.
2. `promoted_registry_path(repo_root)` → `engine/promoted/registry.json`; `_load_registry()`.
3. Collect existing ids.
4. For each kind in `("mechanics", "ai", "dialogue")`, iterate `sorted(gen_dir.glob("*.codetask.json"))`, where `gen_dir = engine/generated/<kind>/<project_id>`. Missing dirs are skipped.
5. For each valid, new sidecar: append the entry and emit `info: promoted <id>`.
6. `_write_registry()` always rewrites the file, creating `engine/promoted/` if needed.
7. Record `succeeded` with message `promoted N (registry now M entries)[; skipped K]`; the artifact is the repo-relative registry path.

### Paths and settings

All paths resolve from `Settings.repo_root`. This stage has no settings of its own.

### Same logic on both targets

The module is identical for PC VR (`_PCVR_TAIL`: `cmake_build` → `engine_promotion`) and Quest (`_QUEST_TAIL`: `quest_build` → `quest_runtime_verify` → `engine_promotion`). Its own module docstring still says it "runs after a successful `cmake_build`", which is only true for PC VR.

### Events

`running` → `info` (one per newly promoted id) → `succeeded`.

## See also

- [`docs/engine-growth.md`](../../../engine-growth.md): the reuse model and registry format
- [`docs/architecture.md`](../../../architecture.md)
- Previous (Quest): [`16b_quest_runtime_verify`](../16b_quest_runtime_verify/README.md)
- Stage 4: [`04_mechanic_plan`](../04_mechanic_plan/README.md)
