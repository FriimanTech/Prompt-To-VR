# Stage 6: Reference Images

> **Pipeline position:** after `scene_spec` → before `tripo_models`  
> **Target:** shared (PC VR and Quest)  
> **Primary code:** [`apps/orchestrator/ptv_orchestrator/pipeline/stage_reference_images.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/stage_reference_images.py)  
> **Primary artifacts:** `projects/<project_id>/images/<asset_id>/<asset_id>_ref_00.png` (or `.jpg`), `images/<asset_id>/reference_prompt.txt`

## Main stage summary

### What this stage is

The reference images stage draws one 2D picture per object asset whose `strategy` is `image_to_3d`. Tripo turns that picture into a textured GLB in the next stage. Images are generated with Google's Gemini native image model (`gemini-3.1-flash-image`); Imagen is no longer used.

### Why it exists

Tripo image-to-3D gives far better meshes from a clean, well-framed picture than from text. This stage also decides the **pose** a character is drawn in (T-pose for humanoids, neutral side-on stance for quadrupeds), because rigging later depends on it. That decision is made in code from facts already on disk, not taken on trust from the asset-manifest agent.

### Inputs

| Input | Location | Used for |
|-------|----------|----------|
| AssetManifest | `projects/<id>/asset_manifest.json` | `strategy`, `reference.prompt`, `reference.negative_prompt`, `reference.aspect_ratio` (default `1:1`), `reference.number_of_images` (pinned to 1), `reference.pose` (shape hint only), `reference.coverage`, `style_notes`, `character_role` |
| NpcPlan | `projects/<id>/npc_plan.json` (optional) | Which asset ids are NPC actors (`npcs[].asset_id`) and so get a character pose |

### Outputs

| Output | Location |
|--------|----------|
| Reference image(s) | `images/<asset_id>/<asset_id>_ref_00.png` (`.jpg` if Gemini returns JPEG) |
| Exact prompt sent | `images/<asset_id>/reference_prompt.txt` |
| Per-asset errors | project log `reference_images` |

### Where output goes next

- **`tripo_models`** uploads the first image (alphabetical) in `images/<asset_id>/` to Tripo image-to-model.
- **`character_routing`** shows the image and reads `reference_prompt.txt` when it judges a character's body plan.

### Models / providers

| Provider | Model | Call |
|----------|-------|------|
| Google GenAI (`google-genai` package) | `gemini-3.1-flash-image` (setting `gemini_image_model`) | `client.models.generate_content(..., config=GenerateContentConfig(response_modalities=["IMAGE"], image_config=ImageConfig(aspect_ratio=...)))` |

No reasoning-effort setting applies. Cost is Google's per-image price; there are no Tripo credits here.

### Failure behavior

- One asset failing (API error, no inline image returned) is logged, emitted as a `warning`, and the loop continues. The stage still **succeeds**.
- A missing `GOOGLE_API_KEY` only fails when an asset actually needs generating; it surfaces as a per-asset warning, not a stage failure. An asset with no image later falls back to Tripo **text-to-3D** in `tripo_models`.

---

## Detailed stage deep dive

### Files and functions

| Symbol | Role |
|--------|------|
| `run(ctx)` | Stage entry |
| `resolve_pose(item, npc_asset_ids)` | Decides `natural` / `t_pose` / `neutral_quadruped` |
| `_npc_asset_ids(ctx)` | Reads `npc_plan.json`; a missing or malformed plan returns an empty set (never fails the stage) |
| `compose_prompt(item, pose)` | Base prompt + `Style:` line + pose clause + coverage clause |
| `compose_negative_prompt(item, pose)` | Asset negatives + pose negatives + coverage negatives, comma-joined |
| `_existing_images(item_dir)` | Idempotency check |
| `GoogleImageClient.generate(...)` | [`packages/provider-clients/ptv_providers/google_image_client.py`](../../../../packages/provider-clients/ptv_providers/google_image_client.py) |

`compose_prompt` and `resolve_pose` are also imported by `stage_character_routing` to rebuild the prompt when `reference_prompt.txt` is missing.

### Algorithm

1. Load the manifest and the NPC actor ids.
2. For each item with `strategy == image_to_3d` (text-to-3D items are skipped):
   1. If `images/<asset_id>/` already holds any `.png/.jpg/.jpeg/.webp`, reuse it and emit `info` (no API call).
   2. `resolve_pose`:
      - an asset is a *character actor* when `character_role` is `player_avatar` or `npc`, or its id is in the NpcPlan (including `slide_no_animation` NPCs, so the GLB stays reusable in other 3D tools);
      - non-actors are always `natural` (e.g. a `category: character` duck used only as a grabbable prop);
      - actors get `neutral_quadruped` only if the agent's hint said so, otherwise `t_pose`.
   3. Compose the prompt. T-pose and quadruped each append a fixed clause and fixed negatives; `coverage == neck_down` (posed characters only) appends a "feet to neck, no head" clause and negatives.
   4. Write `reference_prompt.txt`, then call Gemini with `filename_prefix="<asset_id>_ref"`, `prefer="gemini"`.
3. Record the stage succeeded with every image path (new and reused) as artifacts.

### Provider details

- The Google client is built **lazily**: if every asset is already imaged, no client is created and the stage runs without a Google key.
- Gemini has no negative-prompt field, so the client folds it into the text as `Avoid in the image: ...`.
- `prefer="imagen"` and `generate_with_imagen` still exist but are deprecated (Imagen 4 endpoints discontinued 2026-08-17) and are not used by the pipeline.
- Output files are named `<prefix>_<index:02d>.png|.jpg` from the inline image MIME type.

### Settings

| Setting | Env var | Default |
|---------|---------|---------|
| `google_api_key` | `GOOGLE_API_KEY` | none |
| `gemini_image_model` | `GEMINI_IMAGE_MODEL` | `gemini-3.1-flash-image` |

### Resume / regeneration

Delete `images/<asset_id>/` to force a new picture for that asset. Note that a new picture does not by itself re-run Tripo: `tripo_models` skips any asset that already has a valid GLB.

### SSE events

`running` → `info` (pose chosen, reuse, image count per asset) → `warning` per failed asset → `succeeded` (`"N reference images (M asset(s) reused from disk)"`).

### Quest notes

Nothing Quest-specific happens here; the same images feed both targets.

## See also

- [`docs/provider-setup.md`](../../../provider-setup.md) — API keys
- [`docs/npc-and-ai.md`](../../../npc-and-ai.md) — NpcPlan, which drives pose
- [`docs/uthana.md`](../../../uthana.md) — why T-posed humanoids matter for rigging
- Next: [`07_tripo_models`](../07_tripo_models/README.md)
