> **Shared content:** Identical to [PC_VR_Stages/12_code_generation](../../PC_VR_Stages/12_code_generation/README.md). Update both copies when this stage changes.

# Stage 12: C++ Code Generation

> **Pipeline position:** after `asset_contract` → before `world_generation`  
> **Target:** shared (PC VR and Quest)  
> **Primary code:** [`apps/orchestrator/ptv_orchestrator/pipeline/stage_code_generation.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/stage_code_generation.py)  
> **Primary artifacts:** `engine/generated/{mechanics|ai|dialogue}/<project_id>/*`, `codegen_board.json`

## Main stage summary

### What this stage is

The **code generation stage** turns MechanicPlan **CodeTasks** into real C++ source files under `engine/generated/`. Multiple **AI worker agents** run in parallel, coordinated by a JSON **ledger board**, with a **compile-verify repair loop** that feeds MSVC errors back until code builds or budget is exhausted.

### Why it exists

Built-in engine mechanics cover common VR interactions, but each game needs custom AI, weapons, and rules. This stage is where LLM-produced C++ lives — bounded by include whitelists, registration macros, and compiler verification so generated code cannot break the engine core.

### Inputs (what enters)

| Input | Location |
|-------|----------|
| MechanicPlan | `mechanic_plan.json` (items with `code_task`) |
| BuildSpec | `build_spec.json` |
| Scene specs | `scene_spec*.json` |
| Asset contract | `asset_contract.json` |
| Built-in source | For `code_edit` tasks via `builtin_source.py` |

### Outputs (what it produces)

| Output | Location |
|--------|----------|
| Generated C++ | `engine/generated/{mechanics|ai|dialogue}/<project_id>/*.cpp` (+ optional `.h/.hpp`) |
| Ledger | `projects/<id>/codegen_board.json` |
| Sidecars | `*.codetask.json` per task |

### Where output goes next

- **`world_generation`** — ordering only (codegen runs first)
- **`scene_compile`** — mechanics referenced in scene must exist in build
- **`cmake_build` / `quest_build`** — links generated objects into executable

### Key concepts and terms

- **DAG (Directed Acyclic Graph)** — **D**irected **A**cyclic **G**raph: a graph of tasks with arrow directions (dependencies) and **no cycles**. CodeTask B with `depends_on: ["task_a"]` cannot start until task A is marked `done`. This prevents agents from implementing code that needs symbols not yet written.
- **CodegenBoard** — `codegen_board.json` ledger: one row per CodeTask with status (`blocked`, `pending`, `in_progress`, `done`, `failed`), files written, and exports. Workers **atomically claim** the next eligible `pending` row — pull-based, not pre-assigned partitions.
- **Compile-verify loop** — After all tasks complete (or on resume), MSVC builds the project executable. Compiler errors go back to the owning task's agent for repair, up to `codegen_max_repair_rounds`.
- **IMechanic / IBehaviour / IDialogueHook** — C++ interfaces generated code must implement (in `engine/runtime/include/ptv/api/`). Registration via `PTV_REGISTER_MECHANIC("id")` macros in `.cpp` files only.
- **Include whitelist** — Generated files may only include approved headers (`ptv/api/*`, `ptv/ecs/*`, etc.). Violations fail the stage loudly.

### End-to-end flow (plain language)

1. Load MechanicPlan; collect all `generate` and `code_edit` CodeTasks.
2. Initialize CodegenBoard: tasks with empty `depends_on` → `pending`; others → `blocked`.
3. Start N worker threads (default 3). Each loop: refresh blocked → claim next pending → run one OpenAI agent → validate/write files → mark done/failed.
4. When board quiescent, run MSVC compile-verify.
5. On compile errors, repair affected tasks and rebuild until success or round limit.

---

## Detailed stage deep dive

### Architecture overview

Three cooperating layers:

1. **CodegenBoard** (`codegen_board.json`) — persistent DAG ledger + task statuses
2. **Worker pool** — `ThreadPoolExecutor(max_workers=codegen_max_parallel_agents)` pull-based claim loop
3. **Compile-verify** — real MSVC link of `ptv_starter_room` catches errors include-checks miss

### Stage entry: `run_with_runner(ctx, agent_runner=None, compile_verifier=None)`

| Phase | What happens |
|-------|----------------|
| Init | Filter mechanic_plan items with non-null `code_task`; empty → succeed immediately |
| Board | `CodegenBoard.load_or_init(project_dir, plan, max_agents)` |
| Resume hygiene | `reset_stuck_in_progress()` + `reset_failed()` → emit `info` |
| Context | `_load_static_context()`: BuildSpec, all `scene_spec*.json`, `asset_contract.json`, npc_plan |
| Workers | Each runs `_StageRuntime.worker_loop()` until no claimable tasks |
| Verify | If `codegen_verify_compile` and no hard failures: `_default_verify_compile()` loop |

### CodegenBoard state machine

Each CodeTask row:

| Field | Meaning |
|-------|---------|
| `status` | `blocked` → `pending` → `in_progress` → `done` or `failed` |
| `depends_on` | Other task ids; all must be `done` before `pending` |
| `agent_id` | Set on atomic claim |
| `files_written` | Paths agent produced |
| `exports` | Public symbols/types for downstream tasks |

**`atomic_claim_next_pending(agent_id)`** (thread-safe):

1. Hold `threading.Lock`
2. `refresh_blocked()` — promote blocked→pending when deps done
3. Pick first `pending` in MechanicPlan FCFS order
4. Flip to `in_progress`, stamp agent, persist via temp-file rename
5. Return row to worker

Two workers cannot claim the same row.

### Per-task worker: `_process_one_task`

1. Build `CodegenAgentInput` with ledger snapshot + `dep_exports` from completed deps
2. For `code_edit`: attach `base_source_block` from `read_builtin_sources(catalog_id)`
3. **`run_codegen_agent()`** — OpenAI Agents SDK, `WebSearchTool` enabled (no cap), model `codegen_model` (`gpt-5.6-sol`) with an EXPLICIT `codegen_reasoning_effort` of `high`. The effort cannot be left out: the SDK back-fills model settings from the model name and infers `none` for this model, which would silently strip reasoning from the stage.
4. Agent must follow 4-step process (metadata → deps/ledger → optional search → multi-file C++)
5. **`_write_files()`** per returned file:
   - `validate_includes()` / `validate_header_content()` (no registration macros in headers)
   - `filter_cpp_only()`, `repair_generated_cpp()`, `assert_path_allowed()`
   - Write under `engine/generated/{mechanics|ai|dialogue}/<project_id>/`
6. Write `<task_id>.codetask.json` sidecar with exports
7. `board.mark_done()` or `mark_failed()`

### Compile-verify + repair loop

After all tasks `done` (or on resume when already done):

1. `_default_verify_compile()` runs CMake build of starter-room with `PTV_GENERATED_PROJECT_ID`, linking all generated objects
2. Captures log to `logs/codegen_verify.log`
3. On failure: map compiler errors to owning tasks → `_repair_one_task()` with error text as `repair_context`
4. Repeat up to `codegen_max_repair_rounds`

Include validation alone cannot catch link errors (wrong function signature across tasks).

### SSE events (representative)

`board_init`, `task_unblocked`, `task_claimed`, `task_completed`, `task_failed`, `verify`, `verify_ok`, `verify_error`, `task_repair`, `task_repaired`, `log` (cmake lines)

### Failure modes

| Case | Result |
|------|--------|
| Include violation | Task failed before write |
| Blocked tasks remain | Stage failed RuntimeError |
| Verify exhausted | Stage failed |
| Zero CodeTasks | Immediate success |

### Artifacts

Writes: `engine/generated/**/<project_id>/*.{cpp,h,hpp}`, `*.codetask.json`, `codegen_board.json`, verify log.

See [`docs/codegen-agents.md`](../../../codegen-agents.md) for exhaustive reference.

### `codegen_board.json` example (ledger on disk)

Path: `projects/<project_id>/codegen_board.json`. One row per CodeTask:

```json
{
  "version": 1,
  "max_agents": 3,
  "task_order": ["shooting_hub", "enemy_ai"],
  "tasks": {
    "shooting_hub": {
      "status": "done",
      "depends_on": [],
      "files_written": ["engine/generated/mechanics/my_game/shooting_hub.cpp"],
      "exports": { "registered_mechanic_id": "generated.shooting_hub" }
    },
    "enemy_ai": {
      "status": "blocked",
      "depends_on": ["shooting_hub"],
      "files_written": [],
      "exports": {}
    }
  }
}
```

When `shooting_hub` becomes `done`, `refresh_blocked()` promotes `enemy_ai` to `pending`. A worker then claims it via `atomic_claim_next_pending("worker-2")`.

**Failed tasks do not unblock dependents** — downstream stays `blocked` until upstream is repaired and marked `done`.

### Status machine (transitions)

```
blocked ──(all deps done)──► pending ──(atomic claim)──► in_progress
                                                              │
                                    mark_done() ◄─────────────┤
                                    mark_failed() ◄───────────┘
```

- `blocked → pending`: automatic via `refresh_blocked()`
- `pending → in_progress`: exactly one worker wins the lock
- `in_progress → pending`: only via `reset_stuck_in_progress()` on resume after crash
- `failed → pending`: via `reset_failed()` at stage start (retry transient errors)

### Four-step agent process (mandatory per task)

Enforced in `CODE_TASK_SYSTEM` and `codegen_agent.build_user_message()`:

1. **Read CodeTask JSON** — id, kind, interface, depends_on, description, file hints, required_sockets
2. **Read ledger + dep_exports** — snapshot of every task status; per-dep exports map so agent plugs into upstream APIs instead of reimplementing
3. **Optional WebSearch** — no numeric cap; native C++/OpenXR/Quest references OK; Unity/Unreal patterns rejected
4. **Emit multi-file C++** — structured `files[]` with 1–16 entries (`.cpp`, `.h`, `.hpp`); registration macros **only in .cpp**

### Static context loaded once per stage run

`_load_static_context()` gathers:

- Full BuildSpec JSON
- Every `scene_spec*.json` on disk
- `asset_contract.json` (socket names agents may reference)
- npc_plan JSON

Passed into every agent user message so tasks know scene entities and available sockets.

### Compile-verify mapping errors → tasks

When MSVC reports `error C2065` or unresolved external:

1. Log captured in `logs/codegen_verify.log`
2. Heuristic maps error file/line to owning CodeTask (path under `engine/generated/.../<task>/`)
3. `_repair_one_task()` re-runs agent with `repair_context` containing compiler output
4. Rebuild after repair; repeat until success or `codegen_max_repair_rounds`

This catches signature mismatches between tasks that passed include validation separately.

### Link-verify rationale

Include checks miss cross-task link errors. Building ptv_starter_room forces full link of all generated translation units.

### Sidecar role

`.codetask.json` holds required_sockets, exports, promotion metadata — read by scene_compile and engine_promotion.

---

## Extend this folder

- `concepts-dag.md` — CodegenBoard state machine in depth
- `concepts-cpp-boundaries.md` — include whitelist and registration macros
- `failure-modes.md` — compile repair transcript format

## See also

- [`docs/codegen-agents.md`](../../../codegen-agents.md) — authoritative deep reference
- [`docs/npc-and-ai.md`](../../../npc-and-ai.md) — services codegen must use (not reimplement)
- Stage 4 [`04_mechanic_plan`](../04_mechanic_plan/README.md) — CodeTask origin
