# Stage 6: Reference Images

> **Pipeline position:** after `scene_spec` → before `tripo_models`  
> **Target:** shared (PC VR and Quest)  
> **Primary code:** [`apps/orchestrator/ptv_orchestrator/pipeline/stage_reference_images.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/stage_reference_images.py)  
> **Primary artifacts:** `projects/<project_id>/images/<asset_id>/*.{png,jpg,webp}`

## Main stage summary

### What this stage is

The **reference images stage** generates 2D reference pictures for each asset that will use Tripo's **image-to-3D** pipeline. These images are what Tripo turns into textured GLB meshes in the next stage.

### Why it exists

Tripo's best results come from a clear reference image aligned with your art direction. The manifest's `reference` text becomes an image prompt; Gemini/Imagen produces the actual pixels.

### Inputs (what enters)

| Input | Location |
|-------|----------|
| AssetManifest | `asset_manifest.json` |
| Per-item | `strategy == image_to_3d`, `reference` prompt text |

### Outputs (what it produces)

| Output | Location |
|--------|----------|
| Reference images | `projects/<id>/images/<asset_id>/ref_*.png` (or jpg/webp) |

### Where output goes next

**`tripo_models`** uploads the best reference image per asset to Tripo V3 image-to-model.

### Key concepts and terms

- **image_to_3d vs text_to_3d** — Only `image_to_3d` assets get reference images. `text_to_3d` assets skip this stage and go straight to Tripo text-to-model.
- **Idempotent skip** — If images already exist on disk from a prior run, that asset is skipped (no API call). Resume-friendly.
- **Per-asset failure** — One bad asset logs a warning; the stage still succeeds if others produce images.

### End-to-end flow (plain language)

1. Loop manifest items with `strategy == image_to_3d`.
2. Skip items that already have images on disk.
3. Compose prompt from asset `reference` field + style hints.
4. Call **Google GenAI** (`gemini-3.1-flash-image`) to generate an image.
5. Save to `images/<asset_id>/`.
6. Record success with count of images produced.

---

## Detailed stage deep dive

### Loop structure (`run`)

1. emit + record running
2. Load asset_manifest
3. For each item where `strategy == image_to_3d`:
   - `item_dir = projects/<id>/images/<asset_id>/`
   - **`_existing_images(item_dir)`** — if any `.png/.jpg/.webp` exists → skip (idempotent)
   - Lazy-create `GoogleImageClient` on first needed asset
   - `_compose_prompt(item)` from manifest `reference` fields
   - `client.generate(..., prefer="gemini")` → write bytes to disk
   - On exception: `append_log`, emit `warning`, **continue next asset**
4. record succeeded even if some assets warned

### Idempotent resume

Delete `images/<asset_id>/` to force regeneration for that asset. If all image_to_3d assets already have files, **no Google client is constructed** — resume works without API key.

### Artifact paths

Writes: `images/<asset_id>/<asset_id>_ref*.png` (or jpg/webp)

Reads: `asset_manifest.json` only (scene_spec is ordering predecessor, not data dependency)

### External API

`GoogleImageClient` via `ptv_providers`:

- `settings.google_api_key`
- `settings.gemini_image_model` (default `gemini-3.1-flash-image`)
- `response_modalities=["IMAGE"]`

### SSE

`running` → `info` (reuse/skip counts) → `warning` per failed asset → `succeeded`

### Failure modes

Per-asset failure never fails stage. Missing key fails on first asset needing generation.

### Filename pattern

`<asset_id>_ref*.png` under `images/<asset_id>/`. Tripo stage picks best image via `_pick_reference_image()`.

---

## Extend this folder

- `artifacts.md` — image naming conventions
- `failure-modes.md` — resume and partial success

## See also

- [`docs/provider-setup.md`](../../../provider-setup.md) — API keys
- Stage 7 [`07_tripo_models`](../07_tripo_models/README.md)
