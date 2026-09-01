> **Shared content:** Identical to [PC_VR_Stages/17_engine_promotion](../../PC_VR_Stages/17_engine_promotion/README.md). Update both copies when this stage changes.

# Stage 17: Engine Promotion

> **Pipeline position:** after device build (`cmake_build` or `quest_build`) → end of pipeline  
> **Target:** shared (PC VR and Quest)  
> **Primary code:** [`apps/orchestrator/ptv_orchestrator/pipeline/stage_engine_promotion.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/stage_engine_promotion.py)  
> **Primary artifacts:** `engine/promoted/registry.json`

## Main stage summary

### What this stage is

The **engine promotion stage** indexes successfully compiled generated modules from this project into a **shared registry** so future games' mechanic planner can reuse them as `promoted.*` catalog entries. It does not copy source files — they already live under `engine/generated/` and are linked by CMake.

### Why it exists

Prompt-to-VR is designed so **each game makes the engine more capable**. Without promotion, every new project would regenerate similar shooting hubs or AI patterns. Promotion makes reuse visible to the mechanic-plan agent on the next pipeline run.

### Inputs (what enters)

| Input | Location |
|-------|----------|
| CodeTask sidecars | `engine/generated/{mechanics|ai|dialogue}/<project_id>/*.codetask.json` |
| Paired C++ | matching `.cpp` files |
| Prior successful build | implied by pipeline reaching this stage |

### Outputs (what it produces)

| Output | Location |
|--------|----------|
| Registry append | `engine/promoted/registry.json` |

Each entry: `promoted.<sanitised_id>`, summary, category, source project, timestamp.

### Where output goes next

**Next project's stage 4 (`mechanic_plan`)** — `engine_catalog.as_planner_text()` includes promoted entries alongside built-ins.

### Key concepts and terms

- **Reuse-first planner** — Mechanic plan tries `builtin` → `builtin+config` → `code_edit` → `generate`. Promoted modules appear as catalog entries.
- **Best-effort** — Malformed sidecars skipped with warning; **never fails the pipeline**.
- **Idempotent** — Re-running same project does not duplicate registry ids.
- **No file copy** — Registry is discoverability metadata only; sources stay in `engine/generated/<kind>/<project_id>/`.

### End-to-end flow (plain language)

1. After build succeeds, scan generated dirs for `.codetask.json` + `.cpp` pairs.
2. Load existing `registry.json`.
3. For each new module, append entry if id not present.
4. Save registry; emit promotion count.

---

## Detailed stage deep dive

### Best-effort final stage

**Never fails the pipeline.** Malformed sidecars skipped with warnings.

### Scan paths

For each kind in `mechanics`, `ai`, `dialogue`:

```
engine/generated/<kind>/<project_id>/*.codetask.json
```

Requires paired `.cpp` with same basename.

### Registry entry

Id format: `promoted.<sanitised_task_id>` (lowercase, safe chars, max 96).

Append to `engine/promoted/registry.json`:

- summary from sidecar
- category/kind
- source project id
- timestamp

**Idempotent:** skip if `promoted_id` already in registry.

### Does NOT copy source files

Generated `.cpp` already lives under `engine/generated/` and is glob-linked by engine CMake. Registry only helps **mechanic_plan** discover reuse via `as_planner_text()`.

### When it runs

After successful `cmake_build` OR `quest_build` — same promotion logic both targets.

### SSE

`info` per newly promoted id, `succeeded` with count message

See [`docs/engine-growth.md`](../../../engine-growth.md).

### Catalog consumption

Next run's as_planner_text() reads registry.json and renders promoted.* entries in ENGINE CATALOG section of mechanic planner prompt.

---

## Extend this folder

- `artifacts.md` — registry item schema
- `concepts-reuse-first.md` — planner + catalog interaction

## See also

- [`docs/engine-growth.md`](../../../engine-growth.md) — authoritative reference
- Stage 4 [`04_mechanic_plan`](../04_mechanic_plan/README.md)
