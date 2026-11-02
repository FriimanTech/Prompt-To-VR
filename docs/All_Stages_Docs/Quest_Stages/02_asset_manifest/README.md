> **Shared content:** Identical to [PC_VR_Stages/02_asset_manifest](../../PC_VR_Stages/02_asset_manifest/README.md). Update both copies when this stage changes.

# Stage 2: AssetManifest

> **Pipeline position:** after `optimization_preferences` → before `interaction_design`  
> **Target:** shared (PC VR and Quest)  
> **Primary code:** [`apps/orchestrator/ptv_orchestrator/pipeline/stage_asset_manifest.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/stage_asset_manifest.py)  
> **Primary artifacts:** `projects/<project_id>/asset_manifest.json`

## Main stage summary

### What this stage is

The **AssetManifest stage** plans every **3D object** the game needs — characters, props, weapons, FX and UI meshes — as a list of `AssetItem`s: reference-image prompt, Tripo settings, real-world scale, collision shape, rig / animation hints, socket hints and importance.

### Why it exists

BuildSpec says *what* the game is; the manifest says *which GLBs* get made. Every item costs a reference image plus a paid Tripo call, so the manifest is also the main cost decision. The VR **environment** is not in it: the world is authored in C++ by `world_generation`.

### Inputs

| Input | Location |
|-------|----------|
| BuildSpec | `projects/<id>/build_spec.json` (or `ctx.build_spec`) |

### Outputs

| Output | Location |
|--------|----------|
| AssetManifest (1..48 items) | `projects/<id>/asset_manifest.json` |
| Stage record (e.g. `"Planned 9 assets (dropped 3 audio asset(s) (...); ...)"`) | `project_state.json` |

### Where output goes next

- **`interaction_design`** reads it and may rewrite it (appends seen projectile assets, corrects real-world scale, mirrors `socket_hints`, marks held items `hero`, drops engine-drawn FX meshes).
- **`npc_plan`** reconciles `rigged` / `animations` for characters.
- `reference_images`, `tripo_models`, character routing, rigging, animation, normalization, sockets, contract, optimization and `scene_compile` all read it.

### Models, providers and effort

One `LlmClient.generate_json` call with `model=claude_light_model` (`claude-sonnet-4-6`) and system prompt `ASSET_MANIFEST_SYSTEM`; fallback is OpenAI `gpt-5.6-sol` at `openai_light_reasoning_effort` = `medium`. One extra call if NPC roster variants are missing.

### Failure behavior

Invalid JSON / schema mismatch or a provider error fails the stage. Missing roster variants after the retry are a **warning** (those NPC types simply won't exist).

---

## Detailed stage deep dive

### Execution trace (`stage_asset_manifest.run`)

| Step | Code | Detail |
|------|------|--------|
| 1 | emit `running`, `record_stage(running)` | |
| 2 | Load BuildSpec | `ctx.build_spec` or `store.load_build_spec` |
| 3 | `generate_json(prompt, schema=AssetManifest.model_json_schema(by_alias=True), system=ASSET_MANIFEST_SYSTEM, model=claude_light_model)` | Full BuildSpec JSON embedded; for Hands Only the message adds "plan NO player-avatar / player-body item; hand-shaped props (gloves, gauntlets) are ordinary props with `wearable_region` set" |
| 4 | `AssetManifest.model_validate` | |
| 5 | `missing_roster_assets` | Every `npc_roster` variant needs a manifest item; if any is missing, one retry naming them; still missing → warning |
| 6 | `normalise_roster_items` | Roster variants forced to `category: character`, `character_role: npc` (so they get a T-pose reference, a rig and clips); emits `info` |
| 7 | `_partition` | Drops `environment` and audio items (below) |
| 8 | Hands Only filter | Drops items where `is_player_avatar_item` (role `player_avatar`, or a character whose id/name/style notes name the player's body) |
| 9 | `ctx.asset_manifest = manifest`; `store.save_asset_manifest` | |
| 10 | `record_stage(succeeded)` with notes; emit `"N assets"` | |

### Deterministic drops

Every drop only applies **when something else survives** — `AssetManifest` requires at least one item, and an empty manifest is worse than one wasted Tripo call.

| Dropped | Rule | Why |
|---------|------|-----|
| Environment | `category == environment` | The world is C++ (`world_generation`), not a GLB |
| Audio | `is_audio_item()` | Sound has no shape; it would buy a picture of a gunshot and a silent blob |
| Player body (Hands Only) | `is_player_avatar_item()` | Hands Only uses the engine's default hands |

`is_audio_item` matches whole tokens of `id` + `name` against `_AUDIO_TOKENS` (`audio`, `sfx`, `soundfx`, `sound(s)`, `soundscape`, `soundtrack`, `music`, `bgm`, `foley`, `voiceover`, `vox`, `ambience`) and stands down when a `_SOUND_SOURCE_TOKENS` word names a physical sound-making object (speaker, radio, siren, alarm, bell, jukebox, drum, guitar, microphone…). It is fail-open on purpose: a wrong keep costs one call, a wrong drop deletes an asset the game needs. Near-misses that name geometry (`stinger`, `ambient`, `horn`, `track`) are not audio words. Both prompts (`BUILD_SPEC_SYSTEM`, `ASSET_MANIFEST_SYSTEM`) forbid audio items; this code is the backstop for fallback models.

### `AssetItem` fields that matter downstream

| Field | Purpose | Changed later? |
|-------|---------|----------------|
| `id` | Folder `assets/<id>/`, every reference | No |
| `category` (`character`, `prop`, `fx`, `ui`, `environment`) | Routing | Roster variants forced here |
| `character_role` (`none`, `npc`, `player_avatar`, …) | NPC vs player body | Roster variants forced here |
| `wearable_region` | Hand / forearm-shaped props | No |
| `reference` (`prompt`, `pose`, `coverage`, …) | Reference image request | No |
| `strategy`, `tripo` (`texture`/`pbr` true, `texture_alignment`, `face_limit`, `model_version`) | Tripo request | No |
| `target_scale_meters` | World size | `interaction_design` (real dimensions) |
| `collision_shape` | Physics metadata | No |
| `rigged`, `animations[]` (semantic intents) | Rig / clip gates | **`npc_plan` reconcile** |
| `socket_hints` | Socket names | `interaction_design` mirrors the design |
| `importance`, `max_triangles` | Optimization budget | `interaction_design` sets held items to `hero` |
| `hold_rotation_offset_deg`, `forward_hint` | Optional manual grip / facing corrections | No |

### Resume

No skip logic: resuming at this stage calls the LLM again and overwrites the manifest (later stages then rewrite it again).

### Important files

| File | Role |
|------|------|
| `pipeline/stage_asset_manifest.py` | Stage, roster checks, drops |
| `pipeline/player_experience.py` | `is_player_avatar_item` |
| `pipeline/prompts.py` | `ASSET_MANIFEST_SYSTEM` |
| `packages/schemas/ptv_schemas/asset_manifest.py` | `AssetItem`, `AssetCategory`, `CharacterRole`, `AnimationIntent` |

### Settings

| Setting (env var) | Default |
|-------------------|---------|
| `claude_light_model` (`CLAUDE_LIGHT_MODEL`) | `claude-sonnet-4-6` |
| `openai_light_model` (`OPENAI_LIGHT_MODEL`) | `gpt-5.6-sol` |
| `openai_light_reasoning_effort` (`OPENAI_LIGHT_REASONING_EFFORT`) | `medium` |
| shared token / timeout settings | see Stage 1 |

### Quest-specific notes

None in this stage; the Quest asset budget is enforced later by `asset_optimization`.

## See also

- [`docs/architecture.md`](../../../architecture.md) — objects vs C++ environment
- [`docs/prompt-schema.md`](../../../prompt-schema.md) — schema reference
- [`docs/npc-and-ai.md`](../../../npc-and-ai.md) — NPC roster and variants
- Next stage: [`02b_interaction_design`](../02b_interaction_design/README.md)
