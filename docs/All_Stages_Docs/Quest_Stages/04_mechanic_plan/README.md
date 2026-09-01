> **Shared content:** Identical to [PC_VR_Stages/04_mechanic_plan](../../PC_VR_Stages/04_mechanic_plan/README.md). Update both copies when this stage changes.

# Stage 4: MechanicPlan

> **Pipeline position:** after `npc_plan` → before `scene_spec`  
> **Target:** shared (PC VR and Quest)  
> **Primary code:** [`apps/orchestrator/ptv_orchestrator/pipeline/stage_mechanic_plan.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/stage_mechanic_plan.py)  
> **Primary artifacts:** `projects/<project_id>/mechanic_plan.json`

## Main stage summary

### What this stage is

The **MechanicPlan stage** decides how every gameplay mechanic in the BuildSpec will be implemented: reuse a built-in engine module, tune its parameters, copy-and-edit a built-in's C++ source, or generate brand-new C++ from scratch. It also assigns VR **controller bindings** (which button on which hand) and emits nested **CodeTask** objects for anything that needs code.

### Why it exists

Your engine already ships reusable VR mechanics (teleport, grab, slide locomotion, etc.). Regenerating them every game wastes time and breaks determinism. This stage is **reuse-first**: try the catalog before generating.

It also creates the **dependency graph** (`depends_on`) that stage 12 (code generation) uses to order AI workers.

### Inputs (what enters)

| Input | Location |
|-------|----------|
| BuildSpec | `build_spec.json` |
| AssetManifest | `asset_manifest.json` |
| NpcPlan | `npc_plan.json` |
| Engine catalog | `engine_catalog.py` (built-ins + `promoted.*` from past games) |

### Outputs (what it produces)

| Output | Location |
|--------|----------|
| MechanicPlan | `projects/<id>/mechanic_plan.json` |

Each mechanic entry has a **classification**, optional config params, optional `code_task`, and a **binding** for input-driven mechanics.

### Where output goes next

- **`scene_spec`** — mechanic ids and bindings must match
- **`code_generation`** — runs every `code_task` in dependency order
- **`scene_compile`** — materializes bindings into `scene.json`

### Key concepts and terms

- **Four-way classification:**
  - `builtin` — catalog entry works as-is
  - `builtin+config` — same module, different parameters
  - `builtin+config_code_edit` — copy built-in C++ and edit into a **new** generated file (never replaces the built-in)
  - `generate` — wholly new C++ module
- **CodeTask** — A unit of codegen work: id, kind (`mechanics` / `ai` / `dialogue`), description, `depends_on[]`, file hints.
- **DAG (Directed Acyclic Graph)** — CodeTasks form a dependency graph with no cycles. Task B listing `depends_on: ["task_a"]` means B cannot start until A is `done`. Stage 12 uses this for ordering parallel AI workers.
- **Binding validator** — Deterministic post-pass: normalizes face-button hands (A/B → right, X/Y → left); sharing a control across mechanics is allowed.

### End-to-end flow (plain language)

1. Load BuildSpec, manifest, npc_plan.
2. Append live **engine catalog** text to the planner prompt.
3. Run **MechanicPlanner** (OpenAI Agents SDK, `gpt-5.5`).
4. Validate bindings with `validate_bindings()`.
5. Fail loudly on hard binding conflicts; emit advisory warnings otherwise.
6. Save `mechanic_plan.json` with reuse vs generate counts.

---

## Detailed stage deep dive

### Entry points

`run(ctx)` → `run_with_planner(ctx, planner=None)`; tests inject stub planner.

### Exact execution trace

| Step | Action |
|------|--------|
| 1 | emit + `record_stage(running)` |
| 2 | Load build_spec, manifest, npc_plan |
| 3 | `as_planner_text(repo_root)` — built-ins + promoted registry |
| 4 | Ensure `OPENAI_API_KEY` in `os.environ` |
| 5 | `MechanicPlanner.run(build_planner_message(...))` → MechanicPlan |
| 6 | `validate_bindings(plan)` — hard fail on conflicts |
| 7 | emit binding warnings as `warn` |
| 8 | If not ok: failed + raise ValueError |
| 9 | `save_mechanic_plan()` |
| 10 | emit reuse counts as `info` |
| 11 | succeed |

### Classifications and CodeTasks

| Class | CodeTask on board? |
|-------|-------------------|
| `builtin` | No |
| `builtin+config` | No |
| `builtin+config_code_edit` | Yes — copies built-in source |
| `generate` | Yes — new C++ |

`depends_on[]` lists other **codegen task ids** only; board strips invalid deps.

### Binding validator

Normalizes face-button hands (A/B right, X/Y left). Sharing controls across mechanics is OK. Hard conflicts fail the stage before save.

### Artifact I/O

Reads build_spec, manifest, npc_plan; writes `mechanic_plan.json`.

### SSE

`running` → `warn` → `info` (reuse=N, code_edit=M, generate=K) → `succeeded`|`failed`

### Settings

`mechanic_plan_model` (gpt-5.5), `mechanic_plan_reasoning_effort` (medium), `mechanic_plan_agent_max_turns`

### Hub mechanics and DAG

Hub CodeTasks (e.g. shooting_hub) emit first with empty `depends_on`; weapon/enemy tasks list hub id in deps. Board enforces order at codegen.

### editable_builtins_note

Planner message lists which built-ins support `code_edit` and source paths under `engine/runtime/src/mechanics/builtin/`.

---

## Extend this folder

- `concepts-dag.md` — CodeTask dependency graph in depth
- `artifacts.md` — MechanicPlan + CodeTask fields
- `failure-modes.md` — binding conflicts

## See also

- [`docs/engine-growth.md`](../../../engine-growth.md) — catalog + promotion
- Stage 12 [`12_code_generation`](../12_code_generation/README.md) — DAG execution
