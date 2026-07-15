# Multi-agent code generation (Stage 8)

This document describes how the Code Generation stage of the
Prompt-To-Native-VR orchestrator produces native C++ mechanics from a
`MechanicPlan`. It is the authoritative reference for the per-project
ledger, the OpenAI Agents SDK driver, the multi-file write contract,
and the four-step CodeTask agent process.

The stage lives in
[`apps/orchestrator/ptv_orchestrator/pipeline/stage_code_generation.py`](../apps/orchestrator/ptv_orchestrator/pipeline/stage_code_generation.py)
and is supported by:

| Module | Purpose |
|--------|---------|
| `pipeline/codegen_board.py` | The per-project ledger (`codegen_board.json`), atomic claim, dependency DAG |
| `pipeline/codegen_agent.py` | The OpenAI Agents SDK wrapper used for each claimed `CodeTask` |
| `pipeline/prompts.py` | `CODE_TASK_SYSTEM` and `MECHANIC_PLAN_SYSTEM` prompts |
| `pipeline/bounds.py` | Per-file safety: `validate_includes`, `validate_header_content`, `filter_cpp_only`, `repair_generated_cpp`, `assert_path_allowed` |
| `packages/schemas/ptv_schemas/code_task.py` | `CodeTask` schema, including `depends_on` |

## High-level architecture

```text
Stage 7  ┃ scene_compile / scene_spec*.json on disk
Stage 8  ┃ THIS STAGE
         │
         │ 1. Load MechanicPlan + every scene_spec*.json + BuildSpec
         │ 2. CodegenBoard.load_or_init(project_dir, plan, max_agents=N)
         │      • One row per generate-class CodeTask
         │      • status = "pending" if depends_on == []  else "blocked"
         │ 3. ThreadPoolExecutor(max_workers=N)
         │    each worker runs the loop:
         │      a. board.refresh_blocked()                 (promote done deps)
         │      b. row = board.atomic_claim_next_pending(agent_id)
         │      c. if row is None: exit loop
         │      d. run_codegen_agent(row, ...)             (one Runner.run_sync)
         │      e. validate + write every row.result.files[] entry
         │      f. board.mark_done(...) OR board.mark_failed(...)
         │      g. goto a
         │ 4. Stage result: produced paths, board counts, sidecars
Stage 9  ┃ scene_compile → CMake build
```

Workers are pull-based: there is **no** static "worker-0 owns tasks
0..3" partition. Three workers compete for the next eligible
`pending` row on every loop iteration. The board is the single source
of truth for ordering and concurrency.

## The four-step CodeTask agent process (mandatory)

Every codegen run for every claimed `CodeTask` follows this exact flow.
It is enforced in two places: the `CODE_TASK_SYSTEM` prompt (agent
instructions) and the per-task user message built by
`codegen_agent.build_user_message`.

1. **Read task metadata.** Full `CodeTask` JSON (id, kind, interface,
   `depends_on`, headers, description up to 10 000 chars, planner
   `files[]` hints, asset / scene references). Read before any tool
   call.
2. **Read dependencies and ledger.** Ledger snapshot (every task with
   its status + `files_written` + `exports`) and a per-dep
   `dep_exports` map. Plug into upstream exports; never re-implement
   them.
3. **Research via web search (optional, no cap).** The hosted
   `WebSearchTool` is enabled. The agent may call it as many times as
   it judges useful. No numeric cap. Unity / Unreal / Godot / WebXR
   patterns are rejected; OpenXR / Vulkan / Quest 3 / native C++
   references are fine.
4. **Implement multi-file C++.** Decide the natural file split. Emit
   one entry per file in the structured `files[]` (1..16 entries).
   Files may be `.cpp`, `.h`, or `.hpp`. Single-file output is a
   fallback, not the default. Paths must live under
   `engine/generated/{mechanics|ai|dialogue}/<project_id>/`.

If any step is skipped (e.g. the agent dumps everything into a single
`.cpp` when the mechanic clearly separates), the implementation is
incomplete.

## `codegen_board.json` ledger

Path: `projects/<project_id>/codegen_board.json`.

```json
{
  "version": 1,
  "max_agents": 3,
  "task_order": ["shooting_hub", "repulsor_blast"],
  "tasks": {
    "shooting_hub": {
      "mechanic_plan_item_id": "shooting_hub",
      "status": "done",
      "depends_on": [],
      "agent_id": "worker-1",
      "claimed_at": "2026-05-29T18:01:32.413+00:00",
      "completed_at": "2026-05-29T18:03:12.092+00:00",
      "files_written": [
        "engine/generated/mechanics/proj_abc/shooting_hub_register.cpp",
        "engine/generated/mechanics/proj_abc/shooting_hub_simulation.cpp",
        "engine/generated/mechanics/proj_abc/shooting_hub_profile.cpp"
      ],
      "exports": {
        "registered_mechanic_id": "generated.shooting_hub",
        "weapon_profile_macro": "PTV_REGISTER_WEAPON_PROFILE"
      },
      "error": null
    },
    "repulsor_blast": {
      "mechanic_plan_item_id": "repulsor_blast",
      "status": "in_progress",
      "depends_on": ["shooting_hub"],
      "agent_id": "worker-2",
      "claimed_at": "2026-05-29T18:03:12.114+00:00",
      "completed_at": null,
      "files_written": [],
      "exports": {},
      "error": null
    }
  }
}
```

