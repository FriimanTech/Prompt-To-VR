# Stage 7b: Character Routing

> **Pipeline position:** after `tripo_models` → before `uthana_rigging`  
> **Target:** shared (PC VR and Quest)  
> **Primary code:** [`apps/orchestrator/ptv_orchestrator/pipeline/stage_character_routing.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/stage_character_routing.py)  
> **Primary artifacts:** `projects/<project_id>/character_routing.json`, `projects/<project_id>/tripo_rig_check.json`

## Main stage summary

### What this stage is

For every character that needs a skeleton, this stage decides **which provider rigs and animates it**: Uthana, Tripo, or none. It also lists the motions each character needs and which ones nobody can supply. Every later rigging and animation stage reads its answer from `character_routing.json`.

### Why it exists

The two providers are good at different things. Uthana's auto-rig gives humanoids finger joints and its locomotion API generates gaits in many styles, but it has no one-shot actions (attacks, deaths). Tripo rigs creatures as well as humanoids and has ~101 v1.0 biped presets including actions, but no fingers. Routing is done once, up front, so the rigging stages never disagree.

### Who is routed

| Character | Condition |
|-----------|-----------|
| Player avatar | manifest `category: character`, `character_role: player_avatar`, and BuildSpec `player_experience == full_avatar` |
| NPC | in `npc_plan.json`, its asset is `category: character`, `movement_style != slide_no_animation`, and `animation_intents` is non-empty |

No avatar and no NPC → writes an empty plan and succeeds without any calls.

### Inputs

| Input | Location |
|-------|----------|
| BuildSpec | `build_spec.json` (`player_experience`, `player_avatar.leg_animation`, `mechanics` that mention the asset) |
| AssetManifest | `asset_manifest.json` |
| NpcPlan | `npc_plan.json` (behaviours, `animation_intents`, `animation_fallback`) |
| Tripo mesh + task id | `assets/<id>/model.glb`, `assets/<id>/tripo_task.json` |
| Reference image + prompt | `images/<id>/*` (first image), `images/<id>/reference_prompt.txt` |
| Game prompt | `projects/<id>/prompt.txt` (first 8000 chars) |

### Outputs

| Output | Location | Content |
|--------|----------|---------|
| Routing plan | `character_routing.json` | `CharacterRoutingPlan`: one `CharacterRoute` per character |
| Rig-check cache | `tripo_rig_check.json` | per asset: `riggable`, `rig_type`, `task_id`, `error`, `source_sha256` |
| Manifest reconcile | `asset_manifest.json` | `rigged` set to `rig_provider != none` for each routed asset |

### Where output goes next

`uthana_rigging` (routes with `rig_provider: uthana`), `asset_rigging` (effective provider `tripo`, and it reuses the cached rig-check), `asset_animation`, `uthana_motion_plan`, `uthana_animation`.

### Models / providers / cost

| What | Detail |
|------|--------|
| Tripo rig-check | `POST /v3/animations/rig-check`, **free**, one per NPC (not for the avatar) |
| Uthana style list | `UthanaClient.list_locomotion_styles()`; on error a cached fallback list is used |
| Routing agent | OpenAI Agents SDK (`StructuredAgent`), `gpt-5.6-sol` at reasoning `high`, verbosity `medium`; structured output `RoutingAgentOutput`; receives reference images (sent at `detail: "low"`) |

No credits are spent here.

### Failure behavior

- Uthana disabled or no `UTHANA_API_KEY` → warning; everything routes to Tripo (the avatar loses finger curls) and the agent is not called.
- A rig-check error is recorded on the route (`error`) and a warning is emitted; rules still decide.
- Agent failure (`StructuredAgentError`) → warning, deterministic rules for every NPC.
- The stage itself does not fail on any of these.

---

## Detailed stage deep dive

### Decision rules (`decide_provider`)

Body plan comes from the agent when it answered, otherwise `guess_body_plan`: a creature `rig_type` from rig-check wins, then `reference.pose == neutral_quadruped`, else `biped_humanoid`. A *biped humanoid* means two legs plus two arms, human or not.

| Body plan | Uthana on | Rig-check | Provider | `decided_by` |
|-----------|-----------|-----------|----------|--------------|
| `biped_humanoid` | yes | not riggable / error | `uthana` | code |
| `biped_humanoid` | yes | riggable | agent's pick (`uthana` or `tripo`), else `tripo` | agent / code |
| `biped_humanoid` | no | riggable or unknown | `tripo` | code |
| `biped_humanoid` | no | not riggable | `none` | code |
| `non_creature` | any | any | `none` | code |
| any other creature | any | riggable or unknown | `tripo` | code |
| any other creature | any | not riggable | `none` | code |

The **player avatar** always goes to Uthana when Uthana is available (finger joints), otherwise to Tripo; `decided_by: code`.

### Motions per route

- **Uthana** (`_uthana_motions`): always the shared library idle first (`source: uthana_library`, `library_motion_id = uthana_idle_motion_id`). `walk` / `run` intents and agent locomotion become `uthana_locomotion` motions (an agent `style_hint` is kept only if it is a real style). A walk is added when the NPC has a moving behaviour (`patrol`, `chase`, `flee`, `reposition`, `follow_leader`, `take_cover`). Every other intent is `unmet` ("Uthana has no one-shot actions") with the NPC's `animation_fallback`.
- **Tripo** (`_tripo_motions`): each intent mapped through `presets_for_rig` (humanoids use the `biped` table); unmapped intents are `unmet`. For bipeds the agent may add exact v1.0 presets from `TRIPO_BIPED_V1_PRESETS`. Capped at 5 distinct presets (`TRIPO_MAX_PRESETS_PER_ASSET`); extras become `unmet`.
- **None**: every intent is `unmet` ("not rigged").
- **Tripo avatar** (Uthana off): the asset's `animations`, or `idle/walk/run`; walk/run dropped when `leg_animation` is false.

At most 12 required and 12 unmet motions per route are kept.

### Agent brief (`build_brief`)

Verbatim game prompt, the Tripo v1.0 preset catalog (`tripo_catalog_text()`), the live Uthana style list, and per NPC: id, name, style notes, target height, full NpcSpec, up to 8 BuildSpec mechanics that mention it, the reference-image prompt, and its rig-check result, plus each reference image. System prompt: `CHARACTER_ROUTING_SYSTEM` in `pipeline/prompts`. Decisions for unknown asset ids are dropped.

### Rig-check caching (`_rig_checks`)

The un-rigged source GLB (`stage_asset_rigging._source_glb`) is hashed (SHA-256). A cached, error-free entry with the same hash is reused; otherwise rig-check is called with the generation `task_id` from `tripo_task.json` (or an upload of the GLB when there is no task id). Entries for assets no longer in the manifest are pruned.

### Schema

[`packages/schemas/ptv_schemas/character_routing.py`](../../../../packages/schemas/ptv_schemas/character_routing.py): `BodyPlan` (`biped_humanoid`, `biped_non_humanoid`, `quadruped`, `hexapod`, `octopod`, `avian`, `serpentine`, `aquatic`, `non_creature`), `RigProvider` (`uthana`, `tripo`, `none`), `MotionSource` (`uthana_locomotion`, `uthana_library`, `tripo_preset`, `engine_procedural`, `none`), `MotionKind` (`idle`, `locomotion`, `action`), `CharacterRoute` (incl. `fallback` / `fallback_reason`, set later by `uthana_rigging`; `effective_rig_provider()` = fallback or rig_provider).

### Settings

| Setting | Env var | Default |
|---------|---------|---------|
| `character_routing_model` | `CHARACTER_ROUTING_MODEL` | `gpt-5.6-sol` |
| `character_routing_reasoning_effort` | `CHARACTER_ROUTING_REASONING_EFFORT` | `high` |
| `character_routing_verbosity` | `CHARACTER_ROUTING_VERBOSITY` | `medium` |
| `character_routing_agent_max_turns` | `CHARACTER_ROUTING_AGENT_MAX_TURNS` | 30 |
| `character_routing_max_retries` | `CHARACTER_ROUTING_MAX_RETRIES` | 1 |
| `uthana_enabled` | `UTHANA_ENABLED` | `true` |
| `uthana_api_key` | `UTHANA_API_KEY` | none |
| `uthana_idle_motion_id` | `UTHANA_IDLE_MOTION_ID` | `msf9nhQ4yyTJ` |
| `tripo_api_key`, `tripo_base_url` | `TRIPO_API_KEY`, `TRIPO_BASE_URL` | — |
| `openai_api_key` | `OPENAI_API_KEY` | none |

### SSE events

`running` → `warning` (Uthana unavailable, rig-check / agent failures) → `info` per route (`<id>: <body> -> rig <p>, animate <p> (<decided_by>)`) → `succeeded` with provider counts.

### Quest notes

Routing is target-independent. On Quest, every rigged character also counts against the animated-agent budget set later in `asset_optimization`.

## See also

- [`docs/uthana.md`](../../../uthana.md) — Uthana stages end to end
- [`docs/npc-and-ai.md`](../../../npc-and-ai.md) — NpcPlan and animation intents
- Previous: [`07_tripo_models`](../07_tripo_models/README.md) · Next: [`08a_uthana_rigging`](../08a_uthana_rigging/README.md)
