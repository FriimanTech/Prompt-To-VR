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

- **Validate-and-repair loop** — After the agent emits a SceneSpec, deterministic checks run (asset coverage, valid mechanic binding ids). Problems are fed back to the **same agent session** for repair, up to `scene_spec_max_repair_rounds`. There is no "environment presence" check: the environment is authored in C++ by the world architect and merged in by the scene compiler.
- **Parallel workers** — Multiple scenes compose concurrently via `ThreadPoolExecutor` (capped by `scene_spec_max_parallel_workers`).
- **Compressed context** — Each scene agent gets a ~5–8K token summary instead of the full multi-scene dump (~39K).
- **Idempotent resume** — If valid `scene_spec.*.json` already exist on disk, stage may skip LLM calls. "Valid" includes **asset coverage against the current manifest**, not just schema validity.
- **Equip slots** — An entity can declare `equip_slots[]`: named places on a *wearer* (a hip holster, a back sheath) that `builtin.equip` snaps items into.

### End-to-end flow (plain language)

1. For each scene in BuildSpec, spawn a **SceneSpecComposer** agent (`gpt-5.6-sol`).
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

**Validation checks:** matching scene_id, non-empty entities, every manifest asset referenced, mechanic binding ids exist in MechanicPlan.

### SceneSpecComposer agent

- OpenAI Agents SDK + **SQLiteSession** (repair context preserved across rounds)
- Model: `scene_spec_model`, effort `scene_spec_reasoning_effort`, verbosity `scene_spec_verbosity`, explicit `scene_spec_max_output_tokens`
- No WebSearch

### Strict structured output

The agent's `output_type` is **not** `SceneSpec` but `WireSceneSpec`
(`pipeline/scene_spec_wire.py`) with `strict_json_schema=True`, mapped back to
`SceneSpec` by `to_scene_spec()`. OpenAI strict mode then constrains decoding,
so malformed JSON and ids that violate the `IdStr` pattern become impossible
rather than merely discouraged.

`SceneSpec` itself cannot be the wire schema: strict mode requires
`additionalProperties: false` everywhere, and `MeshRef.sockets` and
`MechanicBinding.params` are open-ended dicts. The wire model drops `sockets`
(the scene compiler fills them from `asset_contract.json`) and carries
`params` as a list of `(key, value_json)` pairs.

Non-strict output caused a real ten-minute triple failure: the SDK redacts
model payloads by default, so a Pydantic validation error arrived as the single
line `Invalid JSON when parsing model output`, the repair turn had nothing to
say, and the agent repeated the same mistake until the budget ran out.
`agent_output_capture.make_capturing_schema` now keeps the raw output, which is
written to `logs/scene_spec.log` and turned into field-level repair
instructions. Truncation at the token cap takes a separate branch that asks for
FEWER entities instead of "fix these problems".

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

`scene_spec_model`, `scene_spec_reasoning_effort`, `scene_spec_verbosity`, `scene_spec_max_output_tokens` (64000), `scene_spec_max_entities` (120), `scene_spec_max_parallel_workers` (3), `scene_spec_max_repair_rounds` (2), `scene_spec_agent_max_turns`

### `_validate_scene_semantics()` checks (deterministic)

| Check | Failure theme |
|-------|---------------|
| `scene.id` matches BuildSpec | Wrong scene file |
| `entities` non-empty | Empty scene |
| Every manifest asset id on some entity | Coverage gap |
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

### Reuse must match the CURRENT manifest

`_try_load_existing_scene_spec()` used to check only the Pydantic schema and the scene id. A scene composed against an earlier manifest therefore sailed through the fast path: one real run reported "Reused 1 scenes already on disk" for a scene naming eight assets that no longer existed, so eight meshes pointed at GLBs that were never generated and the two weapons the game *did* have appeared nowhere.

Reuse now also requires every `mesh.asset_id` to exist in the current `AssetManifest`. A rejected scene is recomposed, and the reason is emitted as an `info` event naming the absent ids, so "why did it re-run" has an answer on screen instead of in a diff. `scene_compile` enforces the same invariant as a hard failure, in case a scene reaches it another way.

### Equip slots (`EquipSlotSpec`)

An `EntitySpec` may declare up to 24 `equip_slots`. A slot is a place **on the wearer** — never on the holster prop, which is set dressing:

| Field | Meaning |
|-------|---------|
| `name` | Slot id gameplay refers to (`hip_holster_r`) |
| `body_slot` | Where on the body: `hand_r`, `hand_l`, `waist`, `wrist_l`, `wrist_r`, `back`, `chest`, `head` |
| `host_item_slot` / `host_socket` | For a slot hosted on an already-equipped item |
| `local_position` / `local_rotation` | Offset from the body slot, e.g. `[0.16, -0.05, 0.02]` from `waist` for a right hip |
| `snap_radius_m` | How near an item must come to snap in — 0.15–0.25 for a holster |
| `exclusive` | One item at a time (default true) |
| `accepts_tags` | Which items fit; an empty list lets a rifle into a pistol holster |

The strict wire model (`scene_spec_wire.py`) mirrors the field verbatim. It has to: strict mode permits **only** the declared surface, so a field the wire schema omits is one the agent physically cannot emit.

### The player's bindings

`player_rig` (meshless, tagged `player_rig`) hosts `builtin.hand_pose` and the
locomotion, turning and settings-menu items the MechanicPlan lists -- nothing
else. In Full Player Avatar the `player_avatar` entity binds
`builtin.avatar_body` itself. A Hands Only scene has no avatar entity. The
compiler fills in anything forgotten and rejects contradictions.

---

## Extend this folder

- `failure-modes.md` — validation rules and repair messages
- `schemas.md` — entity and spawn fields
- `artifacts.md` — per-scene file layout

## See also

- [`docs/architecture.md`](../../../architecture.md) — data flow diagram
- Stage 15 [`15_scene_compile`](../15_scene_compile/README.md) — deterministic merge
