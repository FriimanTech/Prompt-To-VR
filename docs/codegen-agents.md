# Multi-agent code generation (Stage 12)

This document describes how the Code Generation stage of the
Prompt-To-Native-VR orchestrator produces native C++ mechanics from a
`MechanicPlan`. It is the authoritative reference for the per-project
ledger, the OpenAI Agents SDK driver, the multi-file write contract,
and the four-step CodeTask agent process.

Per-stage summary: [`All_Stages_Docs/PC_VR_Stages/12_code_generation/README.md`](All_Stages_Docs/PC_VR_Stages/12_code_generation/README.md).

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
Stage 11  ┃ asset_contract.json on disk
Stage 12  ┃ world_generation → world.<scene_id>.scene.json per environment
Stage 13  ┃ THIS STAGE (code_generation)
         │
         │ 1. Load MechanicPlan + every scene_spec*.json + BuildSpec
         │    + every world.*.scene.json (the declared environment ids)
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
Stage 14  ┃ asset_optimization
Stage 15  ┃ scene_compile → cmake_build | quest_build
```

`world_generation` runs **before** this stage. Gameplay code acts on the
environments: a mechanic that opens a door or waits on a lever has to name the
thing it acts on, and those names are invented by the world architects at
generation time. Writing C++ first meant guessing them — and a guessed id is not
a compile error, it resolves to nothing at runtime and the mechanic silently
does nothing.

So every agent's brief carries a **GENERATED ENVIRONMENTS** block
(`_load_world_context`) listing, per environment, the interactive ids with their
kinds and **state keys**, the spawn points, the portals and the nav regions. The
keys matter as much as the ids: an agent that assumes `is_open` instead of `open`
writes a read that always returns the default. The block also states the two
ownership rules that follow from it — read and change object state through
`ctx.interactive` and never by writing `CTransform` (the affordance built-in owns
that motion, and a second writer fights it every frame); and use
`ctx.persistence` only for facts belonging to no object, because object state
already persists on its own.

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

### Socket-contract invalidation

"Never demote a `done` row" is what makes resume cheap, and it is also
what made a fix to `asset_contract` invisible: every task was `done`, so
re-running the stage generated nothing and the same socket-less C++ went
to the compiler.

`board.invalidate_on_socket_change(sockets_by_asset, repo_root)` closes
that hole without giving up idempotency. It stores a sha256 fingerprint
of the asset → socket-names map in the ledger. When the fingerprint is
unchanged it does nothing at all. When it moves, it resets exactly two
groups:

1. Tasks that **read sockets** — the CodeTask declares `required_sockets`,
   or a written source file mentions `ctx.sockets`, `socket_world` or
   `has_socket`. A file that cannot be read counts as a reader; "unknown"
   is not "unaffected".
2. The **transitive dependents** of those tasks, which consume their
   exports.

Reset rows go to `pending`, or `blocked` when their dependencies are no
longer `done`. The stage emits an `info` event listing the task ids, so
the re-billing is visible rather than inferred. The same
`_sockets_by_asset()` helper feeds both the fingerprint and the agent
context, so the thing being hashed is the thing the agent saw.

### Engine-contract checks and invalidation

`codegen_contracts.py` checks what the C++ DOES with the headers `bounds.py`
let it include -- code that compiles, links, ships and silently does nothing:

- an AI registered with `PTV_REGISTER_BEHAVIOUR` that only implements the
  context-free `on_tick(world, entity, dt)` (no navigation, perception or
  combat -- it cannot even find the real player);
- `world.find("player_rig")` (or `"__player"`): the player is
  `ctx.hands->player_entity()`;
- a private damage channel (`struct *HitInbox` / `*DamageQueue` / `*Mailbox`):
  built-in enemies and other tasks damage through `ctx.combat`, and hits are
  read with `ctx.combat->damage_seq` / `damage_events_since`;
- a private health pool: a `current_health` the task subtracts damage from
  while never reading `ctx.combat` (a display copy is fine). The engine's
  health is the health; mirror `ctx.combat->health` / `max_health`, defeat is
  `!is_alive`. (Caught after a regenerated player-health mechanic swapped its
  rejected `MeleeHitInbox` for an `apply_melee_hit()` nobody called.)
- in a project with a `game_design.json` (contract v4): a private lives
  counter, a private `*respawn*` function that teleports the player, or a
  generated `restart_current` / `revive`. The loop is `builtin.game_flow`;
  generated code reads `ctx.game` and reports objectives with
  `add_progress` / `complete` / `fail`.
- (contract v5) a `ctx.physics->raycast` from a `"muzzle"` socket: the ray
  hits the weapon's own collider first (the humans-vs-aliens pistol never
  hit anything). Use `raycast_ignoring`, or read `ctx.weapons->hits_since`.
- in a project whose `interaction_design.json` hands weapons to the engine
  (contract v5): firing from a muzzle on the trigger, a private ammo /
  magazine count, or a private projectile / bullet / tracer type. Weapons
  are `builtin.weapon` / `bow` / `melee_weapon` / `shield`; generated code
  reacts through `ctx.weapons`, and abilities that launch something use
  `ctx.projectiles` / `ctx.vfx`. See [weapons.md](weapons.md).

The weapon reconcile (`weapon_plan.reconcile_weapon_items`) also runs at the
start of code generation: shooting hubs, fire profiles, ammo / reload,
recoil and projectile tasks are removed (named) in favour of the weapon
built-ins, and tasks that depended on them are told to read `ctx.weapons`.

The game-loop reconcile (`reconcile_game_flow_items`, see
[game-flow.md](game-flow.md#2-planning-and-codegen)) also runs at the start of
code generation: a run flow, lives, respawn, player-health or end-screen
task in the plan becomes `builtin.game_flow`, and a plain health / lives /
timer HUD becomes `builtin.game_hud`, before any agent is started.

A violation raises `CodegenContractError` from `_write_files`; the task gets
ONE repair round with the violations and its own files as `repair_context`
before it fails. The same rules re-check finished tasks whenever
`API_CONTRACT_VERSION` moves (`board.invalidate_on_api_contract_change`):
stale tasks and their transitive dependents go back to `pending`, each with
an `info` event naming why. After the workers finish, `find_dead_contracts`
warns about entry points a task's header offers (`submit_*`, `push_*`,
`report_*`, ...) that no other task calls.

The coverage reconcile (`reconcile_npc_ai_items`, see
[npc-and-ai.md](npc-and-ai.md#built-in-npc-behaviours)) runs at the start of
the stage too: an AI item a built-in NPC behaviour fully covers becomes
`builtin+config`, and the board drops its task and deletes its files.

## OpenAI Agents SDK driver

`pipeline/codegen_agent.py` wraps the [OpenAI Agents SDK]
(<https://openai.github.io/openai-agents-python/>) (Python package
`openai-agents`, import path `agents`). Verified against the official
docs in April / May 2026 with version `0.17.x`.

For each claimed task the worker calls:

```python
result = run_codegen_agent(
    inp,
    model=settings.codegen_model,
    reasoning_effort=settings.codegen_reasoning_effort,
    verbosity=settings.codegen_verbosity,
)
```

Internally:

```python
from agents import Agent, ModelSettings, Runner, WebSearchTool
from openai.types.shared import Reasoning