### Status machine

```text
              ┌─────────────┐
              │   blocked   │  ◄── deps not all done
              └────┬────────┘
                   │ refresh_blocked() (deps all done)
                   ▼
              ┌─────────────┐
              │   pending   │  ◄── claimable (FCFS by task_order)
              └────┬────────┘
                   │ atomic_claim_next_pending(agent_id)
                   ▼
              ┌─────────────┐
              │ in_progress │
              └────┬───┬────┘
                   │   │
   mark_done()     │   │     mark_failed()
                   ▼   ▼
              ┌────┐ ┌────────┐
              │done│ │ failed │
              └────┘ └────────┘
```

* Transitions:
  - `blocked → pending` when every dep id is `done`.
  - `pending → in_progress` on a successful `atomic_claim_next_pending`.
  - `in_progress → done` on success; `→ failed` on exception. Tasks
    never get stuck in `in_progress` on a clean run.
* **Failed tasks do not promote dependents.** A `failed` upstream
  leaves its downstream `blocked`. The stage as a whole records a
  failure to the project state when the run completes.

### Atomic claim

`atomic_claim_next_pending(agent_id)` is the only mutation called by
multiple workers concurrently:

1. Acquires an in-process `threading.Lock`.
2. Re-runs `refresh_blocked()` under the lock so a worker that just
   marked its task `done` sees freshly-promoted downstream tasks on
   its next call.
3. Walks `task_order` (which is the MechanicPlan item order — FCFS).
4. Picks the first row with `status == "pending"` whose deps are all
   `done`.
5. Flips it to `in_progress`, stamps `agent_id` and `claimed_at`,
   persists with `temp file + os.replace` (atomic on Windows + POSIX),
   returns a deep copy of the row.

The in-process lock is correct because the stage runs all workers in a
single Python process via `ThreadPoolExecutor`. A multi-process
orchestrator would need an OS-level file lock here.

### Resume

`CodegenBoard.load_or_init` on an existing board:

* Loads the JSON (corrupt files are rebuilt from the plan).
* Merges in any new tasks from the plan; refreshes `depends_on` on
  non-terminal rows; never demotes a `done` / `failed` /
  `in_progress` row.
* Calls `refresh_blocked()` so blocked rows whose deps are now done
  jump straight to `pending`.

The stage additionally calls `board.reset_stuck_in_progress()` at
start-up: any row left `in_progress` by a previous crashed run is
reset to `pending` (or `blocked` if its deps are no longer satisfied)
so a fresh worker can claim it again.

## OpenAI Agents SDK driver

`pipeline/codegen_agent.py` wraps the [OpenAI Agents SDK]
(<https://openai.github.io/openai-agents-python/>) (Python package
`openai-agents`, import path `agents`). Verified against the official
docs in April / May 2026 with version `0.17.x`.

For each claimed task the worker calls:

```python
result = run_codegen_agent(inp, model=settings.openai_hard_model)
```

Internally:

```python
from agents import Agent, Runner, WebSearchTool

agent = Agent(
    name=f"ptv-codegen-{task.id}",
    instructions=CODE_TASK_SYSTEM,
    model="gpt-5",                          # settings.openai_hard_model
    tools=[WebSearchTool()],                # hosted; NO numeric cap on calls
    output_type=CodegenAgentResult,         # Pydantic structured output
)
run_result = Runner.run_sync(agent, build_user_message(inp))
return run_result.final_output_as(CodegenAgentResult)
```

### Web search cap policy

There is **no numeric cap** on web-search calls per `CodeTask`. The
cap is implicit in the agent's own loop budget. The prompt rejects
Unity / Unreal / Godot / WebXR / browser-VR snippets surfaced by
search.

### Three concurrent agents

`CODEGEN_MAX_PARALLEL_AGENTS=3` means:

* Up to **three CodeTasks** may be `in_progress` at the same time.
* Each worker runs **one `Runner.run_sync`** session per claimed
  CodeTask, then returns to the queue and claims the next eligible
  pending task.
* Each agent run may make an **unbounded** number of tool calls inside
  its own loop (e.g. multiple web searches).

It is **not** "one SDK session for the whole project", and it is
**not** "three files in parallel inside one task" unless the agent's
own loop chooses that.

