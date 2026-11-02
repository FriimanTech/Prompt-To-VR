> **Shared content:** Identical to [PC_VR_Stages/07_tripo_models](../../PC_VR_Stages/07_tripo_models/README.md). Update both copies when this stage changes.

# Stage 7: Tripo Models

> **Pipeline position:** after `reference_images` → before `character_routing`  
> **Target:** shared (PC VR and Quest)  
> **Primary code:** [`apps/orchestrator/ptv_orchestrator/pipeline/stage_tripo_models.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/stage_tripo_models.py)  
> **Primary artifacts:** `projects/<project_id>/assets/<asset_id>/model.glb`, `assets/<asset_id>/tripo_task.json`

## Main stage summary

### What this stage is

This stage turns every object asset into a textured, PBR GLB with Tripo's **V3** API (`https://openapi.tripo3d.ai/v3`), called directly over `httpx` (not the `tripo3d` SDK, whose base URL is still V2). Image-to-3D is used when a reference image exists; text-to-3D otherwise.

### Why it exists

The engine renders real meshes. Only objects (characters, props, fx, ui) are generated here; the VR environment is authored in C++ by `world_generation`, not by Tripo.

### Inputs

| Input | Location |
|-------|----------|
| AssetManifest | `asset_manifest.json` — `strategy`, `reference.prompt`, `tripo.texture_quality`, `tripo.face_limit` |
| Reference image | first file (sorted) in `images/<asset_id>/` with `.png/.jpg/.jpeg/.webp` |

### Outputs

| Output | Location |
|--------|----------|
| Generated mesh | `assets/<asset_id>/model.glb` (the client always names its download `model<ext>`) |
| Generation task record | `assets/<asset_id>/tripo_task.json` = `{"asset_id", "task_id"}` |

### Where output goes next

- **`character_routing`** runs Tripo's free rig-check on these meshes (using the task id).
- **`uthana_rigging`** uploads `model.glb` to Uthana; **`asset_rigging`** rigs via the recorded `task_id` (no re-upload).
- **`asset_normalization`** validates the primary GLB.

### Provider / model / cost

| Item | Value |
|------|-------|
| API | Tripo V3, `POST /v3/files` → `POST /v3/generation/image-to-model` (or `/text-to-model`) → `GET /v3/tasks/{id}` → download `output.model_url` |
| Model | `v3.1-20260211` by default; V3 accepts only `v3.1-20260211`, `v3.0-20250812`, `v2.5-20250123`, `P1-20260311` (validated at client construction) |
| Cost | ~20 credits per asset (a preflight floor, not a quote) |

### Failure behavior

- **Preflight balance check** (`GET /v3/account/balance`) before any paid call: needs `max(20, pending × 20)` credits for the assets still missing a GLB; otherwise `RuntimeError` ("Insufficient Tripo credits..."). A failed balance call also fails the stage (usually a bad key or wrong region base URL).
- Per-asset errors are logged and emitted as warnings; the loop continues.
- **Hard fail** when there was pending work but zero new GLBs were produced.

---

## Detailed stage deep dive

### Files and functions

| Symbol | Role |
|--------|------|
| `run(ctx)` | Stage entry |
| `_existing_glb(item_dir)` | Idempotency: any `*.glb` under the asset dir whose first 4 bytes are `glTF` (a truncated file is regenerated). Sort prefers `animated`, `rigged`, `pbr`, `model` names |
| `_pick_reference_image(images_dir)` | First image alphabetically |
| `_write_task_record(item_dir, asset_id, task_id)` | Writes `tripo_task.json`; an OSError is swallowed |
| `TripoClient`, `TripoConfig` | [`packages/provider-clients/ptv_providers/tripo_client.py`](../../../../packages/provider-clients/ptv_providers/tripo_client.py) |

### Algorithm

1. Partition manifest items with `strategy` in (`image_to_3d`, `text_to_3d`) into already-on-disk (reused) and pending.
2. No pending → succeed without touching Tripo ("All N GLB(s) already on disk; no Tripo calls made").
3. Build `TripoClient`; run the balance preflight.
4. For each pending item:
   - `image_to_3d` with an image → `image_to_model(image_path, texture_quality, face_limit)`;
   - `image_to_3d` without an image → logged, then `text_to_model(prompt=reference.prompt, ...)`;
   - `text_to_3d` → `text_to_model`.
5. Record the GLB path and write `tripo_task.json`.
6. Hard-fail if nothing new was produced; otherwise succeed with a reuse/new count.

### Request body (`TripoClient._common_body` + per-endpoint fields)

| Field | Value |
|-------|-------|
| `model` | resolved model version (always sent; V3 has no server default) |
| `texture` / `pbr` | `true` / `true` |
| `texture_quality` | per asset (`tripo.texture_quality`, default `standard`) |
| `geometry_quality` | `standard`; omitted for P1 and pre-v3 models, which reject it |
| `enable_image_autofix` | `true` |
| `auto_size` | `true` (approximate real-world scale) |
| `face_limit` | per asset if set (64–50000) |
| image-to-model only | `input` = uploaded `file_token`, `orientation: "align_image"`, `texture_alignment: "original_image"` |
| text-to-model only | `prompt` (and optional `negative_prompt`) |

### Client behavior

- Uploads self-pace to one every 6.5 s (V3 allows ~10 uploads/min).
- Polls every 3 s, timeout 600 s. Terminal statuses: `success`, `failed`, `cancelled`.
- Retries (up to 4 attempts, exponential backoff, honours `Retry-After`) on transport errors, HTTP 429/5xx and Tripo codes 1007 (rate quota) / 2000 (concurrency cap). HTTP 401 raises immediately with a region hint. Codes 1004 (malformed, often a bad `model`), 2008 (moderation) and 2010 (credits) raise with an actionable hint.
- Output URLs are signed and expire, so they are downloaded immediately.

### Settings

| Setting | Env var | Default |
|---------|---------|---------|
| `tripo_api_key` | `TRIPO_API_KEY` | none |
| `tripo_model_version` | `TRIPO_MODEL_VERSION` | empty → `v3.1-20260211` |
| `tripo_base_url` | `TRIPO_BASE_URL` | empty → `https://openapi.tripo3d.ai/v3` (China accounts: `https://openapi.tripo3d.com/v3`; a missing `/v3` is appended) |

### Resume / regeneration

Delete `assets/<asset_id>/` to force a new Tripo call. Credits are only spent on pending assets.

### SSE events

`running` → `info` (reuse count, balance, per-asset status) → `warning` per failure → `succeeded` or `failed`.

### Quest notes

Raw Tripo output is roughly 500k triangles. Nothing is decimated here; `asset_optimization` builds the Quest LOD chain later.

## See also

- [`docs/provider-setup.md`](../../../provider-setup.md) — Tripo key and region
- [`docs/rendering.md`](../../../rendering.md) — LODs and triangle budgets
- Previous: [`06_reference_images`](../06_reference_images/README.md) · Next: [`07b_character_routing`](../07b_character_routing/README.md)
