# Stage 9: Animation Retarget (Tripo)

> **Pipeline position:** after `asset_rigging` → before `uthana_motion_plan`  
> **Target:** shared (PC VR and Quest)  
> **Primary code:** [`apps/orchestrator/ptv_orchestrator/pipeline/stage_asset_animation.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/stage_asset_animation.py), [`packages/provider-clients/ptv_providers/tripo_client.py`](../../../../packages/provider-clients/ptv_providers/tripo_client.py)  
> **Primary artifacts:** `projects/<project_id>/assets/<asset_id>/model_animated.glb`, `projects/<project_id>/asset_animation.json`

## Main stage summary

### What this stage is

The **animation retarget stage** gives each **Tripo-animated**, successfully rigged character real movement clips. It turns the clips a character needs into Tripo preset ids, calls Tripo's V3 `POST /v3/animations/retarget` on the rig task, downloads one GLB with mesh + skin + all clips, and records the **actual glTF clip names** Tripo produced against the semantic keys gameplay uses (`walk`, `attack`, …).

Characters that `character_routing` sent to **Uthana** are skipped here; they are animated by `uthana_motion_plan` → `uthana_animation`, which write into the same `asset_animation.json`.

### Why it exists

A rigged mesh without clips is a statue sliding across the floor. `builtin.character_locomotion` and generated code play clips by semantic key (`ctx.skeleton->play_clip(self, "walk", true)`); that works only because this stage records which real clip each key maps to.

### Inputs (what enters)

| Input | Location | Used for |
|-------|----------|----------|
| AssetManifest | `asset_manifest.json` | `rigged`, `animations[]` (semantic intents) per item |
| Character routing | `character_routing.json` | Which characters Tripo animates; exact v1.0 presets chosen by routing (`required_motions` with source `tripo_preset`) |
| Rig ledger | `asset_rig.json` | `status: rigged`, `rig_task_id`, `rig_type`, rig `model` |
| Previous report | `asset_animation.json` | Resume (never re-bill) |
| Settings | `settings.py` / `.env` | Enable switch, per-asset cap, Tripo key and base URL |

### Outputs (what it produces)

| Output | Location | Contents |
|--------|----------|----------|
| Animated GLB | `assets/<id>/model_animated.glb` | Mesh + skin + retargeted clips (in place) |
| Animation report | `asset_animation.json` | Per asset: `status`, intent → clip map (`animations`), `clip_names`, presets, task ids, `credits_estimate` |

### Where output goes next

- **`uthana_animation`** adds its own entries (`provider: "uthana"`) to the same `asset_animation.json`.
- **`asset_normalization`** picks `model_animated.glb` as the primary GLB and copies the `animations` map into `asset_normalization.json`.
- **`asset_optimization`** copies clips through verbatim into every LOD.
- **`scene_compile`** writes `mesh.animations` (intent → clip) into `scene.json`; the runtime `ISkeleton::play_clip` looks clips up by those keys.

### Models / providers

- **Tripo V3 API** only (no LLM). Base URL `https://openapi.tripo3d.ai/v3` by default (`TRIPO_BASE_URL` overrides, e.g. China-mainland accounts).
- Retarget flags: `out_format="glb"`, `bake_animation=True`, `export_with_geometry=True`, `animate_in_place=True`.

### Cost

~**20 credits per animation** (`_CREDITS_PER_ANIMATION`), at most 5 per call. Before any call the stage checks the Tripo balance against `billable_presets × 20` and raises if it is short.

### Failure behavior

- Balance probe fails or credits are short → the stage **fails** (top up, then Resume).
- A single asset's retarget fails → warning, `status: failed` in the report, the stage still **succeeds**; the asset stays a static skinned mesh.
- No recorded `rig_task_id` → warning, `status: missing_rig_task` (retarget cannot take a file upload).

---

## Detailed stage deep dive

### Early exit

`tripo_animation_enabled = false` → succeed immediately, no work.

### Choosing what to animate

For each manifest item:

1. If routing exists and the route's effective animation provider is not `tripo`, skip (Uthana or nothing).
2. Requests start as the item's semantic intents `[(intent, None), ...]`. If the route has no `fallback` and lists `tripo_preset` motions, those replace them as `[(key, exact_preset), ...]`. A character that **fell back** from Uthana keeps its semantic intents, because its routed motions are Uthana gaits Tripo cannot read.
3. Skip if `rigged` is false or there are no requests.
4. If `asset_rig.json` has no `status: rigged` entry → `status: not_rigged` (not an error: rig-check may have said the asset is not a creature).

### Intent → preset resolution

`resolve_tripo_motions(requests[:cap], rig_type, model)` in `tripo_client.py`, where `cap = clamp(tripo_animation_max_per_asset, 1, 5)`:

- An exact preset is used as-is only on a **v1.0 biped** rig (`v1.0-20240301`) and only if it is in `TRIPO_BIPED_V1_PRESETS` (`animation_catalog.py`).
- Otherwise the key is a semantic intent looked up by `presets_for_rig(rig_type, model)`: the v1.0 or v2.5 biped table, or for creature rigs (`quadruped`, `hexapod`, `octopod`, `serpentine`, `aquatic`) only locomotion, with `walk` and `run` sharing one preset.
- Presets are de-duplicated (billed once); intents with no preset are **dropped loudly** (`info` event), never substituted. No presets at all → `status: no_presets`.

### Retarget and download

`TripoClient.retarget(out_dir=assets/<id>/.anim_download, rig_task_id, animations=presets, out_format="glb", bake_animation=True, export_with_geometry=True, animate_in_place=True)`.

- `animate_in_place=True` always: baked root motion would move the mesh on top of the engine moving the entity (double distance, snap-back each loop).
- Immediately after download the report entry is saved as `status: downloaded` **before** any file move, so a failed move (file locked) is finished on the next run instead of paid again.
- `_finish_download` moves the file to `model_animated.glb`, removes the staging folder, and reads the real clip names from the GLB's JSON chunk (`_glb_animation_names`; a nameless clip becomes `animation_<i>`).

### Pairing intents to clips

`_pair_intents_to_clips` pairs **by name**, not position — Tripo does not return clips in request order (a run/slash/hurt/fall request came back fall/hurt/run/slash). `_name_key` normalises `preset:biped:slash`, `Armature|preset_slash`, `Slash` to `slash`. Presets with no name match take leftover clips in order; intents beyond the clips actually returned are omitted so `play_clip` never points at a missing clip. Fewer clips than presets → warning.

### Resume rules

- `status` `animated` with the GLB on disk → reused; `_repair_clip_map` re-pairs the intent → clip map against the file for free (older runs paired by position).
- `no_presets` / `not_rigged` → reused, except `not_rigged` is retried if `asset_rig.json` now says `rigged`.
- `downloaded` with a staged GLB → the move is finished without a new call.
- To force a re-run, delete `asset_animation.json`'s entry and `model_animated.glb`.

The report is pruned to current manifest ids (`prune_to_assets`) and written atomically.

### Report entry (`status: animated`)

`provider: "tripo"`, `retarget_task_id`, `rig_task_id`, `rig_type`, `model`, `requested_intents`, `dropped_intents`, `presets`, `clip_names`, `animations` (intent → clip), `animated_glb`, `animate_in_place: true`, `credits_estimate`.

### Settings

| Setting | Env var | Default |
|---------|---------|---------|
| `tripo_animation_enabled` | `TRIPO_ANIMATION_ENABLED` | `true` |
| `tripo_animation_max_per_asset` | `TRIPO_ANIMATION_MAX_PER_ASSET` | `4` (clamped to 1–5) |
| `tripo_api_key` | `TRIPO_API_KEY` | — |
| `tripo_base_url` | `TRIPO_BASE_URL` | unset (international V3) |

### Quest notes

Nothing Quest-specific happens here. Clip count matters later: skinning is CPU work, so `asset_optimization` gives animated characters a separate budget (`max_animated_agents`, animation LOD distances).

## See also

- [`docs/npc-and-ai.md`](../../../npc-and-ai.md) — character locomotion and clips
- [`docs/uthana.md`](../../../uthana.md) — the Uthana animation path
- [`docs/rendering.md`](../../../rendering.md) — skin-preserving LOD path
- [`docs/provider-setup.md`](../../../provider-setup.md) — Tripo key and region