### Test injection

Production code calls `run_codegen_agent(inp, model=...)`. Tests pass
`run_codegen_agent(inp, runner=my_stub)` to bypass the SDK entirely.
The stage's `stage_code_generation.run_with_runner(ctx,
agent_runner=stub)` lets the whole stage be exercised without any
network call (used by every test in
`apps/orchestrator/tests/test_code_generation_stage.py`).

## Multi-file output (`CodeTask.files[]`)

`CodeTask.files` is the sole disk-write contract. Files may be
`.cpp`, `.h`, or `.hpp`. After the agent returns:

1. The orchestrator validates `1 <= len(files) <= 16`.
2. Collects the set of `.h`/`.hpp` filenames from the result (the
   task's "local headers").
3. For each `files[*]` entry:
   * Normalises the path to
     `engine/generated/<kind>/<project_id>/<name>`.
   * Runs `validate_includes(code, local_headers=...)` — if any include
     is disallowed, the task **fails with a clear error** (fail-loudly;
     includes are never silently stripped).
   * For `.h`/`.hpp` files, runs `validate_header_content` to reject
     registration macros in headers.
   * Runs `filter_cpp_only` → `repair_generated_cpp` (secondary
     safety-net).
   * Runs `assert_path_allowed(abs_path, gen_root)` (boundary check).
4. Writes all validated files to disk (UTF-8, trailing newline
   normalised). Appends each path to the board row's `files_written`.
5. Writes a `.codetask.json` sidecar populated with the final
   `files[]` content; the promotion stage reads it.
6. Calls `board.mark_done(task_id, files_written, exports)`.

### File-split guidance

The agent decides the actual split per mechanic. Common patterns:

| Concern | Example file |
|---------|-------------|
| Registration + metadata | `<id>_register.cpp` |
| Per-tick simulation | `<id>_simulation.cpp` |
| Shared types / helpers header | `<id>_shared.h` |
| Weapon profile / config | `<id>_profile.cpp` |
| VFX hookup | `<id>_vfx.cpp` |
| Helpers private to the mechanic | `<id>_math.cpp` |

The agent is **not** forced to use these specific names — it picks
whatever matches the mechanic. The prompt explicitly states that
**single-file output is a fallback, not the default**.

### Generated headers (same-task, same-directory)

Agents may emit `.h`/`.hpp` header files in their `files[]` output.
These headers are for sharing structs, enums, constants, and helper
declarations across the `.cpp` files of a single CodeTask.

**Security model:**

* Headers must live in the same directory as the task's `.cpp` files
  (`engine/generated/<kind>/<project_id>/`).
* Include with `#include "filename.h"` (quotes, bare filename, no
  path separators).
* Only headers produced by the same task are whitelisted. An
  `#include "other_task_header.h"` is rejected if `other_task_header.h`
  is not in the same task's `files[]`.
* Registration macros (`PTV_REGISTER_MECHANIC`, etc.) must NOT appear
  in header files — they belong in `.cpp` only.
* Headers must not define new `ptv::` engine API types.
* Cross-project header includes are blocked (no `../other_project/`
  paths).

### `test_files[]` is unused

`CodeTask.test_files[]` remains in the schema for future work but is
intentionally ignored: no disk writes, no CMake test targets, no
planner prompt language. The mechanic planner is instructed to leave
the field empty.

## `depends_on` (dependency DAG)

`CodeTask.depends_on: List[IdStr]` lists other CodeTasks in the same
plan that this task depends on. Examples:

* A weapon profile (`repulsor_blast`) depends on the shooting-hub
  task: `depends_on: ["shooting_hub"]`.
* A helper module shared by two behaviours becomes a dep of both.

Rules:

* The board ignores deps that don't exist as code_tasks in the same
  plan (a planner mis-targeting a `builtin.*` id is a no-op).
* Self-references are dropped.
* Cycles cannot make progress — affected tasks stay blocked and the
  stage finishes with a failure.

`MECHANIC_PLAN_SYSTEM` instructs the planner to set `depends_on`
whenever one generated mechanic builds on another.

## Settings + env

```env
# How many OpenAI Agents SDK sessions may run at once.
CODEGEN_MAX_PARALLEL_AGENTS=3

# Required for the codegen stage: OpenAI Agents SDK uses this key.
OPENAI_API_KEY=sk-...

# Model the SDK targets (any chat-capable OpenAI model).
OPENAI_HARD_MODEL=gpt-5
```

Settings live in `apps/orchestrator/ptv_orchestrator/settings.py`.

## Reference workflows (illustrative only)

The architecture described above is general. Two workflows the
platform must support — neither implemented in this PR — illustrate
the shape of typical games:

### Iron-Man-style Quest 3 game (one scene, two assets)

