> **Shared content:** Identical to [PC_VR_Stages/08_asset_rigging](../../PC_VR_Stages/08_asset_rigging/README.md). Update both copies when this stage changes.

# Stage 8: Tripo Auto-Rigging

> **Pipeline position:** after `uthana_rigging` → before `asset_animation`  
> **Target:** shared (PC VR and Quest)  
> **Primary code:** [`apps/orchestrator/ptv_orchestrator/pipeline/stage_asset_rigging.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/stage_asset_rigging.py)  
> **Primary artifacts:** `projects/<project_id>/assets/<asset_id>/model_rigged.glb`, `projects/<project_id>/asset_rig.json`

## Main stage summary

### What this stage is

Adds a Tripo skeleton and skin weights to every manifest item with `rigged: true` that is routed to Tripo. It uses Tripo's free **rig-check** and then, only if the answer is riggable, the paid **rig** call (~30 credits).

### Why it exists

Tripo meshes arrive static. Characters that move need bones before `asset_animation` can retarget clips. It runs before `asset_normalization` so every later stage (validation, sockets, LODs, scene compile) sees the rigged mesh as the primary GLB.

### Which assets

An item is a candidate when `rigged: true` **and** either it has no route in `character_routing.json` or its `effective_rig_provider()` is `tripo` (Tripo-routed, or an Uthana route that fell back to Tripo). Uthana-rigged characters are skipped.

### Inputs

| Input | Location |
|-------|----------|
| AssetManifest | `asset_manifest.json` (`rigged`, already reconciled by `npc_plan`, `character_routing`, `uthana_rigging`) |
| Routing plan | `character_routing.json` |
| Un-rigged source GLB | `assets/<id>/*.glb` excluding names containing `rigged`, `animated`, `_lod` (prefers `pbr`, then `model`) |
| Generation task id | `assets/<id>/tripo_task.json` (preferred rig input) |
| Cached rig-check | `tripo_rig_check.json` (from `character_routing`, used when the source hash matches) |

### Outputs

| Output | Location |
|--------|----------|
| Rigged GLB | `assets/<id>/model_rigged.glb` |
| Rig report | `asset_rig.json` (keys sorted, pruned to manifest assets) |

### Where output goes next

`asset_animation` reads `rig_task_id`, `rig_type` and `model` from `asset_rig.json` (retarget needs a rig task id). `asset_normalization` picks `model_rigged.glb` as the primary GLB.

### Provider / cost

| Call | Endpoint | Cost |
|------|----------|------|
| rig-check | `POST /v3/animations/rig-check` (`input` = task id, else uploaded file token) | free |
| rig | `POST /v3/animations/rig` with `input`, `model`, `rig_type`, `spec: "tripo"`, `out_format: "glb"` | ~30 credits |

Rig `model` comes from `rig_type`: `v1.0-20240301` for `biped`, `v2.5-20260210` for `quadruped`, `hexapod`, `octopod`, `avian`, `serpentine`, `aquatic`. `spec` is always `tripo` because Mixamo-spec skeletons make every later retarget fail with Tripo error 1004. `TRIPO_MODEL_VERSION` is deliberately not passed (it selects the image-to-3D checkpoint).

### Failure behavior

- Balance preflight (`GET /v3/account/balance`): needs `pending × 30` credits, otherwise `RuntimeError`. A failed balance call also fails the stage.
- Per-asset errors are logged, warned and recorded as `status: failed`; the stage still **succeeds** (the static mesh still renders).

---

## Detailed stage deep dive

### Algorithm

1. `tripo_rigging_enabled == false` → succeed immediately.
2. For each candidate:
   - prior `status: downloaded` (paid, but the move to the final name failed) → finish moving `.rig_download/model.glb` to `model_rigged.glb`; no new rig;
   - prior `rigged` with the file present, or `not_riggable` → reuse;
   - no source GLB → `status: missing_source`, warning;
   - else pending.
3. No pending → succeed without Tripo calls.
4. Balance preflight.
5. Per pending asset: use the cached rig-check if valid (same `source_sha256`, has a `task_id`, no error), else call rig-check. Not riggable → `not_riggable`, skip the paid call. Riggable → rig into the scratch dir `.rig_download/` (the client always names downloads `model.glb`, which would overwrite the source), write a `downloaded` entry **before** moving files (so a paid rig task id is never lost), move to `model_rigged.glb`, write the `rigged` entry.

### `asset_rig.json` entry

| Field | Meaning |
|-------|---------|
| `status` | `rigged`, `not_riggable`, `downloaded`, `failed`, `missing_source` |
| `provider` | `tripo` |
| `rig_check_task_id`, `rig_task_id` | Tripo task ids |
| `rig_type`, `model`, `spec`, `out_format` | what was sent |
| `source_glb`, `rigged_glb` / `staged_glb` | repo-relative paths |
| `credits_estimate` | 30 |
| `error` | on failure |

### Settings

| Setting | Env var | Default |
|---------|---------|---------|
| `tripo_rigging_enabled` | `TRIPO_RIGGING_ENABLED` | `true` |
| `tripo_api_key` | `TRIPO_API_KEY` | none |
| `tripo_base_url` | `TRIPO_BASE_URL` | international V3 |

### Resume / regeneration

Delete the asset's entry in `asset_rig.json` and `model_rigged.glb` to force a re-rig (costs another ~30 credits).

### SSE events

`running` → `info` (reuse, balance, `riggable as <type> -> rigging with <model>`, not riggable) → `warning` → `succeeded` (`"N rigged, M not riggable, K reused, J failed"`).

### Quest notes

Nothing target-specific here. `asset_optimization` later decimates rigged meshes on a skin-preserving path so every LOD keeps the same skeleton.

## See also

- [`docs/uthana.md`](../../../uthana.md) — how routing splits Tripo and Uthana
- [`docs/npc-and-ai.md`](../../../npc-and-ai.md)
- [`docs/rendering.md`](../../../rendering.md) — skinned LODs
- Previous: [`08a_uthana_rigging`](../08a_uthana_rigging/README.md) · Next: [`09_asset_animation`](../09_asset_animation/README.md)
