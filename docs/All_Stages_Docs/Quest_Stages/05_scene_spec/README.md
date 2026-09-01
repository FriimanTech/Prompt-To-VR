> **Shared content:** Identical to [PC_VR_Stages/05_scene_spec](../../PC_VR_Stages/05_scene_spec/README.md). Update both copies when this stage changes.

# Stage 5: SceneSpec

> **Pipeline position:** after `mechanic_plan` → before `reference_images`  
> **Target:** shared (PC VR and Quest)  
> **Primary code:** [`apps/orchestrator/ptv_orchestrator/pipeline/stage_scene_spec.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/stage_scene_spec.py)  
> **Primary artifacts:** `projects/<project_id>/scene_spec.<scene_id>.json`, `scene_spec.json`

## Main stage summary

### What this stage is

The **SceneSpec stage** composes one structured scene document per game scene: which entities spawn where, which assets and mechanics they use, player spawn, environment tags, and NPC setup (health, faction, locomotion). It produces `scene_spec.<scene_id>.json` files plus a canonical entry-point copy.

### Why it exists

BuildSpec lists scenes at a high level; MechanicPlan lists mechanics. SceneSpec is where those are **wired together** into something the deterministic scene compiler can merge with the C++ world and GLB assets.

Without schema validation here, the codegen agent and scene compiler would see inconsistent entity lists.

### Inputs (what enters)

| Input | Location |
|-------|----------|
| BuildSpec | `build_spec.json` |
| AssetManifest | `asset_manifest.json` |
| MechanicPlan | `mechanic_plan.json` |
| NpcPlan | `npc_plan.json` |

### Outputs (what it produces)

| Output | Location |
|--------|----------|
| Per-scene specs | `scene_spec.<scene_id>.json` |
| Entry copy | `scene_spec.json` (entry-point scene) |

### Where output goes next

- **`reference_images` / `tripo_models`** — indirect (assets referenced by id)
- **`asset_optimization`** — instance counts for triangle budgeting
- **`code_generation`** — scene context for agents
- **`scene_compile`** — primary consumer

### Key concepts and terms

- **Validate-and-repair loop** — After the agent emits a SceneSpec, deterministic checks run (asset coverage, valid mechanic binding ids, environment presence). Problems are fed back to the **same agent session** for repair, up to `scene_spec_max_repair_rounds`.
- **Parallel workers** — Multiple scenes compose concurrently via `ThreadPoolExecutor` (capped by `scene_spec_max_parallel_workers`).
- **Compressed context** — Each scene agent gets a ~5–8K token summary instead of the full multi-scene dump (~39K).
- **Idempotent resume** — If valid `scene_spec.*.json` already exist on disk, stage may skip LLM calls.

### End-to-end flow (plain language)

1. For each scene in BuildSpec, spawn a **SceneSpecComposer** agent (`gpt-5.5`).
2. Agent produces SceneSpec JSON.
3. `_validate_scene_semantics()` checks coverage and bindings.
4. If invalid, feed errors back and repair (same session).
5. Save per-scene files; copy entry scene to `scene_spec.json`.

---

## Detailed stage deep dive

### Parallelism and fast-path resume

This stage is one of the most complex planners. It can **skip LLM entirely** if valid `scene_spec.<scene_id>.json` files already exist on disk.

### Entry: `run_with_composer_factory`

1. emit + record running
2. Load BuildSpec
3. **`_try_load_existing_scene_spec()` per scene** — if ALL scenes valid on disk → `_finalize_all_existing()` and return (no API)
4. Load manifest, mechanic_plan, npc_plan; bridge OpenAI key
5. `_StaticContext.build()` — compresses assets/mechanics/NPCs (~5–8K tokens per scene vs ~39K dump)
6. `ThreadPoolExecutor(max_workers=scene_spec_max_parallel_workers)` — default 3 parallel scene agents

### Per-scene worker

For each BuildSpec scene id:

- If file exists and validates → reuse + `_ensure_entrypoint_canonical()`
- Else `_compose_one_scene()` with `_Heartbeat` (SSE every 30s during long calls)
- **Save `scene_spec.<scene_id>.json` immediately** after each scene — crash-safe partial progress

### Validate-and-repair loop (`_compose_one_scene`)

```
for attempt in 0..scene_spec_max_repair_rounds:
    scene = composer.compose(initial_message OR build_repair_message(issues))
    issues = _validate_scene_semantics(scene, manifest, plan, ...)
    if issues empty: return scene
    else if attempts left: continue same SQLiteSession
    else: raise SceneSpecAgentError
```

**Validation checks:** matching scene_id, non-empty entities, every manifest asset referenced, environment entity tag present, mechanic binding ids exist in MechanicPlan.

### SceneSpecComposer agent

- OpenAI Agents SDK + **SQLiteSession** (repair context preserved across rounds)
- Model: `scene_spec_model` (gpt-5.5), reasoning medium
- No WebSearch

### Outputs

| File | Purpose |
|------|---------|
| `scene_spec.<scene_id>.json` | Per-scene gameplay layout |
| `scene_spec.json` | Copy of entry-point scene (or sole scene) |

Pre-computed `glb_path`: `assets/<asset_id>.glb` injected in agent message.

### SSE events

`running`, `info` (parallelism, per-scene start/done/reuse, repair round N), heartbeat `info`, `succeeded`

### Failure modes

Worker exception fails whole stage. Validation exhaustion → SceneSpecAgentError.

### Settings

`scene_spec_model`, `scene_spec_max_parallel_workers` (3), `scene_spec_max_repair_rounds` (2), `scene_spec_agent_max_turns`

### `_validate_scene_semantics()` checks (deterministic)

| Check | Failure theme |
|-------|---------------|
| `scene.id` matches BuildSpec | Wrong scene file |
| `entities` non-empty | Empty scene |
| Every manifest asset id on some entity | Coverage gap |
| Environment anchor entity present | Missing world hook |
| Mechanic binding ids exist in plan | Invalid binding |

Non-empty issues → repair via `build_repair_message()` in same SQLiteSession.

### Parallel worker isolation

Each scene gets its own `SceneSpecComposer` + session id. Workers share read-only `_StaticContext` only. Thread pool collects results in BuildSpec scene order.

### `_StaticContext` compression

Full manifest/plan dump (~39K tokens) compressed to ~5–8K per scene: asset id/category/glb_path, mechanic summaries, NPC role/faction/movement.

### Repair issue examples

Missing manifest asset references, invalid mechanic binding ids, missing environment tag, empty entities — all fed back via `build_repair_message()` into same SQLiteSession.

### Crash-safe saves

Each scene written to disk immediately after compose — resume after crash may have partial scene files; fast-path loads valid ones only.

---

## Extend this folder

- `failure-modes.md` — validation rules and repair messages
- `schemas.md` — entity and spawn fields
- `artifacts.md` — per-scene file layout

## See also

- [`docs/architecture.md`](../../../architecture.md) — data flow diagram
- Stage 15 [`15_scene_compile`](../15_scene_compile/README.md) — deterministic merge