* MechanicPlan items: locomotion (`builtin`), snap turn (`builtin`),
  grab (`builtin`), equip (`builtin+config`), shooting hub
  (`generate`), repulsor blast
  (`generate`, `depends_on: ["shooting_hub"]`).
* Board flow: `shooting_hub` starts `pending`; `repulsor_blast`
  starts `blocked`. Whichever worker wins the atomic claim runs the
  hub; when it marks done, the next loop promotes
  `repulsor_blast → pending` and the next claim picks it up. Other
  workers see no eligible task and exit cleanly.
* No worker dead-spins on a specific dependency; no agent-to-agent
  chat exists; the board is the single source of truth.

### Multi-weapon shooter (one hub, N weapon profiles)

* One `shooting_hub` task, one `weapon_profile_<name>` task per
  weapon, all with `depends_on: ["shooting_hub"]`.
* Once the hub completes, every weapon profile becomes claimable
  and three of them run in parallel.

The Equip / Shooting-Hub / Repulsor mechanics themselves are **not**
implemented in this PR — only the architecture that lets a future
real prompt add them.

## Live input bridge (generated mechanics react to real controllers)

`IMechanic::on_update` receives a read-only `const InputSnapshot& input`
(see `engine/runtime/include/ptv/api/InputSnapshot.h`) with this frame's
controller + headset state — triggers, grips, thumbsticks, A/B/X/Y
buttons, hand/aim/head poses (in world space), and which entity each
hand is holding. `XrRuntime::sync_input` fills the raw state, and
`Application.cpp` composes it into the `InputSnapshot` (rig-applied
poses + grab/held state) before calling
`mechanics_.update(world, dt, snapshot)`. So generated mechanics can
respond to real input in-headset — this is no longer a stub.

Controller **bindings** are decided in the MechanicPlan stage and
materialized into each mechanic's `params` at scene compile. A generated
mechanic reads `params.value("control", ...)` (`trigger|grip|thumbstick|
a|b|x|y`) and `params.value("hand", ...)` (`left|right|either`) at
`on_attach`, then checks the live `InputSnapshot` in `on_update`. Do NOT
fake input through boolean params any more.

Held-weapon / GLB-asset mechanics use `input.hand_holding(entity)` to
detect which hand holds them and read that hand's trigger; spawn
projectiles/VFX from a `muzzle_offset` (local Vec3 in params) transformed
by the holding hand's aim pose.

| Component | Status today |
|-----------|--------------|
| Grab + tag `grabbable` | Works (tag-driven `Application.cpp` loop) |
| `builtin.grab` | Registers `CGrabbable`; grab logic lives in `Application.cpp` |
| Live input into `on_update` | **Works** — `InputSnapshot` passed every frame |
| Shooting hub + live trigger | Works — read the held hand's trigger from `InputSnapshot` |
| **Equip on body slots** | Stub — generated code should not assume it works |

## Classifications (MechanicPlan)

The MechanicPlan stage (now an OpenAI Agents SDK agent, `gpt-5.5` +
`WebSearchTool`) classifies each mechanic:

* `builtin` — a catalog mechanic satisfies as-is.
* `builtin+config` — satisfied by parameter tuning only.
* `builtin+config_code_edit` — a catalog mechanic is the right foundation
  but params cannot express the prompt; the code stage injects the base
  built-in's source (`base_builtin_id`) so the agent copies-and-edits it
  into a NEW `generated.<id>` mechanic (never replaces the built-in).
* `generate` — a brand-new mechanic authored from scratch.

Both `builtin+config_code_edit` and `generate` produce a `code_task` and
become codegen-board rows.

## Engine growth loop

After a successful build, `stage_engine_promotion` indexes generated
mechanics into `engine/promoted/registry.json`. The next prompt's
planner sees them in the catalog as `promoted.<id>` and may classify
them as `builtin` for that next game — generating only the truly new
logic on top. The same `depends_on` / DAG architecture handles
cross-game reuse: yesterday's `generate` becomes today's already-`done`
dependency.

The promotion stage relies on each task's:

* `files_written` (final disk paths) — from the board.
* `exports` (registered ids, macros, public symbols) — from the
  board.
* `.codetask.json` sidecar (final `files[]` content) — from the
  stage.

## SSE events

The stage emits the following events on the project bus
(visible in the web UI):

| Event | Meaning |
|-------|---------|
| `board_init` | The codegen board is initialised; carries `pending` / `blocked` / `done` / `failed` counts and `max_agents` |
| `task_claimed` | A worker just atomically claimed a task |
| `task_unblocked` | A blocked task was promoted to pending |
| `task_completed` | A task finished successfully; carries file count and promoted-dep ids |
| `task_failed` | A task failed; carries the truncated error |
| `info` | Auto-repair notes, path normalisation, etc. |
