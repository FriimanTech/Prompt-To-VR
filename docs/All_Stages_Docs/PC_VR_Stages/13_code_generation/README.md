# Stage 13: C++ Code Generation

> **Pipeline position:** after `world_generation` → before `asset_optimization`  
> **Target:** shared (PC VR and Quest)  
> **Primary code:** [`apps/orchestrator/ptv_orchestrator/pipeline/stage_code_generation.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/stage_code_generation.py), [`codegen_board.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/codegen_board.py), [`codegen_agent.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/codegen_agent.py), [`codegen_contracts.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/codegen_contracts.py)  
> **Primary artifacts:** `engine/generated/{mechanics|ai|dialogue}/<project_id>/*.{cpp,h,hpp}`, `<task_id>.codetask.json` sidecars, `projects/<id>/codegen_board.json`, `projects/<id>/logs/codegen_verify.log`

## Main stage summary

### What this stage is

The **code generation stage** turns the MechanicPlan's **CodeTasks** (items classified `generate` or `builtin+config_code_edit`) into real C++ under `engine/generated/`. Several OpenAI Agents SDK workers run in parallel, coordinated by a JSON **ledger board** that enforces a dependency DAG. Every file passes include / path / content validation and an **engine-contract lint** before it is written, and a **compile-verify + repair loop** then builds and links the generated code (plus, for Quest, an NDK clang syntax pass) and feeds errors back to the owning task.

### Why it exists

Built-in engine mechanics cover common VR behaviour; each game still needs some custom rules, AI and dialogue. This is the only stage where LLM-produced gameplay C++ is written, and it is bounded so the code cannot touch the engine core and cannot re-implement what the engine owns (weapons, the game loop, NPC movement, damage).

### Why it runs after `world_generation`

Mechanics that open a door, wait on a lever, use a portal or send the player to a spawn point must name ids the world architect declared. `world_generation` now runs first, and this stage hands every agent a summary of each environment's interactives, spawn points and portals, so the code references things that exist instead of guessing.

### Inputs (what enters)

| Input | Location | Used for |
|-------|----------|----------|
| MechanicPlan | `mechanic_plan.json` | CodeTasks (`code_task`), classification, base built-in for code edits; reconciled and re-saved at stage start |
| BuildSpec | `build_spec.json` | Per-task excerpt |
| Scene specs | `scene_spec*.json` | Entities each task is bound to, plus their sockets |
| Asset contract | `asset_contract.json` | `asset_id → socket names` (also fingerprinted by the board) |
| NPC plan | `npc_plan.json` (optional) | Full plan in every task's context; NPC-AI reconcile |
| Game design | `game_design.json` (optional) | Game-loop reconcile; enables game-flow contract rules |
| Interaction design | `interaction_design.json` (optional) | Weapon reconcile; enables weapon contract rules |
| Animation inventory | `asset_animation.json` + `character_routing.json` | Per character: provider, animation keys, gait variants, missing actions |
| Worlds | `world.scene.json` / `world.<scene_id>.scene.json` | Declared interactives (id, kind, state keys, locked), spawn points, portals |
| Built-in source | `engine/runtime/src/mechanics/builtin/<mechanic>/` via `builtin_source.py` | Copy-and-edit base for `builtin+config_code_edit` tasks |
| Project state | `ProjectState.target_device` | System prompt `code_task_system(target_device)`; Quest syntax check |

### Outputs (what it produces)

| Output | Location |
|--------|----------|
| Generated C++ | `engine/generated/{mechanics|ai|dialogue}/<project_id>/*.cpp` (+ optional `.h` / `.hpp`) |
| Sidecars | `engine/generated/<kind>/<project_id>/<task_id>.codetask.json` (the CodeTask with the final `files[]` contents) |
| Ledger | `projects/<id>/codegen_board.json` |
| Verify log | `projects/<id>/logs/codegen_verify.log` |
| Reconciled plan | `mechanic_plan.json` rewritten when a reconcile changed it |
| Build tree | `projects/<id>/builds/cmake-current/` (shared with `cmake_build`) |

### Where output goes next

