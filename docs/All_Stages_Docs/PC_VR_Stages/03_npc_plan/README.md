# Stage 3: NPC Plan

> **Pipeline position:** after `asset_manifest` → before `mechanic_plan`  
> **Target:** shared (PC VR and Quest)  
> **Primary code:** [`apps/orchestrator/ptv_orchestrator/pipeline/stage_npc_plan.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/stage_npc_plan.py)  
> **Primary artifacts:** `projects/<project_id>/npc_plan.json` (may also update `asset_manifest.json`)

## Main stage summary

### What this stage is

The **NPC plan stage** reads your prompt and decides what every **character asset** is supposed to *do*: role, faction, movement style, combat stats, perception, spawn rules, and which animation intents their AI behaviors need. It outputs `npc_plan.json`.

### Why it exists

Placing a rigged GLB in a scene does not make an enemy. Without this stage, the manifest might rig a duck that the prompt says should **slide without animation** (~30 Tripo credits wasted). Mechanic planning and scene composition also need to agree on who fights whom before C++ is generated.

This stage runs **before rigging** because animation is expensive and only the prompt can answer "does this thing animate?"

### Inputs (what enters)

| Input | Location |
|-------|----------|
| User prompt (verbatim) | `projects/<id>/prompt.txt` |
| BuildSpec | `build_spec.json` |
| AssetManifest | `asset_manifest.json` (character items only) |

### Outputs (what it produces)

| Output | Location |
|--------|----------|
| NpcPlan | `projects/<id>/npc_plan.json` |
| Reconciled manifest (optional) | `asset_manifest.json` — `rigged` / `animations` patched |

### Where output goes next

- **`mechanic_plan`** — knows combatants and behavior needs
- **`scene_spec`** — NPC placement, factions, locomotion components
- **`code_generation`** — AI behavior tasks reference npc_plan
- **`asset_rigging` / `asset_animation`** — see reconciled rig flags

### Key concepts and terms

- **NpcSpec** — One NPC definition tied to an `asset_id` from the manifest.
- **Movement style** — e.g. `animated_locomotion` vs `slide_no_animation`. Slide means no Tripo rig/clip billing.
- **Reconciliation** — The plan is authoritative: if director says ducks slide, manifest `rigged: false` and empty `animations`.
- **Graceful degradation** — If the NPC director agent fails, an empty plan is saved and the pipeline continues (pre-stage-3 behavior).

### End-to-end flow (plain language)

1. Collect character-category assets from the manifest.
2. If none → save empty `NpcPlan`, skip LLM, succeed.
3. Otherwise run **NpcDirector** (OpenAI Agents SDK, `gpt-5.5`) with prompt + BuildSpec + manifest.
4. Drop entries for unknown asset ids.
5. Reconcile manifest rig/animation flags to match the plan.
6. Save `npc_plan.json`.

---

## Detailed stage deep dive

### Why this stage sits between manifest and mechanic_plan

Rigging costs ~30 Tripo credits per asset; each animation ~20. Only the **verbatim prompt** reliably answers whether a character should walk or slide. The manifest LLM might set `rigged: true` on everything; without reconciliation you pay for unused skeletons.

### Entry points

- Production: `run(ctx)` calls `run_with_director(ctx, director=None)`
- Tests: inject a stub `NpcDirector` to avoid OpenAI

### Exact execution trace

| Step | Function / action |
|------|-------------------|
| 1 | emit `running` + `record_stage(running)` |
| 2 | Load `build_spec`, `asset_manifest` |
| 3 | `candidates = manifest items where category == character` |
| 4 | If no candidates: empty `NpcPlan`, save, succeed — **no API call** |
| 5 | Build `NpcDirector` with `NPC_PLAN_SYSTEM`, `npc_plan_model`, reasoning effort, `session_id=ptv-npc-plan-<project_id>` |
| 6 | `NpcPlanAgentInput` + `build_director_message()` — includes prompt (max 20k via `_read_prompt()`), BuildSpec JSON, manifest JSON |
| 7 | `director.run(message)` returns validated `NpcPlan` |
| 8 | On `NpcPlanAgentError`: empty plan, emit `warning`, log — **stage still succeeds** |
| 9 | `_drop_unknown_assets()` removes specs for ids not in manifest; emit `warning` per drop |
| 10 | `save_npc_plan()` writes `npc_plan.json` |
| 11 | `_reconcile_manifest()` may rewrite `asset_manifest.json` rig/animation flags |
| 12 | emit `succeeded` |

### `_reconcile_manifest()` (deterministic)

Matches each character `AssetItem` to an `NpcSpec`:

- `slide_no_animation` → force `rigged=false`, clear `animations[]`
- Animated styles → align `rigged` and `animations[]` with plan intents

**npc_plan is authoritative** over manifest before stage 8.

### NpcDirector (`npc_plan_agent.py`)

- OpenAI Agents SDK: `Agent` + `Runner.run_sync`
- Structured output: `NpcPlan` via `AgentOutputSchema`
- **No tools**
- Default model: `gpt-5.5`, reasoning `high`

### Artifact I/O

| Read | Write |
|------|-------|
| `prompt.txt`, `build_spec.json`, `asset_manifest.json` | `npc_plan.json`, optionally updated `asset_manifest.json` |

### SSE

`running` → `warning` (agent fail / dropped ids) → `info` (reconcile) → `succeeded`

### Failure modes

| Case | Result |
|------|--------|
| Agent error | Empty plan, success |
| No characters | Empty plan, no API |
| Bad asset id in plan | Dropped with warning |

### Settings

`npc_plan_model`, `npc_plan_reasoning_effort`, `npc_plan_agent_max_turns`

### NpcSpec downstream usage

`faction`, `movement_style`, `behaviors[]`, `perception`, and `combat` flow into scene_spec, codegen context, and runtime services (`ctx.combat`, `ctx.nav`, `builtin.character_locomotion`).

### Graceful degradation rationale

Pipeline worked before npc_plan existed; agent failure must not brick projects — empty plan falls back to manifest-only rig flags.

---

## Extend this folder

- `artifacts.md` — NpcSpec fields and manifest patches
- `schemas.md` — movement styles and animation intents
- `failure-modes.md` — graceful degradation paths

## See also

- [`docs/npc-and-ai.md`](../../../npc-and-ai.md) — runtime NPC systems
- [`docs/architecture.md`](../../../architecture.md) — combat / perception services
