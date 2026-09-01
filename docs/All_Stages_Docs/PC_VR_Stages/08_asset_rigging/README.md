# Stage 8: Auto-Rigging

> **Pipeline position:** after `tripo_models` → before `asset_animation`  
> **Target:** shared (PC VR and Quest)  
> **Primary code:** [`apps/orchestrator/ptv_orchestrator/pipeline/stage_asset_rigging.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/stage_asset_rigging.py)  
> **Primary artifacts:** `projects/<project_id>/assets/<asset_id>/model_rigged.glb`, `asset_rig.json`

## Main stage summary

### What this stage is

The **auto-rigging stage** adds a skeleton to character meshes that need animation. For each manifest item with `rigged: true`, it calls Tripo's free **rig-check** endpoint, then (if riggable) the paid **rig** endpoint (~30 credits per asset).

### Why it exists

Tripo meshes arrive static. Characters that walk, shoot, or die need bones and skin weights before animation clips can be retargeted. Running rigging **before** normalization ensures every downstream stage (validation, sockets, LODs, scene compile) sees the rigged mesh as the primary asset.

### Inputs (what enters)

| Input | Location |
|-------|----------|
| AssetManifest | `rigged: true` items (after npc_plan reconcile) |
| Source GLB | `assets/<id>/model.glb` |
| Tripo task id | `assets/<id>/tripo_task.json` (preferred rig input) |

### Outputs (what it produces)

| Output | Location |
|--------|----------|
| Rigged GLB | `assets/<id>/model_rigged.glb` |
| Rig record | `asset_rig.json` (per project, per asset status) |

### Where output goes next

**`asset_animation`** requires `rig_task_id` from `asset_rig.json` to retarget clips.

### Key concepts and terms

- **rig-check (free)** — Tripo answers `riggable: true/false` before billing. Props and weapons fail here at zero cost.
- **rig (~30 credits)** — Produces skeleton + skinned mesh. Uses `spec="tripo"` (Mixamo-spec rigs break retarget with error 1004).
- **rig_type** — `biped`, `quadruped`, etc. Determines which animation presets exist later.
- **Soft failure** — One asset failing rigging does not fail the stage; static mesh remains usable.

### End-to-end flow (plain language)

1. Loop manifest items where `rigged: true`.
2. Skip if `asset_rig.json` already records success (idempotent).
3. Call rig-check with task id or file token.
4. If riggable, call rig → download `model_rigged.glb`.
5. Record `rig_type`, `rig_task_id`, model version in `asset_rig.json`.

---

## Detailed stage deep dive

### Early exit paths

1. `tripo_rigging_enabled == false` → succeed immediately, no artifacts touched
2. No manifest items with `rigged: true` → succeed
3. All rigged assets already in `asset_rig.json` with `model_rigged.glb` on disk → succeed (reuse)

### Per-asset pipeline

| Step | Tripo API | Cost |
|------|-----------|------|
| rig-check | `POST /v3/animations/rig-check` | Free |
| rig | `POST /v3/animations/rig` | ~30 credits |

Input: `tripo_task.json` task_id preferred (not re-upload GLB). Output staged then moved to `model_rigged.glb` (never overwrite raw `model.glb`).

Always `spec=tripo`, `out_format=glb` — Mixamo spec breaks retarget (error 1004).

### Report file: `asset_rig.json`

Per asset records: status (`rigged`, `not_riggable`, `failed`), `rig_type`, `rig_task_id`, model version. Resume reads this — delete report + rigged GLB to force re-rig.

### Credit preflight

`available >= len(pending) * 30` or RuntimeError.

### Failure philosophy

**Stage always succeeds** even with per-asset failures — static mesh remains usable. Warnings emitted per failure.

### SSE

`running` → `info` (reuse, balance, rig_type) → `warning` → `succeeded`

### Report statuses

| status | Next stage behavior |
|--------|---------------------|
| rigged | animation may run |
| not_riggable | skip animation |
| missing_source | warning only |

---

## Extend this folder

- `artifacts.md` — asset_rig.json fields
- `failure-modes.md` — soft fail vs hard fail

## See also

- Stage 3 [`03_npc_plan`](../03_npc_plan/README.md) — rig flag reconciliation
- Stage 9 [`09_asset_animation`](../09_asset_animation/README.md)