agent = Agent(
    name=f"ptv-codegen-{task.id}",
    instructions=CODE_TASK_SYSTEM,
    model="gpt-5.6-sol",                    # settings.codegen_model
    model_settings=ModelSettings(           # MUST be explicit -- see below
        reasoning=Reasoning(effort="high"), # settings.codegen_reasoning_effort
        verbosity="medium",
    ),
    tools=[WebSearchTool()],                # hosted; NO numeric cap on calls
    output_type=CodegenAgentResult,         # Pydantic structured output
)
run_result = Runner.run_sync(agent, build_user_message(inp))
return run_result.final_output_as(CodegenAgentResult)
```

### Why `model_settings` is not optional here

`Agent.__post_init__` back-fills model settings from the model **name** when
a caller passes none:

```python
if self.model is not None and self.model_settings == get_default_model_settings():
    self.model_settings = _initial_model_settings_for_model(self.model)
```

The SDK's table maps `gpt-5` to effort `low` but `gpt-5.6-sol` to effort
**`none`**. This stage passed no settings for most of its life, so the model
upgrade alone would have silently removed reasoning from the hardest C++
generation in the pipeline — no error, no log line, just worse code and more
compile-repair rounds. `apps/orchestrator/tests/test_model_migration.py`
asserts the constructed `Agent` really carries `high`.

Codegen also reads its **own** `codegen_model` rather than
`openai_hard_model`. That setting means "which OpenAI model do we use when
Claude is down"; codegen is not a fallback, so tuning the Claude fallback
should not silently retune C++ generation.

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
* A task's own headers (in its `files[]`) are whitelisted, plus a shared
  header owned by a task in its `depends_on` list (same directory, built
  first — e.g. a weapon profile including `shooting_hub_shared.h`). An
  `#include "other_task_header.h"` is rejected when the owning task is
  neither this task nor a declared dependency.