- **`asset_optimization`** — runs after this stage so the engine build tree is warm and `ptv_glb_optimize` is an incremental target.
- **`scene_compile`** — checks every scene binding resolves to something the build registers (`UnresolvedBindingError`), drops generated NPC AI that is not hosted, validates `required_sockets` against the asset contract, and detects whether generated code calls `ctx.encounters->start()`.
- **`cmake_build` / `quest_build`** — compile and link the generated sources (selected by `PTV_GENERATED_PROJECT_ID`).
- **`engine_promotion`** — reads the sidecars.

### Models, providers and reasoning

- OpenAI Agents SDK on the **Responses API** (`OpenAIResponsesModel` with an `AsyncOpenAI` client whose timeout is `codegen_request_timeout_seconds`), model `codegen_model` = `gpt-5.6-sol`.
- `reasoning.effort` = `codegen_reasoning_effort` (`high`) passed **explicitly**: when an Agent gets no model settings the SDK back-fills them from the model name and infers `none` for `gpt-5.6-sol`, which would silently strip reasoning. Responses is also required because 5.6 only allows reasoning together with tools there.
- Tool: hosted `WebSearchTool`, no numeric cap.
- Structured output `CodegenAgentResult` (`files[]` 1–16 entries, `exports`).
- Cost: one agent run per CodeTask, plus contract-repair and compile-repair runs.

### Failure behavior

- A task whose agent raises a **transient** error (timeout, connection reset, 429 / 5xx, overloaded) is retried in-process up to `codegen_max_task_attempts`; a schema disagreement is not retried.
- Include / path / header violations fail the task before anything is written.
- Engine-contract violations get **one** repair run with the violations as context; if the rewrite still violates, the task fails.
- Compile / link errors are repaired up to `codegen_max_repair_rounds`; still failing tasks are marked `failed` (every task if the errors could not be attributed).
- The stage raises `RuntimeError` when any task is `failed` or `blocked`, so the build never compiles broken code. A plan with no CodeTasks succeeds immediately ("0 files").

---

## Detailed stage deep dive

### Stage entry: `run_with_runner(ctx, agent_runner=None, compile_verifier=None)`

| Phase | What happens |
|-------|--------------|
| Reconcile | `reconcile_npc_ai_items` (generated NPC AI a built-in fully covers → `builtin.npc.follow` / `builtin.npc.melee_chase` config), `reconcile_game_flow_items` (with a game design, lives / respawn / end screens are `builtin.game_flow`), `reconcile_weapon_items` (with an interaction design, firing / ammo / recoil / melee are `builtin.weapon` & co.). Each change re-saves the plan and is logged |
| Board | `CodegenBoard.load_or_init(project_dir, plan, max_agents, repo_root)` — drops rows (and their files) for tasks the plan no longer has; resets tasks whose CodeTask fingerprint changed, plus their transitive dependents |
| Resume hygiene | `reset_stuck_in_progress()`, `invalidate_on_socket_change()`, `invalidate_on_api_contract_change()`, `reset_failed()` — each emits an `info` line |
| Context | `_load_static_context()` once per run |
| Workers | `ThreadPoolExecutor(max_workers=codegen_max_parallel_agents)`, each running `_StageRuntime.worker_loop()` |
| Dead contracts | `find_dead_contracts()` — warns when a task's header offers `submit_*` / `push_*` / `report_*` … entry points nobody else calls |
| Verify | If nothing is failed / blocked and files exist (this run or a previous one): `verify_and_repair()` with `_verify_all_targets` |

### CodegenBoard (`codegen_board.json`)

```json
{
  "version": 1,
  "max_agents": 3,
  "task_order": ["door_puzzle", "boss_ai"],
  "socket_fingerprint": "…",
  "api_contract_version": 5,
  "tasks": {
    "door_puzzle": {
      "mechanic_plan_item_id": "door_puzzle",
      "status": "done",
      "depends_on": [],
      "required_sockets": [],
      "task_fingerprint": "…",
      "agent_id": "worker-0",
      "files_written": ["engine/generated/mechanics/my_game/door_puzzle.cpp"],
      "exports": {"registered_mechanic_id": "generated.door_puzzle"},
      "error": null
    },
    "boss_ai": {"status": "blocked", "depends_on": ["door_puzzle"], "...": "..."}
  }
}
```

