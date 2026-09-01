# Stage 7: Tripo 3D Models

> **Pipeline position:** after `reference_images` → before `asset_rigging`  
> **Target:** shared (PC VR and Quest)  
> **Primary code:** [`apps/orchestrator/ptv_orchestrator/pipeline/stage_tripo_models.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/stage_tripo_models.py)  
> **Primary artifacts:** `projects/<project_id>/assets/<asset_id>/model.glb`, `tripo_task.json`

## Main stage summary

### What this stage is

The **Tripo models stage** converts reference images (or text prompts) into textured, PBR **GLB** 3D meshes using the **Tripo V3 API**. Each asset gets a primary mesh file and a `tripo_task.json` recording the generation task id for later rigging.

### Why it exists

Your game needs actual 3D geometry on disk. Tripo image-to-3D produces game-ready meshes with materials; storing the task id avoids re-uploading the GLB when rigging runs.

### Inputs (what enters)

| Input | Location |
|-------|----------|
| AssetManifest | `asset_manifest.json` |
| Reference images | `images/<asset_id>/` (for `image_to_3d`) |
| Text prompts | item `reference` field (for `text_to_3d`) |

### Outputs (what it produces)

| Output | Location |
|--------|----------|
| Primary GLB | `assets/<asset_id>/model.glb` (or similar) |
| Task record | `assets/<asset_id>/tripo_task.json` |

### Where output goes next

- **`asset_rigging`** — uses `tripo_task.json` as rig input
- **`asset_normalization`** — validates the GLB chain

### Key concepts and terms

- **Tripo V3 API** — Direct HTTP to `openapi.tripo3d.ai/v3` (not the old V2 SDK). Flow: upload file → create generation task → poll → download `model_url`.
- **PBR + texture alignment** — Calls use `texture=True`, `pbr=True`, `texture_alignment="original_image"` per project rules.
- **Credit preflight** — Estimates ~20 credits × pending assets before starting; fails early if balance insufficient.
- **Idempotent skip** — Valid existing GLB (magic `glTF` header) skips re-generation.

### End-to-end flow (plain language)

1. Count assets needing new GLBs.
2. Preflight Tripo credit balance.
3. Per asset: upload reference or submit text prompt → poll task → download GLB.
4. Write `tripo_task.json` with task id.
5. Fail only if **zero** new GLBs produced when assets were pending.

---

## Detailed stage deep dive

### Idempotency pass (before any API)

For each manifest item, `_existing_glb(asset_dir)` checks for valid GLB magic `glTF`. Valid file → skip, add to `skipped` list.

If **no pending assets** after scan → succeed immediately without Tripo client.

### Credit preflight

```
pending = items needing new GLBs
client.get_balance()
require available >= max(20, len(pending) * 20)
```

Fails with RuntimeError before spending credits if balance too low.

### Per-asset generation

| strategy | Flow |
|----------|------|
| `image_to_3d` | `_pick_reference_image()` from `images/<id>/`; if missing → fallback `text_to_model` |
| `text_to_3d` | `client.text_to_model()` directly |

After success: download GLB to `assets/<id>/`, `_write_task_record()` → `tripo_task.json` with `task_id` for rigging.

Tripo V3 HTTP client (`ptv_providers.TripoClient`): `texture=True`, `pbr=True`, `texture_alignment=original_image`, model from `tripo_model_version`.

### Hard failure rule

If `attempted > 0` new generations and **zero** succeeded (`len(produced) == len(skipped)` relative to pending) → **RuntimeError** — stage fails.

Per-asset errors: log + `warning`, continue.

### SSE

`running` → `info` (reuse count, balance) → per-asset `info`/`warning` → `succeeded` or exception

### Settings

`tripo_api_key`, `tripo_model_version`, `tripo_base_url`

### tripo_task.json

Stores generation task_id for rigging input. Missing task file forces slower upload fallback in stage 8.

---

## Extend this folder

- `artifacts.md` — GLB naming and task record schema
- `failure-modes.md` — credit and polling errors

## See also

- [`docs/provider-setup.md`](../../../provider-setup.md) — Tripo setup
- Stage 8 [`08_asset_rigging`](../08_asset_rigging/README.md)
