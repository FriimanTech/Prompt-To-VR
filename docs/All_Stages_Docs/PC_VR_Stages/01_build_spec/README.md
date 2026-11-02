# Stage 1: BuildSpec

> **Pipeline position:** start → before `optimization_preferences`  
> **Target:** shared (PC VR and Quest)  
> **Primary code:** [`apps/orchestrator/ptv_orchestrator/pipeline/stage_build_spec.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/stage_build_spec.py)  
> **Primary artifacts:** `projects/<project_id>/build_spec.json`

## Main stage summary

### What this stage is

The **BuildSpec stage** turns the raw game prompt into the first structured plan for the whole project. A **BuildSpec** (`packages/schemas/ptv_schemas/build_spec.py`) is a Pydantic-validated document holding the title, pitch, game type, scenes, mechanics, the object assets the game needs, the NPC roster, the player's movement options, acceptance tests and constraints.

### Why it exists

Every later stage reads **validated JSON**, not prose (schema-first development, AGENTS.md hard rule 4). If the asset planner and the scene composer each re-read the prompt, they could disagree; BuildSpec is the single answer to "what game are we building?".

### Inputs

| Input | Source |
|-------|--------|
| User prompt (verbatim) | `ProjectState.prompt` (also saved as `projects/<id>/prompt.txt`) |
| Target device (`pcvr` / `quest`) | `ProjectState.target_device`, chosen on the New Project page |
| Player experience (`hands_only` / `full_avatar`) | `ProjectState.player_experience`, chosen on the New Project page |

### Outputs

| Output | Location |
|--------|----------|
| BuildSpec JSON | `projects/<id>/build_spec.json` |
| Stage record | `projects/<id>/project_state.json` |

### Where output goes next

Nearly every later stage reads it: `asset_manifest`, `interaction_design`, `npc_plan`, `game_design`, `mechanic_plan`, `scene_spec`, `world_generation`, `code_generation` and `scene_compile`. The orchestrator picks the PC VR or Quest tail from `ProjectState.target_device` (which `BuildSpec.target_device` mirrors).

### Models, providers and effort

One structured-output call through the shared `LlmClient` (`pipeline/llm.py` → `make_llm`), **not** an Agents SDK agent:

- Primary: Claude **hard** tier, `claude_hard_model` = `claude-opus-4-7`.
- Fallback (no Anthropic key or API error): OpenAI `openai_hard_model` = `gpt-5.6-sol` at `openai_hard_reasoning_effort` = `high`, via Chat Completions JSON mode with the schema in the system prompt; the result is re-validated with the same Pydantic model.

Cost: one LLM call, plus up to two retries if the NPC roster breaks its rules.

### Failure behavior

A response that fails `BuildSpec` validation, a provider error, or a timeout raises; the orchestrator records the stage as `failed` and the pipeline stops. Roster problems that survive the retries are a **warning**, not a failure.

---

## Detailed stage deep dive

### Execution trace (`stage_build_spec.run`)

| Step | Code | Effect |
|------|------|--------|
| 1 | `ctx.emit(build_spec, "running")`, `record_stage(running)` | UI shows stage 1 active |
| 2 | `BuildSpec.model_json_schema(by_alias=True)` | Schema for structured output |
| 3 | Build the user message | Verbatim prompt; `prompt_id` from `_prompt_id_from_text()` (lowercase slug, max 48 chars, `"prompt"` if empty); the project's `target_device` and `player_experience` with "not yours to pick"; for Hands Only, "set `player_avatar.coverage` to `hands_only` and plan NO player-body asset"; "Decide the `movement` block from the prompt" |
| 4 | `build_spec_system(target_device)` | `_BUILD_SPEC_TMPL` with the device-specific text, plus `_IGNORE_OPTIMIZATION_WORDING` (see below) |
| 5 | `client.generate_json(...)` → `BuildSpec.model_validate` | Hard fail on schema mismatch |
| 6 | NPC roster check, up to `_ROSTER_RETRIES` = 2 extra calls | See below |
| 7 | `_force_target_device` | Project's device overwrites the model's (`target_device.sync_build_spec_device`); emits `info` when changed |
| 8 | `_force_player_experience` | Project's experience overwrites the model's; Hands Only pins `player_avatar.coverage = hands_only` and drops player-body `AssetEntry`s (`player_experience.py`) |
| 9 | `ctx.build_spec = spec`; `store.save_build_spec` | Writes `build_spec.json` |
| 10 | `record_stage(succeeded, message="Produced BuildSpec for <title>")`, emit `succeeded` | Returns `StageResult` |

A `dry_run` context raises `ProviderConfigError` (tests supply a canned BuildSpec).

### NPC roster validation (`roster_violations`)

`BuildSpec.npc_roster` decides how many **unique NPC models** get made, which is both the game's variety and a real cost (each variant later costs an image, a Tripo mesh, a rig and clips). Each `NpcFamily` has a `count_reading`, and `required_variant_count()` fixes how many variants it must have:

| `count_reading` | Meaning | Variants |
|-----------------|---------|----------|
| `singular` | one specific character | 1 |
| `single_type_requested` | "all the zombies look the same" | 1 |
| `explicit_types` | "three types of drones" | `explicit_count` |
| `explicit_number` | "five goblins" | `min(3, instance_hint)` |
| `plural_unspecified` | "goblin enemies" | 3 |

`roster_violations` reports: a wrong variant count; `explicit_types` without `explicit_count`; an asset used as a variant of two families; a variant whose `asset_id` has no `AssetEntry`; a variant whose `AssetEntry.category` is not an NPC/character category (`npc`, `enemy`, `character`, `creature`, `ally`, `boss`); and a multi-variant family where every variant has the same `physique` (unless `custom`). Problems are emitted as a `warning` and sent back in a retry prompt ("add the missing variant AssetEntries; keep everything else"). After two retries the stage continues with a warning.

### Key BuildSpec fields and consumers

| Field | Notes / consumers |
|-------|-------------------|
| `prompt_id` | Slug; namespaces generated code and every later artifact |
| `title`, `pitch`, `game_type` | scene.json, exe name / Quest app label |
| `target_device` | Mirror of `ProjectState.target_device` |
| `player_experience`, `player_avatar`, `player_perspective` | Mirror of the project choice; drives player-role mechanics and avatar assets |
| `movement` | `modes` (`teleport` / `slide_head` / `slide_controller`, may be empty for a stationary game), `default_mode`, `turning` (`snap` / `smooth`, may be empty), `default_turn`, `vignette` (`slide`, `smooth_turn`, `user_toggle`, all default false), `settings_menu` (default true). Older BuildSpecs are mapped from the legacy `locomotion` field |
| `scenes[]` (1..50) | scene_spec (one agent per scene), world_generation, scene_compile; includes scene `kind`, `environment_mode`, nested `host`, `links` |
| `mechanics[]` | mechanic_plan; each is `type` `regular` or `hub` |
| `assets[]` (1..48) | asset_manifest echoes each `AssetEntry` |
| `npc_roster[]` (≤16 families, ≤8 variants each) | asset_manifest, npc_plan |
| `npc_summary`, `hud_summary`, `constraints`, `interaction_rules`, `acceptance_tests` | later planners |

Full field reference: [`docs/prompt-schema.md`](../../../prompt-schema.md).

### Optimization wording is ignored here

`_IGNORE_OPTIMIZATION_WORDING` tells the planner that rendering / optimization instructions (foveation, resolution, LOD, texture quality, draw distance, frame rate…) do not shape the game. Those are read by the next stage, `optimization_preferences`.

### Resume behavior

No skip logic: resuming at `build_spec` calls the LLM again and overwrites `build_spec.json`. At run start the orchestrator re-stamps an existing BuildSpec's `target_device` / `player_experience` from `ProjectState` for older projects.

### Important files

| File | Role |
|------|------|
| `pipeline/stage_build_spec.py` | Stage entry, `roster_violations` |
| `pipeline/llm.py` | `make_llm` → `LlmClient` with Claude → OpenAI fallback |
| `pipeline/prompts.py` | `build_spec_system`, `_BUILD_SPEC_TMPL`, `_IGNORE_OPTIMIZATION_WORDING` |
| `pipeline/target_device.py` | `sync_build_spec_device` |
| `pipeline/player_experience.py` | `sync_build_spec_experience`, `strip_player_avatar_assets` |
| `packages/schemas/ptv_schemas/build_spec.py` | Schema |
| `storage.py` | `save_build_spec`, `record_stage` |

### Settings (`apps/orchestrator/ptv_orchestrator/settings.py`)

Env vars are the upper-cased field names, read from the repo `.env`.

| Setting (env var) | Default |
|-------------------|---------|
| `claude_hard_model` (`CLAUDE_HARD_MODEL`) | `claude-opus-4-7` |
| `openai_hard_model` (`OPENAI_HARD_MODEL`) | `gpt-5.6-sol` |
| `openai_hard_reasoning_effort` (`OPENAI_HARD_REASONING_EFFORT`) | `high` |
| `claude_max_tokens` (`CLAUDE_MAX_TOKENS`) | 32 000 |
| `openai_max_tokens` / `openai_max_tokens_ceiling` | 96 000 / 128 000 |
| `claude_request_timeout_seconds` / `openai_request_timeout_seconds` | 300 s each |
| `anthropic_api_key` / `openai_api_key` | from `.env` |

### Quest-specific notes

For a `quest` project the system prompt names the Quest 3 mobile GPU so scenes, asset counts and mechanics are planned against that budget. Nothing else differs.

## See also

- [`docs/prompt-schema.md`](../../../prompt-schema.md) — BuildSpec field reference
- [`docs/architecture.md`](../../../architecture.md) — schema-first pipeline
- [`docs/quest-optimization.md`](../../../quest-optimization.md) — why optimization wording is handled separately
- Next stage: [`01b_optimization_preferences`](../01b_optimization_preferences/README.md)