* Registration macros (`PTV_REGISTER_MECHANIC`, etc.) must NOT appear
  in header files — they belong in `.cpp` only.
* Headers must not define new `ptv::` engine API types.
* Cross-project header includes are blocked (no `../other_project/`
  paths).

### Standard-library include allowlist

`validate_includes` only permits a curated set of C++ standard headers,
kept in sync across `pipeline/bounds.py` (`_HEADER_WHITELIST_EXACT`),
`engine/tools/check_generated_bounds.py` (`HEADER_EXACT`), and the
`CODE_TASK_SYSTEM` prompt. The guiding principle is **pure, deterministic,
side-effect-free computation** — generated gameplay code runs
single-threaded inside the ECS main loop.

* **Allowed** (examples): `<random>` (seeded RNG for fire spread/jitter),
  `<cctype>`/`<cwctype>`/`<charconv>`/`<sstream>`/`<iomanip>` (in-memory
  text formatting for HUD labels), the container and utility headers
  (`<vector>`, `<array>`, `<deque>`, `<optional>`, `<algorithm>`, …),
  math (`<cmath>`, `<numeric>`, `<bit>`, `<limits>`), and error handling
  (`<cassert>`, `<stdexcept>`, `<exception>`).
* **Forbidden** (documented in `_FORBIDDEN_STD_HEADERS`): console/file I/O
  (`<iostream>`, `<fstream>`, `<cstdio>`), filesystem (`<filesystem>`),
  threading/atomics (`<thread>`, `<mutex>`, `<atomic>`, `<future>`, …),
  wall-clock time (`<ctime>` — use the `dt` argument), and heavy /
  locale-dependent headers (`<regex>`, `<locale>`). These break
  determinism or the single-threaded execution model and are rejected
  fail-loudly.

### Multi-file correctness rules

`CODE_TASK_SYSTEM` enforces the C++ rules whose violation most often
broke the `cmake_build` stage:

* A class used from more than one `.cpp` must be **fully defined in a
  shared `.h`** (all data members + method declarations); its members are
  otherwise undeclared in the other translation unit. Never split one
  class across `.cpp` files.
* Free helper functions shared across `.cpp` are declared in the shared
  header with the **exact signature** used at every call site and defined
  in exactly one `.cpp`; `.cpp`-local helpers are `static`.
* `enum class` values must be `static_cast<int>` before integer use, and
  `switch`/`case` labels must be enum-qualified (`case State::Chase:`).

### `test_files[]` is unused

`CodeTask.test_files[]` remains in the schema for future work but is
intentionally ignored: no disk writes, no CMake test targets, no
planner prompt language. The mechanic planner is instructed to leave
the field empty.

## Compile verification + repair loop

