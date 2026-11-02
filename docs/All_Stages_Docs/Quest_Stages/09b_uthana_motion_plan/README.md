> **Shared content:** Identical to [PC_VR_Stages/09b_uthana_motion_plan](../../PC_VR_Stages/09b_uthana_motion_plan/README.md). Update both copies when this stage changes.

# Stage 9b: Uthana Motion Plan

> **Pipeline position:** after `asset_animation` → before `uthana_animation`  
> **Target:** shared (PC VR and Quest)  
> **Primary code:** [`apps/orchestrator/ptv_orchestrator/pipeline/stage_uthana_motion_plan.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/stage_uthana_motion_plan.py), [`pipeline/uthana/glb_measure.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/uthana/glb_measure.py), [`pipeline/uthana/motion_names.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/uthana/motion_names.py)  
> **Primary artifacts:** `projects/<project_id>/uthana_motion_plan.json`

## Main stage summary

### What this stage is

The **Uthana motion plan** decides, for every Uthana-animated character, exactly which Uthana motions it needs — without generating anything. Code measures each rigged character, an agent picks a locomotion **style** for each gait, and code then fixes every number (strides, speed, angle) and the motion's canonical name. Every character also gets the shared library idle.

### Why it exists

Uthana generation is billed by motion seconds. Planning first means the plan can be read and costed before anything is spent, and the deterministic canonical name (`style-str{N}-ms{speed}-deg{angle}`) lets the next stage find and reuse a motion that already exists in the Uthana organisation instead of paying for it again.

### Inputs (what enters)

| Input | Location | Used for |
|-------|----------|----------|
| Character routing | `character_routing.json` | Characters whose effective animation provider is `uthana`; their `uthana_locomotion` motion keys, `style_hint`s, rationale |
| Uthana rig ledger | `uthana_rig.json` | Only `status: rigged` characters; their `character_id` |
| Rigged GLB | `assets/<id>/model_uthana_rigged.glb` | Height measurement |
| AssetManifest | `asset_manifest.json` | `target_scale_meters`, name, `style_notes` |
| NpcPlan (optional) | `npc_plan.json` | Role / behaviour context for the agent |
| BuildSpec | `build_spec.json` | `prompt_id` |
| Uthana API | live | Locomotion style list; idle motion lookup |

### Outputs (what it produces)

| Output | Location | Contents |
|--------|----------|----------|
| Motion plan | `uthana_motion_plan.json` (`UthanaMotionPlan`) | `locomotion_styles`, and per character: `asset_id`, `character_id`, `character_role`, `height_for_motion_m`, `target_scale_meters`, `engine_scale`, `motions[]` (`UthanaMotionSpec`), `notes[]` |

### Where output goes next

- **`uthana_animation`** resolves, downloads and merges every motion in the plan.

### Models / providers

- Agent: OpenAI Agents SDK `StructuredAgent`, `uthana_motion_plan_model` = `gpt-5.6-sol`, reasoning effort **`medium`**, verbosity `medium`, output model `UthanaMotionAgentOutput`, system prompt `UTHANA_MOTION_PLAN_SYSTEM`. No tools.
- Uthana (SDK-backed `UthanaClient`): `list_locomotion_styles()`, `get_motion(idle_id)`.

### Cost

No Uthana generation. One agent call per run (only when there are Uthana characters).

### Failure behavior

- No Uthana-animated rigged characters → writes an empty plan and succeeds, no API calls.
- Style list query fails → warning, the cached `UTHANA_STYLES_FALLBACK` list is used.
- Agent fails (`StructuredAgentError`) → warning, every motion takes code defaults.
- The library idle id does not exist in the organisation → **stage fails** (set `UTHANA_IDLE_MOTION_ID`).
- A rigged GLB that cannot be measured (no mesh, no skin, zero height) raises `GltfError` → stage fails.

---

## Detailed stage deep dive

### 1. Targets

Routes with `effective_animation_provider() == uthana` **and** a `status: rigged` entry in `uthana_rig.json`. A character that fell back to Tripo during `uthana_rigging` is therefore not planned here.

### 2. Measurement (`glb_measure.measure`)

Uthana GLBs put the skeleton under an `Armature` node scaled ~0.01 and rotated 90° about X, so raw `POSITION` spans are not the real size. `measure` walks the node hierarchy, skins the bind-pose vertices through the joint matrices and reports:

- `height_for_motion_m` = world mesh AABB height — the size Uthana animates the character at (the primary value);
- `engine_scale = target_scale_meters / raw longest axis` (what the engine will resize by);
- notes: toe-to-head disagreeing with mesh height by >15%, or a raw longest axis that is not Y (the engine would scale by arm span).

`target_scale_meters` defaults to 1.8 when the manifest item is missing. The module also runs as a CLI: `python -m ptv_orchestrator.pipeline.uthana.glb_measure <glb> --expected-target 1.8`.

### 3. Agent brief

Per character: role, name, `style_notes` (≤600 chars), height, the code-computed walk and run speeds, `motions_to_plan` (routing's `uthana_locomotion` keys, e.g. `walk`, `run`, `run.punching`), routing style hints and rationale, and the NPC plan entry. Plus the live style list. The agent returns `PlannedLocomotion` entries (`key`, `gait`, `style_id`, `strides`, `move_speed`, `travel_angle`, `motion_name`, `purpose`).

### 4. Code correction (`plan_character_motions`)

The agent's answer is advice; code decides:

| Field | Rule |
|-------|------|
| Keys | Routing's keys, plus any extra `walk.*` / `run.*` variant the agent added |
| Gait | `run` if the key's base is `run`, else `walk` |
| Style | Agent's style if it is in the live list → else routing's hint → else (plain gait) the character's style from its other plain gaits → else `neutral_non_gendered`. Rejected styles are noted. |
| Strides | Agent value if 1–5, else `uthana_locomotion_strides` (default 5) |
| Speed | `0.78 × height` (walk) or `2.2 × height` (run); the agent's value is replaced if it is more than 5% off, then clamped to Uthana's 0.25–8.0 m/s and rounded half-up to one decimal |
| Angle | Always `0` |
| Name | `format_locomotion_name(style, strides, speed, 0)` → e.g. `zombie-str5-ms1.4-deg0` |

### 5. Shared idle

Every character gets `key: idle`, `kind: uthana_library`, `motion_id = uthana_idle_motion_id` (default `msf9nhQ4yyTJ`) and the idle's real organisation name, inserted first. The agent never plans an idle.

### Resume

The stage has no skip logic: each run re-measures and re-asks the agent. That is cheap and safe because the names are deterministic — `uthana_animation` matches by name and never pays twice for the same motion.

### Settings

| Setting | Env var | Default |
|---------|---------|---------|
| `uthana_api_key` | `UTHANA_API_KEY` | — |
| `uthana_idle_motion_id` | `UTHANA_IDLE_MOTION_ID` | `msf9nhQ4yyTJ` |
| `uthana_locomotion_strides` | `UTHANA_LOCOMOTION_STRIDES` | `5` (1–5) |
| `uthana_motion_plan_model` | `UTHANA_MOTION_PLAN_MODEL` | `gpt-5.6-sol` |
| `uthana_motion_plan_reasoning_effort` | `UTHANA_MOTION_PLAN_REASONING_EFFORT` | `medium` |
| `uthana_motion_plan_verbosity` | `UTHANA_MOTION_PLAN_VERBOSITY` | `medium` |
| `uthana_motion_plan_agent_max_turns` | `UTHANA_MOTION_PLAN_AGENT_MAX_TURNS` | `20` |
| `uthana_motion_plan_max_retries` | `UTHANA_MOTION_PLAN_MAX_RETRIES` | `1` |

`OPENAI_API_KEY` is copied from settings into the process environment for the Agents SDK if it is not already set.

### Quest notes

Nothing Quest-specific. Speeds are planned at the GLB's own size; `uthana_animation` converts them to engine metres per second with `engine_scale`.

## See also

- [`docs/uthana.md`](../../../uthana.md) — the full Uthana flow
- [`docs/npc-and-ai.md`](../../../npc-and-ai.md) — how locomotion clips are played
- [`docs/provider-setup.md`](../../../provider-setup.md) — Uthana key