- `depends_on` keeps only ids of other CodeTasks in the plan (self-references and built-in ids are dropped).
- `blocked → pending` when every dependency is `done` (`refresh_blocked()`, called at the top of every worker iteration and inside `mark_done`).
- `atomic_claim_next_pending(agent_id)` takes the lock, refreshes, claims the first `pending` row in MechanicPlan order and persists by temp-file rename. Two workers can never claim the same row.
- A failed task never unblocks its dependents.
- `in_progress → pending` only via `reset_stuck_in_progress()` (crash recovery); `failed → pending` via `reset_failed()` at stage start.
- `done → pending` when the CodeTask changed (`task_fingerprint`), the socket contract moved, or the API contract version moved and the code no longer passes — always with transitive dependents, and rows whose dependencies are no longer `done` go back to `blocked`.
- If the board cannot be saved, the in-memory board stays authoritative and the stage warns that a Resume may redo finished tasks.

### Socket-contract fingerprint (`invalidate_on_socket_change`)

The board stores a sha256 fingerprint of the `asset_id → socket names` map. When it moves, only **socket readers** are reset — tasks whose CodeTask declares `required_sockets` or whose source mentions `ctx.sockets` / `socket_world` / `has_socket` (an unreadable file counts as a reader) — plus their transitive dependents. Without this, fixing `asset_contract` and re-running code generation was a no-op.

### API contract (`codegen_contracts.py`, `API_CONTRACT_VERSION = 5`)

`lint_task_sources(kind, files, game_flow=…, weapons=…)` rejects code that compiles but cannot work:

