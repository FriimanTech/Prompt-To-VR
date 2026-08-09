 I want you to make the following changes to my code that I’m going to list below, and as you go about fixing each problem I want you to do the following: A. Make sure while you fix each problem below to focus solely on that problem and after fixing it check to make sure it’s done correctly before moving on to the next problem(Please research the web on for each problem to find the best ways to effectively solve the problem while making all the mechanics work with good quality) B. When moving on to the next problem keep in mind all the code changes you made for the previous problems as that may affect how you approach the new problem due to the updated codebase. C. Please research all the documentation you can to make sure you implement everything correctly with the best quality. Additionally, for some problems I may provide you some links to resources I want you to definitely look at to help solve the issue, but I don’t want you to only rely on me to provide you resources before searching the web. That being said, without messing up any of the code in the codebase I want you to do the following: I want to edit the VR engine to change up how we create the code for all the mechanics needed for the VR Game. Summary: There are two porblems that need to be solved. 1. The use of just one LLM for creating the coding mechanics for the VR Game. 2. The issue that the LLM only writes a maximum of one C++ file per CodeTask when creating code for a mechanic.


# Task: Multi-agent Code Generation stage with work queue, ledger, atomic claim, and dependency DAG

## Context

Project: **Prompt-To-VR** (`C:\Users\micha\Documents\Prompt-To-VR`)

Today, `apps/orchestrator/ptv_orchestrator/pipeline/stage_code_generation.py`:
- Loops **sequentially** over every `MechanicPlanItem` that has a `code_task`
- Makes **one** LLM call per task
- Writes **one** file: `engine/generated/{mechanics|ai|dialogue}/{project_id}/{task_id}.cpp`
- Ignores `CodeTask.files[]` (multi-file codegen is out of scope for this task)
- Ignores `CodeTask.test_files[]` — **keep ignoring test files; no test runner, no writes, no schema changes for tests**
- Has **no** parallel workers, **no** task coordination, **no** dependency ordering

Read before coding:
- `stage_code_generation.py`, `pipeline/llm.py`, `pipeline/bounds.py`
- `packages/schemas/ptv_schemas/code_task.py`, `mechanic_plan.py`
- `apps/orchestrator/ptv_orchestrator/settings.py`
- `apps/orchestrator/ptv_orchestrator/pipeline/orchestrator.py` (stage order: `scene_spec` runs **before** `code_generation`)
- `docs/prompt-schema.md` (update docs in the same change per repo rules)

**Do NOT** add a second FastAPI orchestrator service. Extend the **existing** `stage_code_generation` stage only.

**LLM for this stage:** OpenAI / ChatGPT path only for the new parallel workers (use existing `LlmClient` / `OpenAIClient` or OpenAI Agents SDK if you add it). Do **not** change Claude-first/fallback behavior for other pipeline stages unless required for shared helpers.

---

## Goals (must all be delivered in this change)

### G1 — Configurable parallel workers (40.1)
- Add setting: `codegen_max_parallel_agents: int = 3` in `Settings` (env override e.g. `CODEGEN_MAX_PARALLEL_AGENTS`)
- Code generation runs up to **N workers concurrently** (N = that setting), each worker processes **one CodeTask at a time** until the queue is empty

### G2 — FCFS work queue with shared ledger (40.1)
- Implement a **project-local ledger** (shared awareness), not agent-to-agent chat
- Path: `projects/<project_id>/codegen_board.json`
- All workers **read** the ledger before claim and **write** on claim / progress / done / failed
- Use **atomic claim** so two workers never take the same `task_id` (file lock under `projects/<project_id>/` or `threading.Lock` in-process plus durable JSON writes)

### G3 — Dependency DAG and blocked tasks (40.3) — REQUIRED, not optional
- Each codegen task may declare **`depends_on`: list of task ids** (other CodeTask ids in the same project)
- Ledger task states must include at minimum:
  - `pending` — dependencies satisfied, eligible to be claimed
  - `blocked` — waiting on one or more dependencies not yet `done`
  - `in_progress` — claimed by a worker (`agent_id` set)
  - `done` — finished successfully (`files_written` populated)
  - `failed` — error recorded; optional retry policy (document behavior)
- **Workers may claim ONLY `pending` tasks** where **every** id in `depends_on` is `done`
- **Never** claim `blocked`, `in_progress`, or `done` tasks
- When dependency task A transitions to `done`, re-evaluate all `blocked` tasks; any whose deps are all `done` become `pending`

### G4 — FCFS among eligible tasks
- Among tasks in `pending` state, workers claim in **first-come-first-served** order (stable queue order from MechanicPlan / topological wave)
- Do **not** statically partition tasks by agent index (no “agent 1 gets tasks 0–3”)

### G5 — Observability
- Emit orchestrator SSE events via `ctx.emit(ProjectStage.code_generation, ...)` when:
  - board initialized
  - task claimed (`agent_id`, `task_id`)
  - task unblocked (`blocked` → `pending`)
  - task completed / failed
- Include summary in `project_state` stage record message (counts: done / failed / blocked)

### G6 — Preserve existing safety nets
- Keep `filter_cpp_only`, `repair_generated_cpp`, `assert_path_allowed` for every written `.cpp`
- Keep writing `.codetask.json` sidecar per task (update with final file list)
- Reuse per-project lock pattern from `orchestrator.py` where appropriate so codegen + cmake do not race

---

## Architecture (implement exactly this pattern)

```text
stage_code_generation.run(ctx)
  │
  ├─ Load MechanicPlan → extract all items with code_task != null
  ├─ Load SceneSpec(s) if present (scene_spec runs before codegen) for entity/asset context
  ├─ CodegenBoard.init(plan, scene_context) → create/update codegen_board.json
  │     • Build task entries with depends_on (see schema below)
  │     • Initial status: pending OR blocked per DAG
  │
  ├─ CodegenBoard.refresh_blocked()  # promote blocked→pending when deps done
  │
  ├─ Run thread pool (max_workers = settings.codegen_max_parallel_agents)
  │     each worker loop:
  │       1. board.refresh_blocked()
  │       2. task = board.atomic_claim_next_pending(agent_id)  # FCFS, returns None if empty
  │       3. if no task: exit loop
  │       4. run_codegen_for_task(task)  # existing single-file LLM path OK for v1
  │       5. board.mark_done(task_id, files_written) or mark_failed(...)
  │       6. goto 1
  │
  ├─ Wait for all workers
  ├─ If any failed: record stage status accordingly (succeeded with warnings vs failed — pick one policy and document)
  └─ Return StageResult with artifact paths