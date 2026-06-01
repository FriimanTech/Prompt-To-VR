# Prompt-To-VR: Master Implementation Prompt — Multi-Agent Code Generation Overhaul

Use this document as the **single authoritative task** for Cursor Agent mode. Implement **every part** below in one coordinated change set. Do not treat any section as optional unless explicitly marked “follow-up engine work.”

The work has two problems to fix and one required four-step agent process. Everything else in this document supports them.

---

## Table of contents

1. [How to work (required process)](#how-to-work-required-process)
2. [Executive summary — two problems to fix](#executive-summary-two-problems-to-fix)
3. [Required four-step CodeTask agent process](#required-four-step-codetask-agent-process)
4. [Platform vision (any prompt → full VR game)](#platform-vision-any-prompt--full-vr-game)
5. [Part A — Work queue, ledger, atomic claim, dependency DAG](#part-a--work-queue-ledger-atomic-claim-dependency-dag-401--403)
6. [Part B — OpenAI Agents SDK (ChatGPT only, web search, three concurrent agents)](#part-b--openai-agents-sdk-chatgpt-only-web-search-three-concurrent-agents)
7. [Part C — Multi-file `CodeTask.files[]`](#part-c--multi-file-codetaskfiles-problem-2)
8. [Part D — Engine reality & do not over-promise](#part-d--engine-reality--do-not-over-promise-in-generated-code)
9. [Part E — Schema, prompts, docs, tests, acceptance](#part-e--schema-prompts-docs-tests-acceptance)
10. [Files to read first](#files-to-read-first)
11. [Implementation order](#implementation-order)
12. [PR guardrail & completion rule](#pr-guardrail-paste-into-pr-description)

---

## Incomplete if any of these are missing

| # | Requirement |
|---|-------------|
| 1 | Work queue + **`codegen_board.json`** ledger + **atomic claim** + FCFS on **`pending`** tasks only |
| 2 | Dependency DAG: **`blocked`** until `depends_on` tasks are **`done`**, then **`pending`** (40.3) |
| 3 | **`CODEGEN_MAX_PARALLEL_AGENTS`** (default **3**) = up to three CodeTasks in **`in_progress`** at once |
| 4 | Each active worker runs **one OpenAI Agents SDK session per claimed CodeTask** (not one session for the whole project) |
| 5 | **Web search** tool enabled on every codegen agent run — **no cap on web searches per CodeTask** |
| 6 | **Multiple C++ files per mechanic** written via **`CodeTask.files[]`** (≤16), not a single `.cpp` |
| 7 | Every codegen run obeys the **required four-step agent process** (read metadata → read deps/ledger → optional web search → implement multi-file C++) |
| 8 | Updates to **`apps/orchestrator/ptv_orchestrator/pipeline/prompts.py`** so no system prompt tells the agent to limit itself to **one** `.cpp` file or to cap code length per file |
| 9 | **`CodeTask.test_files[]` remains ignored** (no writes, no tests, no planner prompts for tests) |
| 10 | Schema export, docs, unit tests |

**Do not ship:** parallel workers without `blocked`/`depends_on`; Agents SDK without multi-file writes; a **second FastAPI orchestrator** service; a web-search cap; any new prompt language that pins generated code to a single file.

---

## How to work (required process)

### A. One deliverable at a time

Implement in order: **Part A (board + deps)** → verify tests → **Part B (Agents SDK, no web-search cap, three concurrent agents)** → **Part C (multi-file writes + prompts.py rewrites)** → wire **`stage_code_generation.py`** → **Part E (docs/tests)**. Later parts depend on earlier ones.

Make sure that, before moving on from any one part, the tests for that part pass and the user-visible behavior described in this document still holds.

### B. Keep the full architecture in mind

Ledger states, `refresh_blocked()`, Agents SDK runs, multi-file disk writes, and prompt updates must work together on **resume / re-run** (skip `done` tasks; do not double-claim; do not regenerate already-`done` files).

### C. Research before coding (mandatory)

**Search the web** and read **official documentation** before implementing provider integration — especially the OpenAI Agents SDK. Verify:

- PyPI package name and version (`openai-agents` or current equivalent per docs)
- How to register **web search** as a hosted tool on an SDK agent
- Model IDs compatible with the Agents SDK (align with `settings.openai_hard_model`, default `gpt-5`)
- Agent loop / `Runner` API (see “Running agents” in OpenAI docs)
- That a single SDK `Runner.run` can perform an **unbounded** number of tool calls within one CodeTask (we are intentionally not capping web search; see Part B)

Do **not** rely only on the links in Part B or on training-data assumptions. For every problem you fix in this document, **search the web** for the current best-practice approach and the latest provider docs before writing code.

### D. Hard rules (repo policy)

- **Local-first native C++ VR** (Vulkan + OpenXR). No Unity, Unreal, Godot, WebXR, or browser VR.
- Generated code only under `engine/generated/{mechanics,ai,dialogue}/`.
- **Codegen stage = ChatGPT / OpenAI only** (OpenAI Agents SDK). Do **not** route this stage through Claude or `LlmClient.generate_code`. Other pipeline stages keep existing `LlmClient` unchanged.
- **Ignore `CodeTask.test_files[]` completely** — no disk writes, no CMake test targets, no planner prompts for test files, no schema work for tests in this task.

---

## Executive summary — two problems to fix

| # | Problem today | Target state |
|---|----------------|--------------|
| **1** | One sequential LLM call per `CodeTask`; no parallelism; no coordination; no `depends_on` | **N** parallel workers (default **N=3**). Each worker: claim **one** CodeTask → run **one** OpenAI Agents SDK session → write **all** files for that task → mark `done`/`failed` → claim next **pending** task. Shared **`codegen_board.json`**; **atomic claim**; **FCFS** among eligible tasks; **`depends_on`** DAG with **`blocked` → `pending`**. |
| **2** | Only one `.cpp` per mechanic; `CodeTask.files[]` unused; system prompts in `prompts.py` implicitly cap output to one file / a fixed line count | Agents work **recursively / multi-step** inside one task: analyze → optional web search (no cap) → **file plan** → implement **multiple** `.cpp` files (≤16 per task) that compile together; populate and write **`files[]`**. **`CODE_TASK_SYSTEM`** and any related prompts updated so the agent is **never told to limit itself to one C++ file or to a fixed number of lines** per file. |

### Why OpenAI Agents SDK instead of plain Chat Completions

A single Chat Completion returns one text blob. Complex VR mechanics (grab + equip + shooting hub + weapon profile + VFX) need **several compilation units** and **iterative** steps (research, draft file A, draft file B, fix registration). The Agents SDK **agent loop** (tools + multiple turns) is the right fit — the agent can call **web search** repeatedly, plan, then emit a multi-file result. That is the entire reason for adopting it for the CodeTask stage instead of `LlmClient.generate_code`.

---

## Required four-step CodeTask agent process

Every codegen agent run on every claimed `CodeTask` MUST follow this exact four-step flow. This is **the** process the platform commits to; encode it in the SDK system instructions and in the worker driver so that no agent run can skip a step.

### 1. Read task metadata (before coding or searching)

From the claimed `CodeTask` plus the project artifacts loaded into the worker’s context:

- `id`, `title`, `description` (up to 10,000 chars)
- `interface` (`IMechanic` / `IBehaviour` / `IDialogueHook`)
- `depends_on` (must be satisfied on the ledger before the task can be claimed — see Part A)
- `headers`, planned `files[]` hints from the planner
- Which **assets** the mechanic uses (entity ids, asset ids, GLB paths)
- Which **scenes** require this mechanic (BuildSpec + every `scene_spec*.json` for the project)
- Which **mechanic bindings** the SceneSpec has wired onto those assets (e.g. `builtin.grab` on a prop, generated mechanic ids the agent must register)

The agent must complete this read **before** any tool call. Use it to decide what files to produce and which deps to actually load.

### 2. Read dependencies and ledger

- Load the current **`codegen_board.json`** snapshot
- For each id in `depends_on`, load the **`exports`** field from that ledger entry (registered mechanic id, registered weapon-profile macro / id, public symbols, `files_written` paths)
- Never claim a task whose deps are not all `done` — the board enforces this; the agent must additionally **read** those exports so the new code plugs into the **real** surface the dep produced rather than inventing one
- Pull any relevant fragments of the SceneSpec (entity ids, tags, bindings) for the assets in step 1

### 3. Research via web search (optional, encouraged)

- **No cap on web searches per CodeTask.** The agent may call web search as many times as it judges useful within its own loop budget
- Use web search to research domain meaning (e.g. what a real-world reference looks like and behaves like), VR / OpenXR patterns, native C++ technique, math, projectile design, VFX color/motion intent, controller-input ergonomics — anything that improves the quality of the generated mechanic
- Final artifact must be **native PTV C++** using only the public API under `engine/runtime/include/ptv/api/*` and the public scene / ECS headers — **reject** Unity, Unreal, Godot, browser-VR patterns from search results

### 4. Implement multi-file C++

- Decide the natural file split for this mechanic (registration, profile, simulation, helpers, VFX hookup, etc.) and produce **one entry per file** in `CodeTask.files[]` (≤16)
- Every file path must live under `engine/generated/{mechanics|ai|dialogue}/<project_id>/`
- Each file passes `filter_cpp_only` → `repair_generated_cpp` → `assert_path_allowed`, then is written to disk
- Populate the board’s `files_written` and `exports` for the task
- Update the `.codetask.json` sidecar with the final `files[]` content

This four-step contract is the spine of the codegen stage. If a generated mechanic is missing a step (e.g. the agent skipped reading SceneSpec, or wrote one mega-file instead of separable units), the implementation is incomplete.

---

## Platform vision (any prompt → full VR game)

The pipeline must turn **any** detailed user prompt into a playable native C++ VR game on Quest 3 via PC VR. The reference workflow below describes the **kind of process** the platform must support end-to-end, for any prompt — it is **not** a directive to build that specific game.

### Pipeline stages (fixed order)

```text
BuildSpec  →  AssetManifest  →  MechanicPlan  →  SceneSpec  →
reference images (Gemini / Imagen)  →  Tripo image-to-3D (textured PBR)  →
asset normalization  →  Code Generation (THIS PR)  →
scene compile  →  CMake build  →  launch (OpenXR)
```

Schema-validated artifacts flow between stages. Downstream stages consume the **structured artifact**, never the raw prompt. The Code Generation stage in this PR consumes BuildSpec + AssetManifest + MechanicPlan + every `scene_spec*.json` and produces native C++ under `engine/generated/{mechanics,ai,dialogue}/<project_id>/`.

### MechanicPlan classifications (fixed taxonomy)

Each mechanic the planner emits falls in one of three buckets, in this preference order:

1. **`builtin`** — engine ships it; use as-is, no per-game tuning
2. **`builtin+config`** — engine ships it; planner sets per-game parameters that travel on the SceneSpec **`MechanicBinding`** for the entity, not as new C++
3. **`generate`** — no catalog entry fits; the agent writes new C++ under `engine/generated/` (this PR)

The planner is shown the **live engine catalog** at runtime: every built-in mechanic plus every mechanic that has been **promoted** from a past game into `engine/promoted/registry.json`. The planner must prefer reuse over regeneration in the order above.

### Engine growth loop (promoted registry)

After a build succeeds and the generated mechanics compile and pass acceptance, the engine’s promotion stage indexes them into `engine/promoted/registry.json`. The next prompt’s planner sees them in the catalog as **`promoted.<id>`** and may classify them as `builtin` for that next game, generating only the truly new logic on top. The same `depends_on` / DAG architecture handles cross-game reuse — yesterday’s `generate` becomes today’s already-`done` dependency.

This PR must keep this loop intact: ledger `exports`, `files_written`, and `.codetask.json` sidecars are the inputs the promotion stage relies on.

---

### Reference workflow (illustrative only — do NOT implement these mechanics)

The following Iron Man-style walkthrough exists to show the **shape** of the process the platform must support for any prompt. **Do not implement the Equip mechanic, the Shooting-Hub mechanic, or the Repulsor Blast mechanic as part of this PR.** They are described here so the architecture you build can support workflows like this when a real prompt later asks for them.

**Imagined prompt:** “I want a Quest 3 Iron Man VR game set in New York City where users don't fly but can run around the city with Iron Man's repulsor gauntlets and can shoot anything just for fun.”

For this illustrative game: one scene, two assets, six mechanics. A real game has many more of each — the same rules apply.

#### BuildSpec (illustrative)

Captures the game’s intent in strict schema:

- Scenes: one New York environment
- Mechanics: locomotion, snap turn, grab, equip, shooting hub, repulsor blast
- Asset needs: an Iron Man gauntlet prop and a New York City environment
- Acceptance goals the final build must satisfy before it is considered playable

#### AssetManifest (illustrative)

Turns asset needs into concrete `AssetItem` entries with `category`, `style_notes`, and generation hints:

- `environment`: “Manhattan city block, daytime, photoreal”
- `prop`: “Iron Man gauntlet, MK-style, gold-and-red PBR, paired left/right”

After SceneSpec is composed, reference images are generated (Gemini / Imagen) for each `AssetItem`; the chosen images go to Tripo’s image-to-3D with `texture=True`, `pbr=True`, `texture_alignment="original_image"`. Each `AssetItem` ends up as `projects/<id>/assets/<asset_id>/model.glb`.

#### MechanicPlan (illustrative breakdown — 3 builtin / 1 builtin+config / 2 generate)

| Mechanic | Bucket | Reason |
|----------|--------|--------|
| **Locomotion** | `builtin` | Engine smooth locomotion as-is (left joystick) |
| **Snap Turn** | `builtin` | Engine snap turn as-is (right joystick, 45° per push) |
| **Grab** | `builtin` | Engine grab handles grip-squeeze for any `grabbable`-tagged prop; gauntlet is made grabbable in SceneSpec, **not** by forking grab |
| **Equip** | `builtin+config` | Engine equip lets any tagged object attach to body slots; per-game tuning travels as **parameters on the binding**, never as new C++ |
| **Shooting Hub** | `generate` | Engine does not yet ship a hub that listens for trigger pulls only when holding/wearing a `weapon`-tagged object and routes to a weapon profile |
| **Repulsor Blast** | `generate`, `depends_on: ["shooting_hub"]` | Game-specific weapon profile that registers against the hub; never re-implements grab / equip / trigger listening |

#### Equip parameter design (illustrative)

The platform’s `builtin+config` pattern allows independent, optional parameters per object — the planner uses **only** the ones that matter for the prop. For Equip, the engine should ultimately understand three independent optional parameters:

1. **Allowed body part(s)** (optional). Allowlist of named body slots from the engine skeleton vocabulary (e.g. `head`, `chest`, `waist`, `back`, `left_hand`, `right_hand`, `left_forearm`, `right_forearm`, `left_foot`, `right_foot`). If set, the object can land only on listed slots. A helmet → `["head"]`. A one-handed sword → `["left_hand", "right_hand"]`.
2. **Custom equipable range** (optional). A pair of anchor points on the avatar skeleton that define the object’s coverage along a limb or body region — e.g. `start = "left_elbow"`, `end = "left_fingertips"`, or `start = "head_top"`, `end = "waist"`. Because anchor names identify limb and side, the engine can infer the slot from this parameter alone. If set without (1), the object is restricted to the implied limb / region.
3. **Equip offset, rotation, attachment behavior** (optional). Positional offset relative to the anchor frame, rotation offset, and attachment mode (`rigid_weld` / `soft_follow` / `snap_only`). If omitted, defaults are anchor-aligned, no offset, rigid weld.

For the illustrative Iron Man gauntlets, only parameter 2 is needed: `start = "left_elbow"`, `end = "left_fingertips"` (left gauntlet) and `start = "right_elbow"`, `end = "right_fingertips"` (right). The engine infers the forearm slot from the sided anchor names, automatically rejects head/chest/legs, and pairs left↔left and right↔right — all without the planner filling in (1) or (3).

**This PR does not implement the Equip mechanic.** It just must not *prevent* this design from being added later — `MechanicBinding.params` must be free-form per-binding configuration, and `MECHANIC_PLAN_SYSTEM` must be able to flag `builtin+config` items whose configuration is purely binding parameters.

#### SceneSpec (illustrative)

The SceneSpec stage takes BuildSpec + AssetManifest + MechanicPlan + live engine catalog and produces a deterministic entity / component layout that does four things — the platform must support all four for any prompt:

1. **Places the player spawn** inside the chosen environment (e.g. street level on a Manhattan block, eye height 1.7 m, facing the action). The player rig is just an entity that the built-in locomotion and snap-turn mechanics bind to — there is no separate player-controller C++.
2. **Places one entity per `AssetItem`** from the AssetManifest, each pointing at the right GLB and tagged so the rest of the engine can react. For example:
   - `manhattan_city` at `[0, 0, 0]`, rotation identity, `physics.kind = "static"`, `physics.collision_shape = "mesh"` (so the player can walk on the sidewalks and collide with buildings), tags `["environment"]`. Spawn sits on top of this entity.
   - `gauntlet_left` within arm’s reach of spawn (~0.6 m forward and slightly left, at hand height), referencing the gauntlet GLB, `physics.kind = "dynamic"`, an AABB or sphere collision shape, tags `["interactable", "grabbable", "wearable", "weapon", "hand_left"]`.
   - `gauntlet_right` symmetric, tags `["interactable", "grabbable", "wearable", "weapon", "hand_right"]`.
3. **Wires every mechanic from the MechanicPlan onto the right entities via `MechanicBinding` entries** — the moment classifications become concrete runtime wiring. Examples:
   - Player rig: `builtin.smooth_locomotion`, `builtin.snap_turn` (no params beyond defaults).
   - Each gauntlet: `builtin.grab` so the engine’s Grab mechanic recognizes them on grip-squeeze.
   - Each gauntlet: `builtin.equip` with the per-game params the planner tuned (e.g. `{ "coverage_start": "fingertips", "coverage_end": "elbow", "pair_with": "gauntlet_right" }`). The configuration travels as **parameters on the binding**, not as new C++ — this is exactly what makes Equip `builtin+config`.
   - Each gauntlet: a `generated.shooting_hub` binding declaring it carries a weapon profile, plus a `generated.repulsor_blast` binding with params like `{ "side": "left", "weapon_profile_id": "iron_man_repulsor", "muzzle_anchor": "palm", "forward_axis": "z+" }`.
4. **Places lights and sky** for the scene (e.g. one directional sun across the block, daylight sky color, moderate ambient so PBR materials read correctly).

In short, the SceneSpec is the bridge between the abstract MechanicPlan and the deterministic runtime: it turns sentences like “the gauntlet should be grabbable, equippable on the forearm, and fire repulsors on trigger” into concrete entity ids, transforms, mesh refs, tags, and mechanic bindings that the engine’s scene loader can apply directly to the ECS world — and that the C++ Code Generation stage can target by name.

#### Code Generation (illustrative — shows what this PR must support)

The orchestrator turns each `generate` MechanicPlan item into a `CodeTask` and enrolls it on the per-project ledger at `projects/<project_id>/codegen_board.json`. For the illustrative game, two tasks land on the board:

- `shooting_hub` — `depends_on: []` — starts `pending`
- `repulsor_blast` — `depends_on: ["shooting_hub"]` — starts `blocked`

There is no static “Shooting Hub goes first, Repulsor goes second” schedule. The board enforces order through **state** alone. Up to `CODEGEN_MAX_PARALLEL_AGENTS` workers (default 3) run the same worker loop in parallel:

1. `refresh_blocked()`
2. `atomic_claim_next_pending(agent_id)` (FCFS; returns `None` if nothing claimable)
3. If `None`: exit cleanly
4. Run **one OpenAI Agents SDK session** for the claimed task, following the four-step agent process (read metadata → read deps & ledger → optional web search with no cap → multi-file C++ implementation)
5. `mark_done(task_id, files_written, exports)` or `mark_failed(task_id, error)`
6. Loop

On the first pass, `shooting_hub` is the only `pending` task — whichever worker wins the atomic claim becomes the “Shooting Hub worker” by virtue of that claim; other workers see no eligible task and exit cleanly. Once that worker calls `mark_done`, the ledger flips `shooting_hub` to `done` and records its `files_written` + `exports`. Any worker’s next loop calls `refresh_blocked()`, promotes `repulsor_blast` to `pending`, and the next atomic claim wins it — possibly the same worker that just finished, possibly a different one that was idle. **No worker dead-spins on a specific dependency**, and no agent-to-agent chat exists; the board is the single source of truth.

The two illustrative `generate` tasks each follow the four-step agent process:

- **Shooting Hub task**: reads its metadata (empty `depends_on`, target scene = New York, no linked assets); reads ledger (nothing to wait on; conceptual prerequisites Grab and Equip live in the engine public surface under `engine/runtime/include/ptv/api/`); may web-search **without a cap** for VR shooting-hub design patterns; writes the C++ files needed (one or several) under `engine/generated/mechanics/<project_id>/`, registers itself with `PTV_REGISTER_MECHANIC`, listens for the correct trigger only when the bound entity has the `weapon` tag and is currently held or worn, forwards trigger events to whichever generated mechanic has registered a `weapon_profile_id` against the same entity.
- **Repulsor Blast task**: reads its metadata (`depends_on: ["shooting_hub"]`, linked assets are the left and right gauntlet GLBs, target scene = New York); reads the ledger entry for `shooting_hub` and pulls its **`exports`** (registration symbols, `weapon_profile_id` registration API, trigger callback signature) — this is just a read, not a wait, because the board already gated the claim; may web-search **without a cap** to research what the canonical repulsor looks and behaves like in VR, palm-aligned beam direction, color/motion intent, charge/cooldown timing; writes the multi-file profile under `engine/generated/mechanics/<project_id>/` that registers `iron_man_repulsor` against the hub, matches trigger side to equipped side, spawns the beam from the palm anchor along the gauntlet’s forward axis, applies the canonical gold/blue-white VFX, respects any charge / cooldown.

The same pattern scales to any future game: one Shooting Hub task, many parallel weapon-profile tasks (guns, repulsors, bows, magic wands, energy waves), all coordinated through the same board with `depends_on: ["shooting_hub"]`, without ever regenerating locomotion, snap turn, grab, or equip per prop.

**Again: this PR does not implement Equip, Shooting Hub, or Repulsor Blast.** The above is to make sure the architecture you build supports this exact shape of work the next time a real prompt asks for it.

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

**Pipeline order:** `scene_spec` runs **before** `code_generation` in `orchestrator.py` — load every `scene_spec*.json` for agent context per the four-step process.

### Goals (all required)

**G1 — `codegen_max_parallel_agents`**

- `Settings.codegen_max_parallel_agents: int = 3`
- Env: `CODEGEN_MAX_PARALLEL_AGENTS`
- `ThreadPoolExecutor(max_workers=N)`
- Each thread runs the **worker loop** below until no `pending` task remains

**G2 — One mechanic per worker at a time**

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
- `CodegenBoard.refresh_blocked()` called by each worker at the start of every loop iteration AND inside `mark_done` (belt-and-suspenders) so promotions happen as soon as any worker is next able to claim

**G5 — FCFS among eligible tasks**

- Stable order = **MechanicPlan item order** (first `pending` in plan order wins next claim)

**G6 — Observability**

- `ctx.emit(ProjectStage.code_generation, ...)` SSE events for: `board_init`, `task_claimed`, `task_unblocked` (`blocked`→`pending`), `task_completed`, `task_failed`
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
  ├─ Load every scene_spec*.json → scene_context (per four-step agent process)
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
  │           # one SDK Runner.run per task; web search enabled; no cap
  │           # agent follows the four-step process
  │       5. for each file in result.files:
  │           filter_cpp_only → repair_generated_cpp → assert_path_allowed → write
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
    "shooting_hub": {
      "mechanic_plan_item_id": "shooting_hub",
      "status": "pending",
      "depends_on": [],
      "agent_id": null,
      "claimed_at": null,
      "completed_at": null,
      "files_written": [],
      "exports": {
        "registered_mechanic_id": "generated.shooting_hub",
        "weapon_profile_macro": "PTV_REGISTER_WEAPON_PROFILE"
      },
      "error": null
    },
    "repulsor_blast": {
      "mechanic_plan_item_id": "repulsor_blast",
      "status": "blocked",
      "depends_on": ["shooting_hub"],
      "agent_id": null,
      "files_written": [],
      "exports": {},
      "error": null
    }
  }
}
```

(The ids above are illustrative — do not hard-code them. The board must be built from the actual MechanicPlan items that carry a `code_task`.)

**Status transitions (enforce in code):**

- `blocked` → `pending` when all `depends_on` are `done`
- `pending` → `in_progress` on successful atomic claim
- `in_progress` → `done` on success | `failed` on error (do not leave tasks stuck in `in_progress`)

**Resume policy:** If board exists, skip tasks already `done`; rebuild any missing rows from MechanicPlan; document `failed` retry behavior in `docs/codegen-agents.md`.

### `depends_on` source of truth

- Structured field on `CodeTask` only — board must **not** infer deps from free text
- Update `MECHANIC_PLAN_SYSTEM` so the planner sets `depends_on` for generated tasks (e.g. weapon profiles depend on `shooting_hub`, additional behaviors depend on their shared helper task)
- Fallback: empty `depends_on` → task starts as `pending`

### `atomic_claim_next_pending(agent_id)` algorithm

1. Acquire project-scoped lock (file lock under `projects/<project_id>/` or equivalent)
2. `refresh_blocked()`
3. Iterate tasks in MechanicPlan order
4. First with `status == "pending"`: set `in_progress`, `agent_id`, `claimed_at`; persist atomically (temp file + rename); return task
5. Else return `None`

Must be correct under `max_agents=3` concurrent threads. Unit-tested.

### New module

`apps/orchestrator/ptv_orchestrator/pipeline/codegen_board.py` + unit tests (no network).

---

## Part B — OpenAI Agents SDK (ChatGPT only, web search, three concurrent agents)

### Official resources (verify live before coding)

1. <https://developers.openai.com/api/docs/guides/agents>
2. <https://github.com/openai/openai-agents-python>

Treat these as authoritative for: package install name and version, `Runner` / `Agent` API, how to enable hosted **web search**, supported model ids, structured-output options, and tool-call loop semantics. Search the web for any details these pages don’t cover before changing code.

### What “three concurrent agents” means

| Setting | Meaning |
|---------|---------|
| `CODEGEN_MAX_PARALLEL_AGENTS=3` | Up to **three CodeTasks** may be `in_progress` simultaneously |
| Per worker | **One** Agents SDK `Runner.run` (or equivalent) **per claimed CodeTask**, then return to queue and claim the next |
| Per agent run | **Unbounded** tool calls inside the agent loop (no cap on web searches) |
| Not | One SDK session for all 10 mechanics; not three files in parallel inside one task unless the agent chooses that in its own loop |

The Code Task stage runs three OpenAI Agents SDK agents at once — each one independently working a different mechanic — and each one has the **web search** hosted tool enabled.

### Implementation requirements

**B1 — Dependency**

- Add the OpenAI Agents SDK to `apps/orchestrator/pyproject.toml` (pin version after verifying current docs)

**B2 — Settings / env**

- `OPENAI_API_KEY` required for this stage
- Model: `settings.openai_hard_model` (default `gpt-5`) or a documented compatible model
- `codegen_max_parallel_agents` (default 3)
- Document in `.env.example`

**B3 — Agent per claimed task**

- **Instructions:** from the updated `CODE_TASK_SYSTEM` — must include the four-step process, the multi-file expectation, the hub/profile pattern, allowed includes, dependency-exports usage, and the no-hallucinated-headers rule
- **Input context (user message):**
  - Full `CodeTask` JSON (including `depends_on` and any planner `files[]` hints)
  - Ledger snapshot: all tasks with `status`, `files_written` for `done` tasks
  - `exports` from each completed `depends_on` task
  - Every relevant `scene_spec*.json` excerpt: entity ids, tags, `mechanic_bindings`, `MeshRef` paths
  - BuildSpec mechanic summary excerpt as needed
- **Tools:** enable the SDK **web search** hosted tool. **Do not set a cap** on the number of search calls per CodeTask.
- **Output format (structured JSON preferred):**

```json
{
  "files": [
    { "path": "engine/generated/mechanics/<project_id>/<name_a>.cpp", "contents": "..." },
    { "path": "engine/generated/mechanics/<project_id>/<name_b>.cpp", "contents": "..." }
  ],
  "exports": {
    "registered_mechanic_id": "generated.<id>",
    "weapon_profile_id": "<optional>"
  }
}
```

**B4 — Recursive / multi-step behavior inside one CodeTask**

Instruct the agent to:

1. Analyze description, deps, scene context, completed ledger entries (four-step process step 1 + 2)
2. Plan file split (≤16 files) with path + purpose per file
3. Optionally call **web search** (no cap) for domain / VR / visual research (step 3)
4. Implement files across multiple turns of the agent loop (step 4)
5. Ensure registration consistency across files (one `PTV_REGISTER_MECHANIC` per mechanic; profile registrations consistent across files)
6. Return structured `files` + `exports` for the board

**B5 — Web search guardrails (quality, not quantity)**

- **No numeric cap on calls.** The cap is implicit in the agent’s own loop budget.
- Use search results for **research only**; the final artifact is C++ under `engine/generated/`
- **Reject** Unity / Unreal / Godot / browser-VR / WebXR implementation advice
- OpenXR / Quest 3 / Vulkan / native C++ references are fine

**B6 — No Claude on this path**

- `stage_code_generation` must not call `make_llm()` / Anthropic for mechanic generation
- Other stages unchanged unless shared utilities are needed

### New module

`apps/orchestrator/ptv_orchestrator/pipeline/codegen_agent.py` — SDK wrapper; mockable for tests; exposes a single async/sync entry the worker loop calls per claimed task.

---

## Part C — Multi-file `CodeTask.files[]` (Problem 2)

### Requirements

**C1 — `files[]` is the sole write contract**

- Do **not** write a single fixed `{task_id}.cpp` unless the agent returns exactly one file in `files[]`
- After SDK run: populate `CodeTask.files` with `CodeTaskFile(path, contents)` and persist via the existing schema
- Limits per schema: ≤16 files, ≤120,000 characters per `contents`, path ≤256 chars
- Paths must start with `engine/generated/{mechanics|ai|dialogue}/` and include `<project_id>/`

**C2 — Design for real VR mechanics (not one blob)**

The agent must split work by separable concern. Examples of natural splits:

| Concern | Example file |
|---------|--------------|
| Registration & metadata | `<id>_register.cpp` |
| Per-tick simulation | `<id>_simulation.cpp` |
| Weapon profile / config | `<id>_profile.cpp` |
| VFX hookup | `<id>_vfx.cpp` |
| Math / helpers private to this mechanic | `<id>_math.cpp` |

The agent is not required to use these specific names — it must pick names that match the mechanic. It is required to **not** dump every concern into a single `.cpp`.

**C3 — Per-file write pipeline**

For each `files[]` entry:

`filter_cpp_only(contents)` → `repair_generated_cpp` → `assert_path_allowed` → write to disk → append path to board `files_written` for the task.

**C4 — Single-file fallback**

If the mechanic genuinely fits in one file, the agent may return one entry in `files[]`. The pipeline writes exactly that file. Do not force a split when no concern separates.

**C5 — `test_files[]`**

**Still completely ignored.** No reads, no writes, no CMake test targets, no planner prompts for tests, no schema work for tests in this PR.

### Prompt updates (`apps/orchestrator/ptv_orchestrator/pipeline/prompts.py`)

This is part of Solution 2 for Problem 2. The implementing agent must **read every system prompt in `prompts.py` that affects the code task stage** and update it so:

**`CODE_TASK_SYSTEM` must:**

- **Remove** any language that tells the agent it must produce a single C++ file (e.g. “write one .cpp”, “only one file”, “a single self-contained source file”)
- **Remove** any per-file line cap or character cap framed as a constraint on the agent (the schema cap is large; do not pre-emptively shrink it in the prompt)
- **Add** explicit instruction that the agent should produce **multiple** files via `CodeTask.files[]` whenever the mechanic has separable concerns, and that **single-file output is a fallback, not the default**
- **Add** explicit instruction to follow the **four-step process** (read metadata → read deps & ledger → optional web search with no cap → multi-file C++ implementation)
- **Add** explicit instruction to register one `PTV_REGISTER_MECHANIC` per generated mechanic across the file set, and to consume `exports` from `depends_on` tasks rather than re-implementing them
- **Add** explicit instruction that the agent may call **web search** without a cap during step 3

**`MECHANIC_PLAN_SYSTEM` must:**

- Set `depends_on` on generated tasks where appropriate (e.g. weapon profiles depend on a shooting hub task in the same plan)
- Encourage `builtin+config` for grab, equip, and shooting-hub-style mechanics when the catalog covers them
- In `code_task.description`, list integration points (asset ids, trigger bindings, profile id, scene id)
- Optional: suggest a file-split outline in `description` (planner hint; the agent finalizes the actual split)
- **Not** instruct the planner to cap `files[]` below the schema maximum (16) or to prefer single-file outputs

If any other prompt in `prompts.py` (e.g. helpers used by codegen) carries similar “one file” / “short file” language, update it the same way.

---

## Part D — Engine reality & do not over-promise in generated code

Read before writing prompts that imply full Quest behavior:

| Component | Status today |
|-----------|----------------|
| Grab + tag `grabbable` | **Works** — `Application.cpp` grip loop |
| `builtin.grab` | Registers `CGrabbable`; grab logic itself lives in `Application.cpp` |
| **Equip** on body slots | **Not fully implemented** — plan via `builtin+config` + SceneSpec; **engine follow-up**, not part of this PR |
| **Shooting hub + live trigger → held/equipped object** | **Not fully implemented** — `builtin.projectile_hook` is a stub; no input bridge to `IMechanic::on_update`. **Engine follow-up**, not part of this PR |
| `MechanicHost::update` | Runs generated `on_update`, but without live OpenXR trigger state |

**This PR delivers:** ledger + parallel Agents SDK + multi-file + planner / schema / prompt updates.

**Generated code may:** use `params` for simulated trigger / equip state (`primary_trigger`, `grip_held`, `equipped_side`) with `TODO` comments noting that the live input bridge will be added in engine follow-up work.

**Document gaps** in `docs/codegen-agents.md` under “Engine follow-up.” Do not block this PR on Equip / Shooting Hub / Repulsor mechanics — they are illustrative reference workflows, not deliverables.

---

## Part E — Schema, prompts, docs, tests, acceptance

### Schema changes

- Add `depends_on: List[IdStr] = Field(default_factory=list)` to `CodeTask` in `packages/schemas/ptv_schemas/code_task.py`
- Export: `python -m ptv_schemas.export packages/schemas/json`
- Do **not** add test file handling
- Do **not** lower the existing `files[]` cap (16) or per-file character cap

### Settings / `.env.example`

```env
CODEGEN_MAX_PARALLEL_AGENTS=3
OPENAI_API_KEY=sk-...
OPENAI_HARD_MODEL=gpt-5
```

### Prompts (`apps/orchestrator/ptv_orchestrator/pipeline/prompts.py`)

Apply the updates described in [Part C — Prompt updates](#prompt-updates-appsorchestratorptv_orchestratorpipelinepromptspy). The agent system prompts must:

- Mandate the four-step process
- Allow and encourage multiple files via `files[]`
- Never pre-emptively cap output to one file or to a fixed length below the schema cap
- Mention web search is enabled with no cap
- Reference the engine reality and the dependency-exports pattern

### Documentation (required)

Add **`docs/codegen-agents.md`** (or extend `docs/prompt-schema.md`) covering:

- The four-step CodeTask agent process (read metadata → read deps & ledger → optional web search with no cap → multi-file C++)
- Ledger path + status machine diagram (`blocked` / `pending` / `in_progress` / `done` / `failed`)
- FCFS + atomic claim semantics
- `depends_on` field
- OpenAI Agents SDK + web search (no cap)
- Multi-file `files[]` workflow
- The illustrative Iron Man / Shooter City reference workflows and the explicit note that Equip / Shooting Hub / Repulsor are **not** implemented in this PR
- **`test_files[]` unused**
- Engine gaps (equip, weapon_fire, input bridge) and how generated code should currently stub them via `params`
- Engine growth loop (`engine/promoted/registry.json`) and how today’s `generate` becomes tomorrow’s `promoted.<id>` dep

### Tests (no network)

| Test | Purpose |
|------|---------|
| Board: `blocked` not claimable until dep `done` | 40.3 |
| Dep `done` → `refresh_blocked` → claimable | 40.3 |
| Two threads cannot claim the same task | Atomic claim |
| FCFS order matches MechanicPlan | 40.1 |
| Mock SDK returns 3 files → 3 paths on disk, bounds applied | Part C |
| Mock SDK returns 1 file → 1 path on disk | Part C fallback |
| Mock SDK with `depends_on` integration test | End-to-end board |
| Stage test mocks `codegen_agent`; verifies four-step agent context (metadata, deps, scene_spec, web-search tool enabled) is wired | Wiring |
| `CODE_TASK_SYSTEM` does not contain phrases like “one file” / “single self-contained source file” / fixed line cap | Prompt regression |

### Acceptance checklist

- [ ] `CODEGEN_MAX_PARALLEL_AGENTS=3` runs up to three Agents SDK sessions on distinct `pending` tasks
- [ ] `codegen_board.json` shows `agent_id`, statuses, `files_written`, `exports`  
- [ ] Task B with `depends_on: ["A"]` stays `blocked` until A is `done`, then becomes `pending`
- [ ] No duplicate claims under parallel load
- [ ] Each task uses the **OpenAI Agents SDK** with the **web search** hosted tool enabled
- [ ] **No web-search cap** is set per CodeTask
- [ ] Complex mechanic writes **multiple** `.cpp` via `files[]` (not only `{id}.cpp`)
- [ ] Every relevant `scene_spec*.json` is loaded into agent context
- [ ] `CODE_TASK_SYSTEM` and any related prompts no longer require single-file output or cap line counts
- [ ] `CodeTask.test_files[]` still unused
- [ ] Codegen does **not** call Claude
- [ ] No second orchestrator service  
- [ ] Existing `pytest` passes + new tests green
- [ ] Equip / Shooting Hub / Repulsor mechanics are **not** added to the engine in this PR (illustrative only)

---

## Files to read first

| File | Why |
|------|-----|
| `apps/orchestrator/ptv_orchestrator/pipeline/stage_code_generation.py` | Integration point |
| `apps/orchestrator/ptv_orchestrator/pipeline/prompts.py` | `CODE_TASK_SYSTEM`, `MECHANIC_PLAN_SYSTEM` — must be updated per Part C |
| `apps/orchestrator/ptv_orchestrator/pipeline/bounds.py` | Per-file safety (`filter_cpp_only`, `repair_generated_cpp`, `assert_path_allowed`) |
| `apps/orchestrator/ptv_orchestrator/pipeline/orchestrator.py` | Stage order: `scene_spec` before `code_generation` |
| `apps/orchestrator/ptv_orchestrator/settings.py` | New settings (`codegen_max_parallel_agents`, OpenAI model ids) |
| `packages/schemas/ptv_schemas/code_task.py` | `files[]`, add `depends_on` |
| `packages/schemas/ptv_schemas/scene_spec.py` | `MechanicBinding.params` shape for `builtin+config` parameters |
| `engine/runtime/src/core/Application.cpp` | Grab, trigger (engine context only — do not edit) |
| `engine/runtime/src/mechanics/BuiltinMechanics.cpp` | Builtin stubs (context only) |
| `apps/orchestrator/ptv_orchestrator/engine_catalog.py` | Valid `builtin.*` ids + `promoted.*` ids exposed to the planner |
| `apps/orchestrator/pyproject.toml` | Add Agents SDK dependency |

---

## Implementation order

1. **`codegen_board.py`** + `depends_on` on `CodeTask` + schema export + board unit tests (**40.3 mandatory**)
2. **`codegen_agent.py`** — OpenAI Agents SDK + web search (no cap) + structured multi-file JSON output, mockable for tests
3. **Multi-file write path** + update **`prompts.py`** (`CODE_TASK_SYSTEM`, `MECHANIC_PLAN_SYSTEM`) per Part C
4. **Rewrite `stage_code_generation.py`** — board + thread pool + agent + per-file bounds, following the four-step agent process
5. **Settings**, `.env.example`, **`docs/codegen-agents.md`**
6. **Manual smoke:** project with ≥2 `generate` tasks where task B `depends_on` task A; verify board states, multiple `.cpp` files per task, and that the agent context carried metadata, deps, and `scene_spec*.json`

---

## PR guardrail (paste into PR description)

> **Incomplete** if this PR: (1) adds parallel workers without `blocked` / `depends_on` and `refresh_blocked`, (2) does not use **OpenAI Agents SDK** with **web search (no cap)** for codegen, (3) still writes only one `.cpp` per CodeTask while `CodeTask.files[]` remains unused, (4) leaves `CODE_TASK_SYSTEM` (or any sibling prompt) telling the agent to produce a single `.cpp` or to cap output below the schema limit, or (5) does not encode the **four-step agent process** in agent instructions and the worker driver.
>
> **`test_files`** must remain unused.
>
> Equip / Shooting Hub / Repulsor mechanics must **not** be added to the engine here — the Iron Man-style reference workflow is illustrative only.

---

## One-line completion rule for the implementing agent

> If the implementation has parallel workers but no `blocked` status or `depends_on` handling, uses plain `LlmClient.generate_code` instead of OpenAI Agents SDK for the codegen stage, does not write multiple files per CodeTask via `files[]`, leaves single-file or line-cap language in `CODE_TASK_SYSTEM`, caps web search per task, or fails to drive every agent run through the four-step process (metadata → deps & ledger → optional web search → multi-file C++), the task is **not complete**.
