# Stage 10: Asset Normalization

> **Pipeline position:** after `asset_animation` → before `asset_contract`  
> **Target:** shared (PC VR and Quest)  
> **Primary code:** [`apps/orchestrator/ptv_orchestrator/pipeline/stage_asset_normalize.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/stage_asset_normalize.py)  
> **Primary artifacts:** `projects/<project_id>/asset_normalization.json`

## Main stage summary

### What this stage is

The **asset normalization stage** validates every asset's primary GLB file and records metadata: path, scale, collision shape, animation map, validity flags. It does **not** modify geometry.

### Why it exists

Downstream stages need a trusted index of which GLB to load, whether it is valid, and what collision/animation metadata applies. Centralizing inspection here avoids each stage re-implementing GLB header checks.

### Inputs (what enters)

| Input | Location |
|-------|----------|
| AssetManifest | `asset_manifest.json` |
| GLB files | `assets/<id>/` (best available variant) |
| Animation contract | `asset_animation.json` (optional merge) |

### Outputs (what it produces)

| Output | Location |
|--------|----------|
| Normalization index | `asset_normalization.json` |

Per asset: `glb_path`, `target_scale_meters`, `collision_shape`, `animations`, `status` (`ok`, `warn_no_material`, `invalid`, `missing`).

### Where output goes next

- **`asset_contract`** — valid GLB paths for socket derivation
- **`asset_optimization`** — source meshes for LOD chain
- **`world_generation`** — object GLB map for prop placement
- **`scene_compile`** — mesh metadata in final scene

### Key concepts and terms

- **Primary GLB pick order** — Prefers `*animated*` → `*rigged*` → `*pbr*` → `*model*` so the richest variant is validated.
- **Validation only** — Parses GLB header + JSON chunk; no mesh rewriting (optimization stage handles decimation later).
- **Deterministic** — No AI or external API calls.

### End-to-end flow (plain language)

1. For each manifest item, locate best GLB on disk.
2. `_inspect_glb()` — check magic bytes, JSON chunk, materials.
3. Merge animation names from `asset_animation.json` if present.
4. Write per-asset record to `asset_normalization.json`.

---

## Detailed stage deep dive

### Deterministic full rewrite every run

No LLM, no skip — always re-inspects all assets and overwrites `asset_normalization.json`.

### Primary GLB selection (`_pick_primary_glb`)

Preference order in `assets/<id>/`:

1. `*animated*` (e.g. `model_animated.glb`)
2. `*rigged*`
3. `*pbr*`
4. `*model*`

Ensures downstream sees richest available variant.

### Inspection (`_inspect_glb`)

Pure Python parse (no tinygltf):

- Verify 12-byte header magic `glTF`
- Parse JSON chunk: mesh count, material presence
- Status: `ok`, `warn_no_material`, `invalid`, `missing` (no file)

Does **not** modify geometry.

### Animation map merge

`_animation_clip_names()` loads `asset_animation.json` if present and attaches intent→clip names to normalization record.

### Per-asset record fields

`glb_path`, `target_scale_meters`, `collision_shape` (from manifest), `animations`, `status`, mesh/material metadata.

### Downstream consumers

asset_contract (paths), world_generation (object map), asset_optimization (source meshes), scene_compile (mesh block).

### SSE

`running` → `succeeded` with ok/rejected counts

### Failure modes

Never fails stage — bad assets recorded as invalid/missing in report.

### Pick order rationale

Animated GLB includes mesh+skin+clips — richest variant for validation index. Optimization reads same paths then builds LODs.

---

## Extend this folder

- `artifacts.md` — normalization record fields
- `failure-modes.md` — status enum meanings

## See also

- Stage 11 [`11_asset_contract`](../11_asset_contract/README.md)
- Stage 14 [`14_asset_optimization`](../14_asset_optimization/README.md)
