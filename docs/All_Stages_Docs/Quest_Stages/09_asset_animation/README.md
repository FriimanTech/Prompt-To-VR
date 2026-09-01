> **Shared content:** Identical to [PC_VR_Stages/09_asset_animation](../../PC_VR_Stages/09_asset_animation/README.md). Update both copies when this stage changes.

# Stage 9: Animation Retarget

> **Pipeline position:** after `asset_rigging` → before `asset_normalization`  
> **Target:** shared (PC VR and Quest)  
> **Primary code:** [`apps/orchestrator/ptv_orchestrator/pipeline/stage_asset_animation.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/stage_asset_animation.py)  
> **Primary artifacts:** `projects/<project_id>/assets/<asset_id>/model_animated.glb`, `asset_animation.json`

## Main stage summary

### What this stage is

The **animation retarget stage** adds movement clips to rigged characters. It maps **semantic animation intents** from the manifest (like `walk`, `run`, `shoot`) to Tripo preset ids, calls retarget API (~20 credits per animation), and records the **actual glTF clip names** Tripo produced.

### Why it exists

A rigged mesh without clips is a statue that slides across the floor. The engine's `builtin.character_locomotion` plays clips by semantic name via `ctx.skeleton->play_clip(self, "walk", true)` — but only if this stage wrote the clip contract.

### Inputs (what enters)

| Input | Location |
|-------|----------|
| AssetManifest | `rigged: true` + non-empty `animations[]` intents |
| Rig metadata | `asset_rig.json` (`rig_task_id`, `rig_type`, model) |

### Outputs (what it produces)

| Output | Location |
|--------|----------|
| Animated GLB | `assets/<id>/model_animated.glb` |
| Clip contract | `asset_animation.json` — intent → glTF clip name |

### Where output goes next

- **`asset_normalization`** — picks animated GLB as primary
- **`scene_compile`** — writes `mesh.animations` map into `scene.json`
- **Runtime** — `ISkeleton::play_clip` uses semantic keys

### Key concepts and terms

- **Animation intent** — Semantic label (`idle`, `walk`, `run`, `attack`, …) defined in schema, not a Tripo preset id. `resolve_animations()` maps intent → preset based on `rig_type`.
- **animate_in_place=True** — Clips must not bake root motion; the engine moves the entity separately. Double movement causes foot sliding and snap-back.
- **export_with_geometry=True** — One GLB contains mesh, skin, and all clips (runtime loads a single file).
- **Clip contract** — Scene JSON stores `{ "walk": "Walk_Loop", "run": "Run_Fast" }` so generated C++ never hard-codes Tripo internal names.

### End-to-end flow (plain language)

1. For each rigged asset with animation intents, load rig record.
2. Resolve intents to Tripo presets (dedupe shared presets).
3. Call retarget (≤5 presets per API call).
4. Read clip names back from GLB JSON chunk.
5. Write `asset_animation.json` and `model_animated.glb`.

---

## Detailed stage deep dive

### Early exit

`tripo_animation_enabled == false` → succeed, no work.

### Eligibility

Manifest item must have `rigged: true` and non-empty `animations[]` (semantic intents). Requires successful `asset_rig.json` entry with `rig_task_id` — **no upload fallback**.

### Intent resolution

`resolve_animations(intents, rig_type, model)` maps semantic intents (`walk`, `run`, …) to Tripo preset ids. Unsupported intents dropped loudly. Duplicate presets billed once.

Cap: `min(5, tripo_animation_max_per_asset)` intents per asset.

### Retarget call

`client.retarget(rig_task_id, presets, animate_in_place=True, export_with_geometry=True)`

- **animate_in_place** — engine moves body; root motion would double distance
- **export_with_geometry** — single GLB with mesh + skin + clips

Output: `model_animated.glb`. `_glb_animation_names()` reads actual clip names from GLB JSON chunk → `asset_animation.json` intent→clip map (clip contract).

### Credit preflight

`billable_presets * 20` credits required.

### Resume

Report status `animated` + file on disk → skip. Delete report + animated GLB to force.

### Failure philosophy

Per-asset soft fail; stage succeeds. Missing rig_task_id → warning skip.

### Runtime clip contract path

asset_animation.json → normalization merge → scene.json mesh.animations → ISkeleton::play_clip semantic keys.

---

## Extend this folder

- `schemas.md` — AnimationIntent enum
- `artifacts.md` — clip contract format

## See also

- [`docs/npc-and-ai.md`](../../../npc-and-ai.md) — locomotion + clips
- Stage 14 [`14_asset_optimization`](../14_asset_optimization/README.md) — skin-preserving LOD path
