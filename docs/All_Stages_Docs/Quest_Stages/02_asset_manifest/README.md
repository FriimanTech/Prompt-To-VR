> **Shared content:** Identical to [PC_VR_Stages/02_asset_manifest](../../PC_VR_Stages/02_asset_manifest/README.md). Update both copies when this stage changes.

# Stage 2: AssetManifest

> **Pipeline position:** after `build_spec` → before `npc_plan`  
> **Target:** shared (PC VR and Quest)  
> **Primary code:** [`apps/orchestrator/ptv_orchestrator/pipeline/stage_asset_manifest.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/stage_asset_manifest.py)  
> **Primary artifacts:** `projects/<project_id>/asset_manifest.json`

## Main stage summary

### What this stage is

The **AssetManifest stage** plans every **3D object** the game needs: characters, props, weapons, FX, and UI meshes. It produces `asset_manifest.json`, a list of `AssetItem` entries with generation strategy, scale, collision hints, rigging flags, animation intents, and socket hints.

### Why it exists

BuildSpec says *what* the game is; AssetManifest says *which GLB files* Tripo must produce. Separating object assets from the VR **environment** (which is C++ world generation, not Tripo) keeps the 3D pipeline focused and cheaper.

### Inputs (what enters)

| Input | Location |
|-------|----------|
| BuildSpec | `projects/<id>/build_spec.json` |

### Outputs (what it produces)

| Output | Location |
|--------|----------|
| AssetManifest | `projects/<id>/asset_manifest.json` |

Each item includes: `id`, `category`, `strategy` (`image_to_3d` or `text_to_3d`), `reference` prompt text, `tripo` settings, `rigged`, `animations[]`, `socket_hints`, `target_scale_meters`, `collision_shape`.

### Where output goes next

- **`npc_plan`** may rewrite `rigged` / `animations` to match prompt intent.
- **`reference_images`**, **`tripo_models`**, rigging, animation, normalization, contract, optimization, and **`scene_compile`** all read the manifest.

### Key concepts and terms

- **AssetItem** — One row in the manifest; maps 1:1 to a folder under `projects/<id>/assets/<asset_id>/`.
- **Environment category** — Items tagged `environment` are **dropped** if any non-environment items exist. The world is built in C++ (`world_generation`), not as a Tripo GLB.
- **Audio items** — Items naming a *sound* (`pistol_shot_audio`, "Audio: UI Click") are **dropped** the same way. Sound has no shape, so a Tripo call for one buys a silent blob.
- **Rigging / animations** — Initial hints only; `npc_plan` reconciles them before credits are spent on Tripo rigging.

### End-to-end flow (plain language)

1. Load BuildSpec JSON.
2. Call **Claude Sonnet** with `ASSET_MANIFEST_SYSTEM` and the BuildSpec embedded in the user message.
3. Validate response as `AssetManifest`.
4. Post-filter: remove stray `environment` items and any item naming a sound, when other objects survive.
5. Save manifest and succeed.

---

## Detailed stage deep dive

### Exact execution trace (`stage_asset_manifest.run`)

| Step | Action | Detail |
|------|--------|--------|
| 1 | emit + `record_stage(running)` | Standard stage bookkeeping |
| 2 | Load BuildSpec | `ctx.build_spec` cache or `load_build_spec()` |
| 3 | `make_llm()` | Same factory as stage 1 |
| 4 | Embed full BuildSpec JSON in user message | Instructs model to emit AssetManifest |
| 5 | `generate_json(..., model=claude_light_model, system=ASSET_MANIFEST_SYSTEM)` | **Sonnet**, not Opus — cheaper planning pass |
| 6 | `AssetManifest.model_validate()` | Schema enforcement |
| 7 | **Reconcile: environment + audio post-filter** | See below |
| 8 | `save_asset_manifest()` | Writes disk; caches on `ctx.asset_manifest` |
| 9 | Success message | Includes dropped environment count if any |

### Reconcile filter (deterministic, after LLM)

`_partition()` splits the model's answer into three buckets and keeps only the first, but **only when the first is non-empty** — `AssetManifest` requires ≥1 item, so emptying it would turn a wasted API call into a crashed pipeline.

| Bucket | Rule | Why it cannot be a GLB |
|--------|------|------------------------|
| kept | everything else | — |
| environment | `category == environment` | The VR environment is authored in C++ by `world_generation`. |
| audio | `is_audio_item()` | Sound has no shape. |

Both are belt-and-suspenders behind `ASSET_MANIFEST_SYSTEM`, which forbids each of them outright. The prompt is advice; a fallback model may not take it, and every surviving item costs a reference image plus a paid Tripo call.

#### `is_audio_item()`

Three real runs planned audio as geometry: `pistol_shot_audio` / `rifle_shot_audio` / `target_impact_audio` as `fx`, and nine `Audio: …` items as `ui` — which is how they got past a category-only check. Each one generated a picture of a gunshot, a mesh of that picture, and a silent blob in the scene.

The matcher is whole-token over `id` and `name`, so `soundproof_panel` and `Audiotorium Seats` survive. It stands down entirely when the item also names a physical sound-**making** object (speaker, radio, alarm bell, klaxon, jukebox): those are props with silhouettes, and the failure directions are not symmetric — a wrong drop deletes geometry the game needs, a wrong keep costs one call. Near-misses that name real geometry are deliberately excluded from the audio vocabulary: `stinger` (a missile), `ambient` (lighting), `horn`, `track`.

Audio the prompt asks for belongs in the mechanic that triggers it, not the asset list. `BUILD_SPEC_SYSTEM` now says the same thing one stage earlier, since that is where the `Audio` entries originated.

### Per-AssetItem contract for downstream stages

| Field | Purpose | Overwritten later? |
|-------|---------|-------------------|
| `id` | Directory `assets/<id>/`, all references | No |
| `category` | `character` → npc_plan; props/weapons/etc. | No |
| `strategy` | `image_to_3d` → stage 6+7; `text_to_3d` skips images | No |
| `reference.*` | Image/Tripo prompts | No |
| `rigged`, `animations[]` | Rigging/animation gates | **Yes — npc_plan reconcile** |
| `socket_hints` | Names for `ptv_glb_sockets` | No |
| `target_scale_meters` | World scale, capsule height, socket scale | No |
| `collision_shape` | Metadata → scene physics | No |

### Artifact I/O

| Read | Write |
|------|-------|
| `build_spec.json` | `asset_manifest.json` |
| | `project_state.json` stage record |

**Note:** Stage 3 may rewrite `asset_manifest.json` in the same pipeline pass after saving npc_plan.

### SSE events

`running` → optional `info` naming any dropped audio ids → `succeeded` with `"N assets"`, plus a parenthesised note per bucket, e.g. `"N assets (dropped 1 environment asset(s); world is C++-generated; dropped 3 audio asset(s) (pistol_shot_audio, …); the manifest describes geometry only)"`.

### Resume / idempotency

No skip logic — each run calls the LLM and overwrites the manifest. If you resume at stage 2, stage 1's BuildSpec is assumed already on disk.

### Failure modes

| Condition | Result |
|-----------|--------|
| LLM invalid JSON | Stage fails |
| Manifest empty after filter | Validation error (requires ≥1 item) |
| npc_plan reconcile (stage 3) | Clears `rigged`/`animations` for slide NPCs — not a failure |

### Important files

| File | Role |
|------|------|
| `stage_asset_manifest.py` | Stage + filter |
| `asset_manifest.py` (schema) | `AssetItem`, `AssetCategory`, strategies |
| `prompts.py` | `ASSET_MANIFEST_SYSTEM` — objects only, not environment |

### Settings

| Setting | Value |
|---------|-------|
| `claude_light_model` | `claude-sonnet-4-6` (explicitly passed to API call) |

### LLM prompt contract

`ASSET_MANIFEST_SYSTEM` instructs objects-only output, rig/animation hints, socket_hints, and scale. The environment filter is code-enforced because models sometimes still emit environment GLBs.

### After stage 3

Post-npc_plan manifest is authoritative for `rigged`/`animations` — stage 2 values may be overwritten.

### Hands Only drops the player body

When `BuildSpec.player_experience` is `hands_only`, every item with
`character_role: player_avatar` (or a character whose text names the player's
body) is removed before anything is billed -- no reference image, Tripo mesh,
rig or animation for a body nobody sees. Hand-shaped props are kept as
ordinary props and labelled with `AssetItem.wearable_region` (`hands`,
`forearms`, ...). `AssetItem.hold_rotation_offset_deg` is an optional manual
grip correction that scene_compile forwards to `builtin.grab`.

---

## Extend this folder

- `artifacts.md` — AssetItem field reference
- `schemas.md` — category and strategy enums
- `failure-modes.md` — reconcile with npc_plan

## See also

- [`docs/architecture.md`](../../../architecture.md) — objects vs C++ environment split
- Stage 3 [`03_npc_plan`](../03_npc_plan/README.md) — manifest reconciliation
