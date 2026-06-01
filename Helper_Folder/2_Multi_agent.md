# Prompt-To-VR: Master Implementation Prompt — Multi-Agent Code Generation Overhaul

Use this document as the **single authoritative task** for Cursor Agent mode. Implement **every part** below in one coordinated change set. Do not treat any section as optional unless explicitly marked “follow-up engine work.”

---

## Table of contents

1. [How to work (required process)](#how-to-work-required-process)
2. [Executive summary](#executive-summary-two-problems-to-fix)
3. [Platform vision (any prompt → full VR game)](#platform-vision-any-prompt--full-vr-game)
4. [Part A — Work queue, ledger, atomic claim, dependency DAG](#part-a--work-queue-ledger-atomic-claim-dependency-dag-401--403)
5. [Part B — OpenAI Agents SDK (ChatGPT only, web search)](#part-b--openai-agents-sdk-chatgpt-only-web-search-enabled)
6. [Part C — Multi-file `CodeTask.files[]`](#part-c--multi-file-codetaskfiles-problem-2)
7. [Part D — Engine reality & prebuilt “weapon hub” pattern](#part-d--engine-reality--do-not-over-promise-in-generated-code)
8. [Part E — Schema, docs, tests, acceptance](#part-e--schema-docs-tests)
9. [Files to read first](#files-to-read-first)
10. [Implementation order](#implementation-order)

---

## Incomplete if any of these are missing

| # | Requirement |
|---|-------------|
| 1 | Work queue + **`codegen_board.json`** ledger + **atomic claim** + FCFS on **`pending`** tasks only |
| 2 | Dependency DAG: **`blocked`** until `depends_on` tasks are **`done`**, then **`pending`** (40.3) |
| 3 | **`CODEGEN_MAX_PARALLEL_AGENTS`** (default **3**) = up to three CodeTasks in **`in_progress`** at once |
| 4 | Each active worker runs **one OpenAI Agents SDK session per claimed CodeTask** (not one session for the whole project) |
| 5 | **Web search** tool enabled on every codegen agent run |
| 6 | **Multiple C++ files per mechanic** written via **`CodeTask.files[]`** (≤16), not a single `.cpp` |
| 7 | **`CodeTask.test_files[]` remains ignored** (no writes, no tests, no planner prompts for tests) |
| 8 | Schema export, docs, unit tests |

**Do not ship:** parallel workers without `blocked`/`depends_on`; Agents SDK without multi-file writes; a **second FastAPI orchestrator** service.

---

## How to work (required process)

### A. One deliverable at a time

Implement in order: **Part A (board + deps)** → verify tests → **Part B (Agents SDK)** → **Part C (multi-file writes)** → wire **`stage_code_generation.py`** → **Part E (docs/tests)**. Later parts depend on earlier ones.

### B. Keep the full architecture in mind

Ledger states, `refresh_blocked()`, Agents SDK runs, and multi-file disk writes must work together on **resume/re-run** (skip `done` tasks; do not double-claim).

### C. Research before coding (mandatory)

**Search the web** and read **official documentation** before implementing provider integration—especially the OpenAI Agents SDK. Verify:

- PyPI package name and version (`openai-agents` or current equivalent per docs)
- How to register **web search** as a hosted tool
- Model IDs compatible with Agents SDK (align with `settings.openai_hard_model`, default `gpt-5`)
- Agent loop / `Runner` API (see “Running agents” in OpenAI docs)

Do **not** rely only on the links in Part B or on training-data assumptions.

### Hard rules (repo policy)

- **Local-first native C++ VR** (Vulkan + OpenXR). No Unity, Unreal, Godot, WebXR, or browser VR.
- Generated code only under `engine/generated/{mechanics,ai,dialogue}/`.
- **Codegen stage = ChatGPT / OpenAI only** (OpenAI Agents SDK). Do **not** route this stage through Claude or `LlmClient.generate_code`. Other pipeline stages keep existing `LlmClient` unchanged.
- **Ignore `CodeTask.test_files[]` completely** — no disk writes, no CMake test targets, no planner prompts for test files, no schema work for tests in this task.

---

## Executive summary: two problems to fix

| # | Problem today | Target state |
|---|----------------|--------------|
| **1** | One sequential LLM call per `CodeTask`; no parallelism; no coordination; no `depends_on` | **N** parallel workers (default **N=3**). Each worker: claim **one** CodeTask → run **one** OpenAI Agents SDK session → write **all** files for that task → mark `done`/`failed` → claim next **pending** task. Shared **`codegen_board.json`**; **atomic claim**; **FCFS** among eligible tasks; **`depends_on`** DAG with **`blocked` → `pending`**. |
| **2** | Only one `.cpp` per mechanic; `CodeTask.files[]` unused | Agents work **recursively / multi-step** inside one task: analyze → optional web search → **file plan** → implement **multiple** `.cpp` files (≤16 per task) that compile together; populate and write **`files[]`**. |

### Why OpenAI Agents SDK instead of plain Chat Completions

A single Chat Completion returns one text blob. Complex VR mechanics (grab + equip + shooting hub + weapon profile + VFX) need **several compilation units** and **iterative** steps (research, draft file A, draft file B, fix registration). The Agents SDK **agent loop** (tools + multiple turns) is the right fit. That is the main reason to adopt it for the CodeTask stage instead of `LlmClient.generate_code`.

---

## Platform vision (any prompt → full VR game)

The pipeline must turn **any** detailed user prompt into a playable VR game—not a single franchise. **Iron Man VR** and **Shooter City** below are **reference workflows** the platform must support **generically** via BuildSpec → AssetManifest → MechanicPlan → SceneSpec → Codegen.

---

### Example A — Iron Man VR (New York, gauntlet, repulsor)

*Illustrative scale: one environment asset, one hero prop, five mechanics. Full games have many more scenes, assets, and mechanics; the same rules apply.*

#### Pipeline stages (this example)

| Stage | What happens for Iron Man VR |
|--------|------------------------------|
| **BuildSpec** | Scenes (e.g. New York), assets (Iron Man gauntlet, city environment), mechanics with Quest 3 button bindings, acceptance tests |
| **AssetManifest** | Image prompts for gauntlet + city → Gemini/Imagen → Tripo image-to-3D → GLB assets |
| **MechanicPlan** | Each mechanic: `builtin`, `builtin+config`, or `generate`; `depends_on` where needed |
| **SceneSpec** | Entities for gauntlet + city; tags (`grabbable`, `equippable`, `weapon_profile`); `mechanic_bindings`; player spawn in New York |
| **Code generation** | Agents SDK workers (≤3 parallel) claim tasks from ledger; respect deps; multi-file C++ per task |

#### MechanicPlan breakdown (exemplar)

| Mechanic | Classification | Role |
|----------|----------------|------|
| **Locomotion** | `builtin` | Teleport / head-relative slide / controller-relative slide / snap turn — scene bindings; movement in `Application.cpp` |
| **Grab objects** | `builtin+config` | Quest **grip** grabs tagged props; gauntlet must be **`grabbable`** (SceneSpec tags + optional config) |
| **Equip objects** | `builtin+config` | After grab, attach gauntlet to body slot — **forearm through fingertips only** (game-specific `params`) |
| **Shooting hub** | `builtin+config` or `generate` (`shooting_core`) | While holding/equipped weapon-capable object: **right trigger** (right hand) or **left trigger** (left hand) activates fire; **projectile type** comes from **weapon profile** code |
| **Iron Man repulsor blast** | `generate`, `depends_on: ["shooting_core", ...]` | Multi-file profile on hub; only when gauntlet equipped on matching side; trigger on **same side** as equipped gauntlet |

#### CodeTask agent workflow (repulsor task — steps A–D)

Every codegen agent run must follow this pattern (automate via SDK instructions + context injection):

**A — Read task metadata (before coding or searching)**  
From `CodeTask` + MechanicPlan item + project artifacts:

- `id`, `title`, `description` (up to 10,000 characters)
- `depends_on` (must be satisfied on ledger before claim)
- `interface` (`IMechanic` / `IBehaviour` / `IDialogueHook`)
- `headers`, planned `files[]`
- Which **assets** the mechanic uses (gauntlet asset id)
- Which **scenes** require this mechanic (e.g. New York) — from BuildSpec + `scene_spec*.json`

**B — Read dependencies and ledger**  
Repulsor example depends on: **grab** (pick up gauntlet), **equip** (wear on forearm), **shooting hub** (`shooting_core` task).  

- Do **not** implement until ledger shows every `depends_on` task **`done`**.  
- Load **`exports`** from completed deps: registered mechanic ids, `files_written`, hub/profile API hints.  
- Read **SceneSpec**: gauntlet entity id, tags, bindings.

**C — Research via web search (optional, encouraged)**  
After understanding the task, the agent may call **web search** to improve quality, for example:

1. Who Iron Man is and what a **repulsor blast** is visually  
2. How repulsors relate to the **gauntlet**  
3. C++ / VR patterns for spawning beam-like effects (conceptual only—final code uses `ptv/api/*`)  
4. Making beams appear to emit from **gauntlet muzzle** position in VR  
5. Color / motion design (e.g. cyan-green hard light) for plausible visuals  

**Guardrails:** Cap searches per task (default 5). **Reject** Unity/Unreal/Godot implementation advice. Output must be **native PTV** C++ only.

**D — Implement multi-file C++**  
Create **all** files needed for the mechanic (not one monolithic file):

- Build on **prebuilt shooting hub** pattern (profile registration / params)—do **not** duplicate global trigger polling if hub task already provides it  
- Game-specific **repulsor profile** code only  
- Write every file to paths under `engine/generated/mechanics/<project_id>/`  
- Populate `CodeTask.files[]` and board `files_written`

#### “Build on prebuilt shooting” (guns, gauntlets, rockets)

One **shooting hub** handles Quest **trigger input** while the player holds or has equipped a weapon-capable object. **Per-game / per-weapon** code registers a **profile** (pistol, rocket, repulsor):

```text
builtin.grab              → grabbable tag + Application grip loop
builtin.equip             → attach prop to body slot (e.g. forearm_l / forearm_r)
builtin.weapon_fire       → trigger while holding/equipped → dispatch by weapon_profile_id
    ├── generated.profile_pistol
    ├── generated.profile_rocket
    └── generated.iron_man_repulsor   (depends_on shooting_core)
```

**Shooter City variant:** One hub, many gun profiles (different projectiles, animations), each as its own generate task or multi-file profile—same architecture.

---

### Example B — Shooter City (gun, enemies, town hall, 2-minute defend)

| Mechanic | Plan | Agent reasoning |
|----------|------|-----------------|
| Locomotion / turn | `builtin` | Scene bindings only |
| Grab | `builtin` | Gun entity: tag **`grabbable`**, dynamic physics, near spawn |
| Shooting hub | `generate` `shooting_core` | Routes trigger when gun held in hand |
| Gun behavior | `generate`, `depends_on: ["shooting_core"]` | Muzzle from gun entity `CTransform`; **do not** reimplement grab |

Use **multiple files** when needed (`gun_profile.cpp`, `gun_projectile.cpp`, `gun_register.cpp`) so integration stays maintainable.

---

### Dependency scenario (40.3) — shooting and repulsor built in parallel

Two generate tasks: **`shooting_core`** and **`iron_man_repulsor`** with `depends_on: ["shooting_core"]`.

| Time | `shooting_core` | `iron_man_repulsor` |
|------|-----------------|---------------------|
| T0 | `pending`, claimable | `blocked` |
| T1 | Worker-1 claims; Agents SDK; multi-file write | Stays `blocked`; Worker-2 takes **other** `pending` tasks |
| T2 | `done`; `exports` filled | `refresh_blocked()` → `pending` |
| T3 | — | Worker-2 claims; uses shooting `exports`; implements repulsor files |

**No agent-to-agent chat.** Only ledger + `CodegenBoard.refresh_blocked()`.

---

## Part A — Work queue, ledger, atomic claim, dependency DAG (40.1 + 40.3)

### Context (today)

`apps/orchestrator/ptv_orchestrator/pipeline/stage_code_generation.py`:

- Sequential loop over `MechanicPlanItem` with `code_task`
- One `LlmClient.generate_code` call → one `{task_id}.cpp`
- Ignores `CodeTask.files[]`
- Ignores `CodeTask.test_files[]` (keep ignoring)
- No ledger, no `depends_on`, no parallelism

**Do NOT** add a second FastAPI orchestrator. Extend **`stage_code_generation`** only.

**Pipeline order:** `scene_spec` runs **before** `code_generation` in `orchestrator.py` — load `scene_spec*.json` for agent context.

### Goals (all required)

**G1 — `codegen_max_parallel_agents`**

- `Settings.codegen_max_parallel_agents: int = 3`
- Env: `CODEGEN_MAX_PARALLEL_AGENTS`
- `ThreadPoolExecutor(max_workers=N)`
- Each thread runs the **worker loop** below until no `pending` tasks remain

**G2 — One mechanic per agent at a time**

- At any moment, each `agent_id` (`worker-0`, `worker-1`, …) has **at most one** CodeTask in `in_progress`
- FCFS: workers compete for the next eligible **`pending`** task — **not** a static split (“worker-0 owns tasks 0–3”)

**G3 — Shared ledger (awareness without agent chat)**

- Path: `projects/<project_id>/codegen_board.json`
- All workers read before claim; write on claim / done / failed
- **Atomic claim** — two workers must never claim the same `task_id`

**G4 — Dependency DAG (40.3 — mandatory)**

- Add `depends_on: List[IdStr]` on `CodeTask` (Pydantic + JSON schema export)
- States: `pending` | `blocked` | `in_progress` | `done` | `failed`
- **Claim rule:** only `pending` where every `depends_on` id is `done`
- Never claim `blocked`, `in_progress`, or `done`
- `CodegenBoard.refresh_blocked()` after each `mark_done` (promote `blocked` → `pending` when deps satisfied)

**G5 — FCFS among eligible tasks**

- Stable order = **MechanicPlan item order** (first pending in plan order wins next claim)

**G6 — Observability**

- `ctx.emit(ProjectStage.code_generation, ...)` for: board init, claim, unblock (`blocked`→`pending`), done, failed
- Stage record message: counts done / failed / blocked

**G7 — Safety (per written file)**

- `filter_cpp_only`, `repair_generated_cpp`, `assert_path_allowed`
- `.codetask.json` sidecar updated with final `files[]` content
- Respect per-project lock in `orchestrator.py` (codegen vs cmake)

### Combined runtime architecture (Parts A + B + C)

```text
stage_code_generation.run(ctx)
  │
  ├─ Load MechanicPlan → items with code_task != null
  ├─ Load scene_spec.json + scene_spec.<scene_id>.json → scene_context
  ├─ CodegenBoard.init(plan, scene_context) → codegen_board.json
  │     • Initial status: pending OR blocked per depends_on
  ├─ CodegenBoard.refresh_blocked()
  │
  ├─ ThreadPoolExecutor(max_workers=settings.codegen_max_parallel_agents)
  │     worker loop (agent_id = worker-0, worker-1, worker-2):
  │       1. refresh_blocked()
  │       2. task = atomic_claim_next_pending(agent_id)   # FCFS; pending only
  │       3. if none: exit loop
  │       4. run_openai_agents_sdk_for_task(task, scene_context, board_snapshot)
  │       5. for each file in result.files: bounds + write disk
  │       6. mark_done(files_written, exports) OR mark_failed(error)
  │       7. goto 1
  │
  ├─ join all workers
  └─ StageResult(artifacts=...)
```

### `codegen_board.json` (implement + document)

```json
{
  "version": 1,
  "max_agents": 3,
  "tasks": {
    "shooting_core": {
      "mechanic_plan_item_id": "shooting_core",
      "status": "pending",
      "depends_on": [],
      "agent_id": null,
      "claimed_at": null,
      "completed_at": null,
      "files_written": [],
      "exports": {
        "registered_mechanic_id": "generated.shooting_core",
        "weapon_profile_macro": "PTV_REGISTER_WEAPON_PROFILE"
      },
      "error": null
    },
    "iron_man_repulsor": {
      "status": "blocked",
      "depends_on": ["shooting_core"],
      "agent_id": null,
      "files_written": [],
      "exports": {},
      "error": null
    }
  }
}
```

**Status transitions (enforce in code):**

- `blocked` → `pending` when all `depends_on` are `done`
- `pending` → `in_progress` on successful atomic claim
- `in_progress` → `done` on success | `failed` on error (do not leave tasks stuck in `in_progress`)

**Resume policy:** If board exists, skip tasks already `done`; rebuild missing board from MechanicPlan; document `failed` retry behavior.

### `depends_on` source of truth

- Structured field on `CodeTask` only — board must **not** infer deps from free text
- Update `MECHANIC_PLAN_SYSTEM` so planner sets `depends_on` for generated tasks (e.g. profiles depend on `shooting_core`)
- Fallback: empty `depends_on` → task starts as `pending`

### `atomic_claim_next_pending(agent_id)` algorithm

1. Acquire project-scoped lock  
2. `refresh_blocked()`  
3. Iterate tasks in MechanicPlan order  
4. First with `status == "pending"`: set `in_progress`, `agent_id`, `claimed_at`; persist atomically (temp file + rename); return task  
5. Else return `None`  

Must be correct under `max_agents=3` concurrent threads.

### New module

`apps/orchestrator/ptv_orchestrator/pipeline/codegen_board.py` + unit tests (no network).

---

## Part B — OpenAI Agents SDK (ChatGPT only, web search enabled)

### Official resources (verify live before coding)

1. [OpenAI Agents guide](https://developers.openai.com/api/docs/guides/agents)  
2. [openai-agents-python (GitHub)](https://github.com/openai/openai-agents-python)  
3. [Running agents](https://developers.openai.com/api/docs/guides/agents/running-agents)  
4. [Tools (hosted tools, including web search)](https://developers.openai.com/api/docs/guides/tools)  

Confirm PyPI install name and import path from current docs before editing `pyproject.toml`.

### What “three active agents” means

| Setting | Meaning |
|---------|---------|
| `CODEGEN_MAX_PARALLEL_AGENTS=3` | Up to **three CodeTasks** may be `in_progress` simultaneously |
| Per worker | **One** Agents SDK `Runner.run` (or equivalent) **per claimed CodeTask**, then return to queue |
| Not | One SDK session for all 10 mechanics; not three files in parallel inside one task unless the agent chooses that in its loop |

### Implementation requirements

**B1 — Dependency**

- Add OpenAI Agents SDK to `apps/orchestrator/pyproject.toml` (document version pin after verifying docs)

**B2 — Settings / env**

- `OPENAI_API_KEY` required for codegen stage  
- Model: `settings.openai_hard_model` (default `gpt-5`) or documented compatible model  
- `codegen_max_parallel_agents` (default 3)  
- Document in `.env.example`

**B3 — Agent per claimed task**

- **Instructions:** from updated `CODE_TASK_SYSTEM` (multi-file, hub/profile pattern, allowed includes, dependency exports, no hallucinated headers)  
- **Input context (user message):**  
  - Full `CodeTask` JSON  
  - Ledger snapshot: all tasks with `status`, `files_written` for `done` tasks  
  - `exports` from each completed `depends_on` task  
  - SceneSpec excerpt: entity ids, tags, `mechanic_bindings` for relevant assets  
  - BuildSpec mechanic summary excerpt if useful  
- **Tools:** enable SDK **web search** (hosted tool per official docs)  
- **Output format (structured JSON preferred):**

```json
{
  "files": [
    { "path": "engine/generated/mechanics/<project_id>/repulsor_profile.cpp", "contents": "..." },
    { "path": "engine/generated/mechanics/<project_id>/repulsor_register.cpp", "contents": "..." }
  ],
  "exports": {
    "registered_mechanic_id": "generated.iron_man_repulsor",
    "weapon_profile_id": "repulsor"
  }
}
```

**B4 — Multi-step / “recursive” behavior inside one CodeTask**

Instruct the agent to:

1. Analyze description, deps, scene context, completed ledger entries  
2. Plan file split (≤16 files) with path + purpose per file  
3. Optionally web search (within cap) for domain / VR / visual research  
4. Implement files (multiple turns OK)  
5. Ensure single `PTV_REGISTER_MECHANIC` / profile registration consistency across files  
6. Return structured `files` + `exports` for the board  

**B5 — Web search guardrails**

- Use for research only; final artifact is C++ under `engine/generated/`  
- Ignore Unity/Unreal/Godot patterns from search results  
- OpenXR / Quest 3 references are fine for comments/TODOs; code must use existing PTV APIs  

**B6 — No Claude on this path**

- `stage_code_generation` must not call `make_llm()` / Anthropic for mechanic generation  
- Other stages unchanged unless shared utilities are needed  

### New module

`apps/orchestrator/ptv_orchestrator/pipeline/codegen_agent.py` — SDK wrapper; mockable for tests.

---

## Part C — Multi-file `CodeTask.files[]` (Problem 2)

### Requirements

**C1 — `files[]` is the sole write contract**

- Do **not** write only `{task_id}.cpp` unless the agent returns exactly one file in `files[]`  
- After SDK run: populate `CodeTask.files` with `CodeTaskFile(path, contents)`  
- Limits: ≤16 files, ≤120,000 characters per `contents`, path ≤256 chars  
- Paths must start with `engine/generated/{mechanics|ai|dialogue}/` and include `<project_id>/`

**C2 — Design for real VR mechanics (not one blob)**

For shooting, repulsor, gun profiles, agents must account for:

| Concern | Action |
|---------|--------|
| Specific in-game object (gun, gauntlet) | Use SceneSpec entity id + `CMesh.asset_id` in comments/code via `world.find` |
| Grab already exists | Tag `grabbable`; **do not** reimplement grip in generated code |
| Equip | `builtin+config` + SceneSpec; generated code assumes equipped state via params until engine exposes equip API |
| Quest 3 triggers | Document in params / profile; use `params` flags until engine input bridge exists |
| Depends on hub | Read `exports` from `shooting_core` (or hub task); implement **profile** only |

**C3 — Per-file write pipeline**

For each `files[]` entry:

`filter_cpp_only(contents)` → `repair_generated_cpp` → `assert_path_allowed` → write disk → append path to board `files_written`.

**C4 — Single-file fallback**

If agent returns one file, set `files = [{ path, contents }]` for backward compatibility.

**C5 — `test_files[]`**

**Still completely ignored.** No reads, no writes, no CMake tests, no planner changes for tests.

### Prompt updates

**`CODE_TASK_SYSTEM`:**

- Remove “one file only / 300 lines max” as the only output mode  
- Require multi-file output when mechanic has separable concerns (registration, profile, simulation, config)  
- Document weapon hub / `PTV_REGISTER_WEAPON_PROFILE` pattern (engine follow-up may add macro)  
- Instruct: read `depends_on` exports from context; do not duplicate hub logic  

**`MECHANIC_PLAN_SYSTEM`:**

- Set `depends_on` on generated tasks  
- Encourage `builtin+config` for grab, equip, shooting hub when catalog covers them  
- In `code_task.description`, list integration points (asset ids, trigger binding, profile id)  
- Optional: suggest file split names in description (planner hint; agent finalizes)

---

## Part D — Engine reality & do not over-promise in generated code

Read before writing prompts that imply full Quest behavior:

| Component | Status today |
|-----------|----------------|
| Grab + tag `grabbable` | **Works** — `Application.cpp` grip loop |
| `builtin.grab` | Registers component; grab logic is in Application |
| **Equip** on body slots | **Not fully implemented** — plan via `builtin+config` + SceneSpec; engine follow-up |
| **Shooting hub + live trigger → held/equipped object** | **Not fully implemented** — `builtin.projectile_hook` is stub; no input bridge to `IMechanic::on_update` |
| `MechanicHost::update` | Runs generated `on_update` but **without** live OpenXR trigger state |

**This task delivers:** ledger + parallel Agents SDK + multi-file + planner/schema/prompt updates.

**Generated code may:** use `params` for simulated trigger (`primary_trigger`, `grip_held`) with TODOs until engine adds `builtin.weapon_fire` + input bridge.

**Document gaps** in `docs/codegen-agents.md` under “Engine follow-up.”

---

## Part E — Schema, docs, tests

### Schema changes

- Add `depends_on: List[IdStr] = Field(default_factory=list)` to `CodeTask` in `packages/schemas/ptv_schemas/code_task.py`  
- Export: `python -m ptv_schemas.export packages/schemas/json`  
- Do **not** add test file handling

### Settings / `.env.example`

```env
CODEGEN_MAX_PARALLEL_AGENTS=3
OPENAI_API_KEY=sk-...
OPENAI_HARD_MODEL=gpt-5
```

### Documentation (required)

Add **`docs/codegen-agents.md`** (or extend `docs/prompt-schema.md`) covering:

- Ledger path + status machine diagram (`blocked` / `pending` / …)  
- FCFS + atomic claim  
- `depends_on` field  
- OpenAI Agents SDK + web search caps  
- Multi-file `files[]` workflow  
- Iron Man + Shooter City as reference workflows  
- **`test_files` unused**  
- Engine gaps (equip, weapon_fire, input bridge)  

### Tests (no network)

| Test | Purpose |
|------|---------|
| Board: `blocked` not claimable until dep `done` | 40.3 |
| Dep `done` → `refresh_blocked` → claimable | 40.3 |
| Two threads cannot claim same task | Atomic claim |
| FCFS order matches MechanicPlan | 40.1 |
| Mock SDK returns 3 files → 3 paths on disk, bounds applied | Part C |
| `depends_on` integration test | End-to-end board |
| Mock `codegen_agent` in stage test | Wiring |

### Acceptance checklist

- [ ] `CODEGEN_MAX_PARALLEL_AGENTS=3` runs up to three SDK sessions on distinct `pending` tasks  
- [ ] `codegen_board.json` shows `agent_id`, statuses, `files_written`, `exports`  
- [ ] Task B with `depends_on: ["A"]` stays `blocked` until A is `done`, then claimable  
- [ ] No duplicate claims under parallel load  
- [ ] Each task uses **OpenAI Agents SDK** with **web search** tool enabled  
- [ ] Complex mechanic writes **multiple** `.cpp` via `files[]` (not only `{id}.cpp`)  
- [ ] SceneSpec loaded into agent context  
- [ ] `CodeTask.test_files[]` still unused  
- [ ] Codegen does **not** call Claude  
- [ ] No second orchestrator service  
- [ ] Existing `pytest` passes + new tests green  

---

## Files to read first

| File | Why |
|------|-----|
| `apps/orchestrator/ptv_orchestrator/pipeline/stage_code_generation.py` | Integration point |
| `apps/orchestrator/ptv_orchestrator/pipeline/prompts.py` | `CODE_TASK_SYSTEM`, `MECHANIC_PLAN_SYSTEM` |
| `apps/orchestrator/ptv_orchestrator/pipeline/bounds.py` | Per-file safety |
| `apps/orchestrator/ptv_orchestrator/pipeline/orchestrator.py` | Stage order |
| `apps/orchestrator/ptv_orchestrator/settings.py` | New settings |
| `packages/schemas/ptv_schemas/code_task.py` | `files[]`, add `depends_on` |
| `engine/runtime/src/core/Application.cpp` | Grab, trigger (engine context) |
| `engine/runtime/src/mechanics/BuiltinMechanics.cpp` | Builtin stubs |
| `apps/orchestrator/ptv_orchestrator/engine_catalog.py` | Valid `builtin.*` ids |
| `apps/orchestrator/pyproject.toml` | Add Agents SDK dependency |

---

## Implementation order

1. **`codegen_board.py`** + `depends_on` on `CodeTask` + schema export + board unit tests (**40.3 mandatory**)  
2. **`codegen_agent.py`** — OpenAI Agents SDK + web search + structured multi-file JSON output  
3. **Multi-file write path** + update `CODE_TASK_SYSTEM` / `MECHANIC_PLAN_SYSTEM`  
4. **Rewrite `stage_code_generation.py`** — board + thread pool + agent + per-file bounds  
5. **Settings**, `.env.example`, **`docs/codegen-agents.md`**  
6. **Manual smoke:** project with ≥2 generate tasks where task B `depends_on` task A; verify board states and multiple `.cpp` files per task  

---

## PR guardrail (paste into PR description)

> **Incomplete** if this PR: (1) adds parallel workers without `blocked`/`depends_on` and `refresh_blocked`, (2) does not use **OpenAI Agents SDK** with **web search** for codegen, or (3) still writes only one `.cpp` per CodeTask while `CodeTask.files[]` remains unused.  
> **`test_files`** must remain unused.

---

## One-line completion rule for the implementing agent

> If the implementation has parallel workers but no `blocked` status or `depends_on` handling, or uses plain `LlmClient.generate_code` instead of OpenAI Agents SDK for the codegen stage, or does not write multiple files per CodeTask via `files[]`, the task is **not complete**.