| Rule | Applies |
|------|---------|
| `ctx.physics->raycast` from a `"muzzle"` socket (hits the weapon's own collider; use `ctx.weapons->hits_since` or `raycast_ignoring`) | always |
| Firing from the muzzle on the trigger; keeping and spending a private ammo count without reading `ctx.weapons`; a private `*Projectile*` / `*Bullet*` / `*Tracer*` type | project has engine-run weapons |
| A private lives counter; a `*respawn*` function that teleports the player; calling `restart_current` / `revive` | project has a `game_design.json` |
| `PTV_REGISTER_BEHAVIOUR` without the context-taking `on_tick(..., const InputSnapshot&, MechanicContext&)` | always |
| A private `current_health` pool that takes damage but never reads `ctx.combat` | always |
| `find("player_rig")` / `find("__player")` (the player is `ctx.hands->player_entity()`) | always |
| A private `*HitInbox` / `*DamageQueue` / `*Mailbox` damage channel | always |

When the version number moves, `invalidate_on_api_contract_change` re-lints every `done` task and regenerates exactly those that fail (with dependents).

### Static context (`_load_static_context`)

BuildSpec, every `scene_spec*.json`, `sockets_by_asset` from `asset_contract.json`, the target device, the NPC plan JSON, `animation_inventory()` and `_load_world_context()` (per environment: `scene_id`, interactives with `id`, `kind`, sorted `state_keys` and `locked`, spawn-point ids, portals with `to_scene` / `spawn_point_id`).

### Per-task worker (`_process_one_task`)

1. Build `CodegenAgentInput`: task JSON, ledger snapshot, `dep_exports_json` for its dependencies, scene context (bound entities and their sockets), BuildSpec excerpt, NPC plan, animation inventory, world context, `code_task_system(target_device)` instructions, and for code-edit tasks the built-in's source with "copy and adapt into a NEW mechanic registered under `generated.<task_id>`; do NOT replace the built-in".
2. `_run_agent_with_retry()` → `run_codegen_agent()`.
3. `_write_files()`:
   - 1–16 files; path normalised into `engine/generated/<kind>/<project_id>/`; no duplicates; `assert_path_allowed`.
   - `validate_includes` (same-task headers and headers of declared `depends_on` tasks are allowed by bare filename), `validate_header_content` for headers (no registration macros), all violations collected and raised together.
   - `filter_cpp_only` and `repair_generated_cpp` (each auto-repair is reported as `info`).
   - `lint_task_sources` → `CodegenContractError` → one repair run with the produced files as context.
   - Write files.
4. Write `<task_id>.codetask.json` with the final file contents; `mark_done` (or `mark_failed`).

### Compile-verify + repair (`verify_and_repair`)

For `round` in `0..codegen_max_repair_rounds`:

1. `_verify_all_targets()`:
   - `_default_verify_compile()` — `configure_and_build` of `engine/` into `builds/cmake-current`, target **`ptv_starter_room`** (an executable, so cross-task `unresolved external symbol` errors surface; a static-lib build never links), `clean_first=True` (no stale `.obj` false green), cache `PTV_GENERATED_PROJECT_ID` + `PTV_GENERATED_EXTRA_SOURCES` (promoted mechanics the plan reuses). Log saved to `logs/codegen_verify.log`.
   - Quest projects only, once MSVC is clean and `codegen_quest_syntax_check` is on: `quest_syntax_check()` runs the NDK's `clang++ --target=aarch64-linux-android32 -std=c++20 -fsyntax-only` on every generated `.cpp` with the Android defines used by `engine/android`, the engine include dirs and the desktop vcpkg headers (up to 8 in parallel, 300 s each). MSVC and clang disagree on real code, and without this the error only surfaced in `quest_build`. Skipped with a warning when no NDK is found (`android_ndk_home`, `ANDROID_NDK_HOME`, then `<sdk>/ndk/<android_ndk_version>`).
2. On failure, `_failing_tasks_from_log()` maps compiler lines (MSVC `file.cpp(42):`, clang `file.cpp:42:`) and MSVC linker lines (`foo.obj : error LNK2019`) to the owning task.
3. `_repair_one_task()` re-runs each failing task with its errors (≤ 60 lines), the log tail (8 000 chars), a **dependency contract** block (the real headers of its `depends_on` tasks, ≤ 16 000 chars each) and its current files (≤ 40 000 chars each).
4. Stop early when the errors cannot be attributed to any task.

### Settings (`apps/orchestrator/ptv_orchestrator/settings.py`)

Environment variables are the upper-cased field names.

| Setting | Env var | Default |
|---------|---------|---------|
| `codegen_model` | `CODEGEN_MODEL` | `gpt-5.6-sol` |
| `codegen_reasoning_effort` | `CODEGEN_REASONING_EFFORT` | `high` (validated at startup; `minimal` is rejected) |
| `codegen_verbosity` | `CODEGEN_VERBOSITY` | `medium` |
| `codegen_max_parallel_agents` | `CODEGEN_MAX_PARALLEL_AGENTS` | 3 (1–16) |
| `codegen_verify_compile` | `CODEGEN_VERIFY_COMPILE` | `true` |
| `codegen_max_repair_rounds` | `CODEGEN_MAX_REPAIR_ROUNDS` | 3 (0–10) |
| `codegen_quest_syntax_check` | `CODEGEN_QUEST_SYNTAX_CHECK` | `true` |
| `codegen_request_timeout_seconds` | `CODEGEN_REQUEST_TIMEOUT_SECONDS` | 1800 |
| `codegen_max_task_attempts` | `CODEGEN_MAX_TASK_ATTEMPTS` | 2 (1–5) |
| `android_ndk_home`, `android_sdk_root`, `android_ndk_version` | `ANDROID_NDK_HOME` … | unset, unset, `30.0.15729638` |

### SSE events

`running`, `info` (reconciles, board resets, auto-repairs, files written, retries), `board_init`, `task_unblocked`, `task_claimed`, `task_completed`, `task_failed`, `warning` (dead contracts, board save, skipped Quest check), `warn` (unparseable SceneSpec), `verify`, `verify_ok`, `verify_error`, `task_repair`, `task_repaired`, `log`, `succeeded` / `failed`.

### Boundaries (always enforced)

Generated C++ lives only under `engine/generated/{mechanics,ai,dialogue}/<project_id>/`, implements `IMechanic`, `IBehaviour` (context-taking forms) or `IDialogueHook`, registers with `PTV_REGISTER_MECHANIC` / `PTV_REGISTER_BEHAVIOUR` / `PTV_REGISTER_DIALOGUE_HOOK` in `.cpp` files only, and includes only the public `ptv/api/*` and public scene / ECS headers. `engine/tools/check_generated_bounds.py` repeats the check before the build. The CodeTask `test_files[]` field is intentionally unused.

## See also

- [`docs/codegen-agents.md`](../../../codegen-agents.md) — authoritative deep reference
- [`docs/npc-and-ai.md`](../../../npc-and-ai.md) — engine services generated AI must use
- [`docs/weapons.md`](../../../weapons.md) — what generated code may and may not do with weapons
- [`docs/game-flow.md`](../../../game-flow.md) — `ctx.game` and the game-loop contract
- Previous stage: [`12_world_generation`](../12_world_generation/README.md); CodeTask origin: [`04_mechanic_plan`](../04_mechanic_plan/README.md)