Include validation proves a file only uses allowed headers — it does
**not** prove the C++ compiles or links. To stop a mechanic that passes
include validation but fails to compile/link from sinking the later
`cmake_build` stage, the code-generation stage runs a
compile-verify-and-repair loop once every task is `done` (mirrors the
world-gen compile loop).

Controlled by settings:

* `codegen_verify_compile` (default `True`) — master switch. Set
  `False` on a machine without the C++ toolchain.
* `codegen_max_repair_rounds` (default `3`) — how many
  build→repair→rebuild cycles to attempt (`0` = verify once, never
  repair).

Flow:

1. Build the `ptv_starter_room` **executable** (`cmake --build ...
   --target ptv_starter_room --clean-first`) in the same
   `builds/cmake-current` dir the final build uses, so vcpkg is shared and
   the later full build is incremental. Building the exe (not just the
   `ptv_runtime` static lib) is deliberate: a static-lib-only build never
   links, so it would miss cross-task `unresolved external symbol` errors
   (e.g. a task calling a dependency's function with a mismatched
   signature). Those only surface at exe link — exactly what breaks the
   playable build. The project's `scene.json` is not needed (the
   starter-room ships its own scene). **`--clean-first` (`clean_first=True`)
   is mandatory**: it forces every generated source to recompile so a stale
   `.obj` from a previous run (e.g. one left before a repair rewrote a
   generated file) can never mask a compile error and produce a FALSE
   GREEN. The verify must reflect the CURRENT files on disk.
2. On success, the stage finishes normally.
3. On failure, compiler/linker error lines are mapped back to the owning
   CodeTask via each board row's `files_written` (path + basename
   match, MSVC and clang/gcc formats). Linker errors that name no source
   file cannot be attributed to a single task; the stage fails loudly so
   they are surfaced rather than silently ignored.
4. Each failing task's agent is re-run with a `repair_context` (the
   compiler errors, the current file contents on disk, AND the shared
   headers + ledger exports of its `depends_on` tasks — the "dependency
   contract") so it **fixes the existing files** against the REAL
   cross-task symbol names rather than authoring from scratch. On a repair
   the agent must re-emit ALL of the task's files as one consistent set
   (`CODE_TASK_SYSTEM` rules 13-14), so a partial repair can never leave a
   task self-contradictory (header renamed but a `.cpp` not updated). The
   new files overwrite the old and `board.mark_done` refreshes the row.
5. The stage rebuilds (again clean) and repeats up to
   `codegen_max_repair_rounds`.
6. If the budget is exhausted (or errors cannot be attributed to any
   task), the still-failing tasks are marked `failed` and the stage
   raises — exactly like an agent failure — so `cmake_build` never runs
   against C++ that does not compile.

The verify also runs on an **idempotent resume**: if every task is already
`done` on disk from a previous run (so this run produced no new files), the
stage still performs one compile-verify pass before reporting success, so a
resume after an interrupted/partial repair cannot ship un-verified C++ to
`cmake_build`.

The verifier is injectable: `stage_code_generation.run_with_runner(ctx,
agent_runner=..., compile_verifier=...)` lets tests exercise the loop
with a stub build (no real toolchain). Production passes `None` and the
default cmake verifier is used when `codegen_verify_compile` is set.

## `depends_on` (dependency DAG)

`CodeTask.depends_on: List[IdStr]` lists other CodeTasks in the same
plan that this task depends on. Examples:

* A spell profile (`fireball`) depends on a casting-hub task:
  `depends_on: ["spell_hub"]`. (Weapons no longer work this way: they are
  engine built-ins, see [weapons.md](weapons.md).)
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

