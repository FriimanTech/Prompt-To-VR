# Stage 5: SceneSpec

> **Pipeline position:** after `mechanic_plan` → before `reference_images`  
> **Target:** shared (PC VR and Quest)  
> **Primary code:** [`apps/orchestrator/ptv_orchestrator/pipeline/stage_scene_spec.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/stage_scene_spec.py)  
> **Primary artifacts:** `projects/<project_id>/scene_spec.<scene_id>.json` (one per scene), `scene_spec.json` (entry-point copy)

## Main stage summary

### What this stage is

The **SceneSpec stage** composes one structured **gameplay** layout per
`BuildSpec.scenes` entry: which entities exist, where, with which mesh
(asset), physics, tags, health / faction, attachments, equip slots and
mechanic bindings, plus the player spawn, lights, sky colour and ambient
level. Scenes are composed in parallel by an agent with a deterministic
validate-and-repair loop.

The **environment is not composed here**: it is C++ authored later by the
world architect (`world_generation`) and merged in by `scene_compile`. This
stage places gameplay entities only.

### Why it exists

The BuildSpec lists scenes at a high level and the MechanicPlan lists
mechanics; SceneSpec wires assets and mechanics onto concrete entities so the
deterministic scene compiler can merge them with the generated world and the
optimized GLBs.

### Inputs (what enters)

| Input | Location | What the agent sees |
|-------|----------|---------------------|
| BuildSpec | `build_spec.json` | Meta (title, pitch ≤600 chars, game type, locomotion, comfort, player height, constraints), THIS scene in full, other scenes' id + name |
| AssetManifest | `asset_manifest.json` | Per asset: id, name, category, `character_role`, style notes (≤400 chars), target scale, collision shape, `rigged`, pre-computed `glb_path` = `assets/<asset_id>.glb` |
| MechanicPlan | `mechanic_plan.json` | Per item: id, name, type, classification, `binding_id`, config, brief (≤400 chars), `depends_on_assets`, `required_in_scenes` |
| NpcPlan | `npc_plan.json` | Per NPC: role, faction, `hostile_to`, movement style, max health, walk / run / turn speeds, spawn counts and `waves` |

The full multi-scene dump (~39K tokens on a large brief) is compressed to
~5–8K tokens per scene. GameDesign and InteractionDesign are **not** inputs;
the scene compiler binds the game director and weapons from them later.

### Outputs (what it produces)

| Output | Location |
|--------|----------|
| Per-scene spec | `projects/<id>/scene_spec.<scene_id>.json` |
| Entry-point copy | `projects/<id>/scene_spec.json` (the `entry_point` scene, or the only scene) |
| Per-scene agent log | `projects/<id>/logs/scene_spec.log` (lines prefixed `[<scene_id>]`) |

Spec files for scenes the BuildSpec no longer has are deleted
(`run_reset.remove_stale_scene_files`), because later stages glob
`scene_spec*.json`.

### Where output goes next

- **`world_generation`** / **`code_generation`** — scene context for the agents.
- **`asset_optimization`** — instance counts for triangle budgeting.
- **`scene_compile`** — the primary consumer: merges with `world.scene.json`, fills sockets / holds from `asset_contract.json`, merges MechanicPlan config, binds weapons and the game director, validates equip slots and asset references.

### Models / providers

- OpenAI Agents SDK agent (`SceneSpecComposer`), one per scene, `gpt-5.6-sol`, reasoning effort `medium`, verbosity `medium`, `max_tokens` = `scene_spec_max_output_tokens` (64,000), **no tools**.
- Repair rounds reuse the same agent session (`SQLiteSession`, id `ptv-scene-<project_id>-<scene_id>`).
- System prompt `scene_spec_system(target_device, max_entities=scene_spec_max_entities)` — the entity budget and the Quest / PC VR device clause are baked in.
- No other provider; cost scales with the number of scenes not already on disk.

### Failure behaviour

- A scene that still fails schema or semantic validation after `scene_spec_max_repair_rounds` raises `SceneSpecAgentError`; any worker exception **fails the whole stage**. Scenes already saved stay on disk and are reused on Resume.

---

## Detailed stage deep dive

### Files and functions

| File | Role |
|------|------|
| `pipeline/stage_scene_spec.py` | `run` → `run_with_composer_factory(ctx, _default_composer_factory)`; `_try_load_existing_scene_spec`, `_finalize_all_existing`, `_compose_one_scene`, `_validate_scene_semantics`, `_build_user_message`, `_StaticContext`, `_Heartbeat` |
| `pipeline/scene_spec_agent.py` | `SceneSpecComposer`, `SceneSpecAgentError`, `SceneSpecOutputError`, `build_repair_message`, `build_truncation_message` |
| `pipeline/scene_spec_wire.py` | Strict wire schema `WireSceneSpec` and `to_scene_spec()` mapping |
| `pipeline/agent_output_capture.py` | Keeps raw model output and turns validation failures into field-level issues |
| `packages/schemas/ptv_schemas/scene_spec.py` | `SceneSpec`, `EntitySpec`, `MeshRef`, `MechanicBinding`, `EquipSlotSpec`, … |

### Execution trace

1. Emit `running`; load BuildSpec, AssetManifest and MechanicPlan.
2. **Reuse check per scene** (`_try_load_existing_scene_spec`). A file on disk is reused only if it parses as `SceneSpec`, its `scene_id` matches, every `mesh.asset_id` is in the **current** manifest, and every `generated.*` binding is still generated by the **current** plan. A rejected file gets an `info` event saying why ("recomposing, it references 8 asset(s) absent from…").
3. All scenes reusable → `_finalize_all_existing`: re-stamp `scene_spec.json`, remove stale files, succeed with "Reused N scenes already on disk" — no API call.
4. Otherwise bridge `OPENAI_API_KEY`, load NpcPlan, build `_StaticContext`, and run scenes through a `ThreadPoolExecutor` of `min(scene_spec_max_parallel_workers, scene count)`.
5. Each worker: reuse if valid, else build a composer and `_compose_one_scene`; a `_Heartbeat` emits "still composing … (Ns elapsed)" every 30 s. Each composed scene is **saved immediately** (crash-safe), plus the entry-point copy.
6. Collect results in BuildSpec order, remove stale spec files, succeed with "Composed N scenes".

### Binding ids

`_compress_mechanics` pre-computes each item's `binding_id`: `generated.<code_task_id>` for `generate` and `builtin+config_code_edit`, otherwise the item's `builtin_id`. The agent copies it into `MechanicBinding.mechanic_id`.

### Validate-and-repair loop (`_compose_one_scene`)

For `attempt` in `0..scene_spec_max_repair_rounds`:

| Outcome | Next message |
|---------|--------------|
| `SceneSpecOutputError`, truncated at the token cap | `build_truncation_message(max_entities)` — asks for FEWER entities |
| `SceneSpecOutputError`, invalid output | `build_repair_message(field-level issues)` |
| `SceneSpecAgentError` (transport, turn budget, API) | The original brief again |
| Valid output with semantic issues | `build_repair_message(issues)` |
| Valid, no issues | Done |

On the last attempt any of the failures is raised.

### Semantic checks (`_validate_scene_semantics`)

| Check | Issue |
|-------|-------|
| `scene_id` equals the BuildSpec scene id | scene_id mismatch |
| `entities` non-empty | empty scene |
| Every manifest asset id appears on at least one entity's `mesh` | coverage gap (first 20 listed) |
| Every `mechanic_id` is a plan `binding_id` or starts with `builtin.` | unknown mechanic ids |

There is deliberately **no environment-presence check**: the environment belongs to the world architect.

### Strict structured output

The agent's `output_type` is `WireSceneSpec` with `strict_json_schema=True`,
mapped back to `SceneSpec` by `to_scene_spec()`. Strict mode constrains
decoding, so malformed JSON and ids violating the `IdStr` pattern cannot be
produced. `SceneSpec` itself cannot be the wire schema because strict mode
needs `additionalProperties: false` everywhere:

- `MeshRef.sockets` is dropped (the scene compiler fills sockets from `asset_contract.json`); `MeshRef.hold` is not on the wire either.
- `MechanicBinding.params` is carried as `param_overrides`: a list of `{key, value_json}` pairs holding **per-entity overrides only** — the MechanicPlan config is merged in by the scene compiler.
- `attached_to` and `equip_slots` are mirrored verbatim; a field the wire schema omits is one the agent cannot emit.

Raw output is kept by `make_capturing_schema` and written to `logs/scene_spec.log`, so a failure becomes field-level repair instructions rather than the SDK's redacted "Invalid JSON when parsing model output".

### NPC placement instructions

When the NPC plan has NPCs the brief tells the agent, per NPC asset: set `health` from `max_health` and `faction` from faction + `hostile_to`; bind `builtin.character_locomotion` with the locomotion numbers (`animated: false` for `slide_no_animation`); use `physics.kind: kinematic` and a `capsule` collider; and for wave / over-count types place ONE hidden copy tagged `template` far away instead of every instance (the runtime `builtin.encounter_director` clones it).

### Entities and equip slots

`EntitySpec`: `id`, `name`, `transform`, `mesh` (`asset_id`, `glb_path`), `physics` (static / dynamic / kinematic, mass, collision shape, restitution, friction), `mechanic_bindings`, `tags` (≤16), `health`, `faction`, `attached_to` (`entity_id`, `anchor`, `socket`, offset), `equip_slots` (≤24).

An `EquipSlotSpec` is a place **on the wearer** (a hip holster), never on the holster prop:

| Field | Meaning |
|-------|---------|
| `name` | Slot id gameplay refers to (`hip_holster_r`) |
| `body_slot` | `hand_r`, `hand_l`, `waist`, `wrist_l`, `wrist_r`, `back`, `chest`, `head` |
| `host_item_slot` / `host_socket` | For a slot hosted on an already-equipped item |
| `local_position` / `local_rotation` | Offset from the body slot |
| `snap_radius_m` | How near an item must come to snap in |
| `exclusive` | One item at a time (default true) |
| `accepts_tags` | Which items fit (empty = anything) |

`builtin.equip` uses them; `scene_compile` validates slot names and uniqueness.

### The player's bindings

`player_rig` (meshless, tagged `player_rig`) hosts `builtin.hand_pose` and the locomotion, turning and settings-menu items the MechanicPlan lists — nothing else. In Full Player Avatar the `player_avatar` entity binds `builtin.avatar_body`; a Hands Only scene has no avatar entity. `scene_compile` fills in anything forgotten and rejects contradictions.

### Settings (`apps/orchestrator/ptv_orchestrator/settings.py`)

| Setting | Env var | Default |
|---------|---------|---------|
| `scene_spec_model` | `SCENE_SPEC_MODEL` | `gpt-5.6-sol` |
| `scene_spec_reasoning_effort` | `SCENE_SPEC_REASONING_EFFORT` | `medium` |
| `scene_spec_verbosity` | `SCENE_SPEC_VERBOSITY` | `medium` |
| `scene_spec_max_output_tokens` | `SCENE_SPEC_MAX_OUTPUT_TOKENS` | 64,000 (4,000–128,000) |
| `scene_spec_agent_max_turns` | `SCENE_SPEC_AGENT_MAX_TURNS` | 30 (4–200) |
| `scene_spec_max_repair_rounds` | `SCENE_SPEC_MAX_REPAIR_ROUNDS` | 2 (0–6) |
| `scene_spec_max_parallel_workers` | `SCENE_SPEC_MAX_PARALLEL_WORKERS` | 3 (1–16) |
| `scene_spec_max_entities` | `SCENE_SPEC_MAX_ENTITIES` | 120 (8–1000) |

To force recomposition, delete the relevant `scene_spec.<scene_id>.json` (or all of them).

### Quest notes

The Quest device clause in the system prompt tells the composer it is planning for a standalone mobile GPU (tighter draw-call and triangle budgets, fewer simultaneous animated characters). The 120-entity budget applies on both targets; `scene_complexity` in `scene_compile` later measures the real cost and picks Quest settings (FFR, multiview, culling, static batching).

### SSE events

`running` → `info` (reuse / recompose reasons, worker count, per-scene start / done / reused, repair rounds, output problems, heartbeat, stale files removed) → `succeeded`.

## See also

- [`docs/architecture.md`](../../../architecture.md) — data flow
- [`docs/world-generation.md`](../../../world-generation.md) — where the environment comes from
- [`docs/npc-and-ai.md`](../../../npc-and-ai.md) — templates and the encounter director
- Stage 15 [`15_scene_compile`](../15_scene_compile/README.md) — deterministic merge
