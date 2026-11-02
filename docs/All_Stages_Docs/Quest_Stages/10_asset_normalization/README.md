> **Shared content:** Identical to [PC_VR_Stages/10_asset_normalization](../../PC_VR_Stages/10_asset_normalization/README.md). Update both copies when this stage changes.

# Stage 10: Asset Normalization

> **Pipeline position:** after `uthana_animation` → before `socket_generation`  
> **Target:** shared (PC VR and Quest)  
> **Primary code:** [`apps/orchestrator/ptv_orchestrator/pipeline/stage_asset_normalize.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/stage_asset_normalize.py)  
> **Primary artifacts:** `projects/<project_id>/asset_normalization.json`

## Main stage summary

### What this stage is

The **asset normalization stage** picks each asset's primary GLB, validates it, and writes one index of per-asset metadata: path, scale, collision shape, rig facts, and the animation clip contract. Despite the name it does **not** modify geometry.

### Why it exists

Several stages write GLBs beside each other (`model.glb`, `model_rigged.glb`, `model_uthana_rigged.glb`, `model_animated.glb`). Later stages need one trusted answer to "which file is this asset, is it valid, is it rigged, and what are its clips" — this index is that answer.

### Inputs (what enters)

| Input | Location |
|-------|----------|
| AssetManifest | `asset_manifest.json` (`target_scale_meters`, `collision_shape`) |
| GLB files | `assets/<id>/**/*.glb` |
| Animation report | `asset_animation.json` (Tripo and Uthana entries) |

### Outputs (what it produces)

| Output | Location |
|--------|----------|
| Normalization index | `asset_normalization.json` |

Per asset: `status` (`ok`, `warn_no_material`, `invalid`, `missing`), `glb_path` (repo-relative), `target_scale_meters`, `collision_shape`, header/JSON facts (`gltf_version`, `generator`, `num_meshes`, `num_primitives`, `num_materials`, `num_textures`, `has_any_material`, `has_any_texture`, `num_skins`, `joint_count`, `animation_clips`, `byte_length`), and when present `animations`, `animation_speed_overrides`, `animation_provider`, `bind_sole_y`.

### Where output goes next

- **`socket_generation`** and **`asset_contract`** — the GLB to analyse (status `ok` / `warn_no_material` only).
- **`world_generation`** — object GLB map.
- **`asset_optimization`** — source meshes for LODs (it later adds `glb_source` and repoints `glb_path` at LOD0).
- **`scene_compile`** — mesh metadata, clip map, clip speeds.
- **`quest_runtime_verify`** — reads it too.

### Models / providers

None. Pure Python, deterministic, no network.

### Failure behavior

Never fails. Bad or missing GLBs are recorded as `invalid` / `missing`; later stages decide whether that is fatal (for example `asset_contract` fails on a GLB this index vouched for that is not on disk).

---

## Detailed stage deep dive

### Primary GLB selection (`pick_primary_glb`)

All `*.glb` under `assets/<id>/` (recursively) sorted by: name contains `animated` → `rigged` → `pbr` → `model` → alphabetical. So `model_animated.glb` beats `model_rigged.glb` / `model_uthana_rigged.glb`, which beat `model.glb`. Each stage writes beside its input rather than over it, so the order is what makes skeletons and clips reach the scene. `exclude_lods=True` (used by other stages, not here) skips `*_lod*` outputs of `asset_optimization`.

### Inspection (`_inspect_glb`)

Reads the 12-byte header (magic `glTF`, declared length ≤ file size) and the first chunk, which must be JSON; parses it with `json` (no tinygltf / pygltflib). Valid with materials → `ok`; valid without → `warn_no_material`; anything else → `invalid` with a `reason`.

### Animation data (`_animation_clip_names`, `_animation_extras`)

From `asset_animation.json` entries with `status: animated`:

- `animations` — semantic key → real clip name;
- `animation_speed_overrides` — `clip_speeds[key].engine_mps` when > 0 (the Uthana path), which `scene_compile` prefers over its height-derived estimate;
- `animation_provider` — `tripo` (default) or `uthana`.

### Bind sole (`_bind_sole`)

For a GLB with a skin, the lowest rest-pose vertex height through the skin (joint matrices × inverse bind, up to 20,000 sampled vertices) is recorded as `bind_sole_y`. Best effort: errors leave it out.

### Shared helpers

`read_glb_json`, `glb_rig_summary` (`num_skins`, `joint_count`, `animation_clips`), `pick_primary_glb` and `is_lod_output` are imported by other stages (notably `asset_optimization`) so every stage answers "is this rigged" the same way.

### Re-runs

No skip logic: every run re-inspects every manifest asset and overwrites `asset_normalization.json`.

### Settings

None of its own.

### Quest notes

Nothing Quest-specific; the texture and LOD work for Quest happens in `asset_optimization`.

## See also

- [`docs/rendering.md`](../../../rendering.md) — LODs and textures built from these GLBs
- [`docs/uthana.md`](../../../uthana.md) — Uthana clip speeds
- [`docs/sockets.md`](../../../sockets.md) — the next two stages