# Model + reasoning effort the SDK targets for THIS stage. Effort must be
# explicit: the SDK infers `none` for gpt-5.6-sol when it is omitted.
CODEGEN_MODEL=gpt-5.6-sol
CODEGEN_REASONING_EFFORT=high
```

Settings live in `apps/orchestrator/ptv_orchestrator/settings.py`.

## Reference workflows (illustrative only)

The architecture described above is general. Two workflows the
platform must support — neither implemented in this PR — illustrate
the shape of typical games:

### Wizard duel (one hub, N spell profiles)

* MechanicPlan items: locomotion (`builtin`), snap turn (`builtin`),
  grab (`builtin`), a casting hub (`generate`), one `spell_<name>` task
  per spell (`generate`, `depends_on: ["spell_hub"]`).
* Board flow: `spell_hub` starts `pending`; the spells start `blocked`.
  Whichever worker wins the atomic claim runs the hub; when it marks done,
  the next loop promotes the spells to `pending` and up to three run in
  parallel. No worker dead-spins on a dependency; no agent-to-agent chat
  exists; the board is the single source of truth.

### Shooter (no generated weapons)

* Every gun, bow, sword and shield is a `builtin+config` item of its
  weapon built-in, tuned from the interaction design. Only game-specific
  reactions are generated -- a scoring target or a custom HUD reading
  `ctx.weapons->hits_since` / `state`.

## Live input bridge (generated mechanics react to real controllers)

`IMechanic::on_update(world, entity, dt, input, ctx)` receives a read-only
`const InputSnapshot& input` (see
`engine/runtime/include/ptv/api/InputSnapshot.h`) with this frame's
controller + headset state — triggers, grips, thumbsticks, A/B/X/Y
buttons, hand/aim/head poses (in world space), smoothed hand velocity, and
which entity each hand is holding. `XrRuntime::sync_input` fills the raw
state and `Application.cpp` composes it into the `InputSnapshot`. So
generated mechanics can respond to real input in-headset — this is no
longer a stub.

It also receives `MechanicContext& ctx`
(`engine/runtime/include/ptv/api/MechanicContext.h`) exposing
`ctx.locomotion` (move the player rig), `ctx.physics` (raycast, impulse,
kinematic), `ctx.draw` (one-frame debug geometry) and `ctx.hands` (what each
hand holds). Every mutating call is validated and clamped engine-side, so
generated code can use them freely. Mechanics may also declare a
`MechanicPhase` (`Input` runs before the snapshot freezes and before
physics; `Simulate` is the default) and a `MechanicScope` (`Entity` is the
default; `Player` gives one shared instance for player-global systems).
See [`docs/input-and-bindings.md`](input-and-bindings.md).

Controller **bindings** are decided in the MechanicPlan stage and
materialized into each mechanic's `params` at scene compile. A generated
mechanic reads `params.value("control", ...)` (`trigger|grip|thumbstick|
a|b|x|y`) and `params.value("hand", ...)` (`left|right|either`) at
`on_attach`, then checks the live `InputSnapshot` in `on_update`. Do NOT
fake input through boolean params any more.

Mechanics bound to a held item use `input.hand_holding(entity)` to detect
which hand holds them. Points on the item come from its sockets
(`ctx.sockets->socket_world(self, "<name>")`, names from the interaction
design), never a hard-coded offset. Designed weapons are not generated at
all ([weapons.md](weapons.md)); a custom ability that launches something
uses `ctx.projectiles->launch` (or `preset("<projectile id>")`) and
`ctx.vfx`.

| Component | Status today |
|-----------|--------------|
| Grab + tag `grabbable` | Works. Binding `builtin.grab` to an entity ADDS the tag, so scenes no longer author both |
| `builtin.grab` | A real mechanic in `engine/runtime/src/mechanics/builtin/grab/` — copy-and-editable |
| `builtin.throw` | A real per-entity mechanic in `builtin/throw/`; grab without throw simply drops |
| Live input into `on_update` | **Works** — `InputSnapshot` passed every frame |
| Locomotion / physics from generated code | **Works** — via `MechanicContext` (clamped) |
| Weapons (guns, beams, rockets, bows, melee, shields) | Engine built-ins bound by `scene_compile` from the interaction design; generated code reads `ctx.weapons` |
| **Equip on body slots** | Stub — generated code should not assume it works |

## Classifications (MechanicPlan)

The MechanicPlan stage (an OpenAI Agents SDK agent, `gpt-5.6-sol`, no tools —
plan output only) classifies each mechanic:

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
