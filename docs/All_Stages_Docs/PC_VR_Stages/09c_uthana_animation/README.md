# Stage 9c: Uthana Animation

> **Pipeline position:** after `uthana_motion_plan` → before `asset_normalization`  
> **Target:** shared (PC VR and Quest)  
> **Primary code:** [`apps/orchestrator/ptv_orchestrator/pipeline/stage_uthana_animation.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/stage_uthana_animation.py), [`pipeline/uthana/motion_library.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/uthana/motion_library.py), [`pipeline/uthana/glb_merge.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/uthana/glb_merge.py)  
> **Primary artifacts:** `assets/<id>/uthana_motions/*.glb`, `assets/<id>/model_animated.glb`, `uthana_animation.json`, `asset_animation.json`

## Main stage summary

### What this stage is

The **Uthana animation stage** turns every motion in `uthana_motion_plan.json` into a clip on disk — reusing a motion that already exists in the Uthana organisation, or generating and renaming a new one — downloads each clip retargeted onto the character, and, only when **every** motion of a character is present, merges them with the rigged GLB into one `model_animated.glb`.

### Why it exists

The runtime loads one GLB per character with mesh, skin and clips. Uthana returns each motion separately, with the feet at the origin, so the clips have to be merged and offset to the engine's convention (origin at the character's middle, as for Tripo characters). The ledger and name-based reuse mean a motion is never generated or downloaded twice.

### Inputs (what enters)

| Input | Location | Used for |
|-------|----------|----------|
| Motion plan | `uthana_motion_plan.json` | Per character: `character_id`, motions (`key`, `kind`, `motion_name`, `motion_id`, speed, `gait`), `height_for_motion_m`, `engine_scale` |
| Rigged GLB | `assets/<id>/model_uthana_rigged.glb` | Merge base |
| Ledger | `uthana_animation.json` | Resume |
| Animation report | `asset_animation.json` | Existing Tripo entries are kept; Uthana entries are added |
| Uthana API | live | List / verify / generate / rename / download motions |

### Outputs (what it produces)

| Output | Location | Contents |
|--------|----------|----------|
| Motion clips | `assets/<id>/uthana_motions/<motion_name>.glb` (+ `.root.glb` for gaits) | In-place clip; root-motion copy used only to measure speed |
| Animated GLB | `assets/<id>/model_animated.glb` | Rig + all clips, Hips offset applied |
| Ledger | `uthana_animation.json` | Per character `status`, `character_id`, per-motion `motion_id`, `source` (`created` / `reused` / `library`), file, `sha256`, speeds, merge report |
| Animation report | `asset_animation.json` | `provider: "uthana"` entry in the Tripo path's shape plus `clip_speeds` |

### Where output goes next

- **`asset_normalization`** picks `model_animated.glb`, copies `animations` and turns `clip_speeds[*].engine_mps` into `animation_speed_overrides` for `scene_compile`.
- **`asset_contract`** measures the merged character's model frame (Uthana rigs are hips-centred and face +Z).
- **`scene_compile`** writes `mesh.animations` and prefers the recorded engine speeds over its height-based estimate.

### Models / providers

Uthana only (SDK-backed `UthanaClient`), no LLM. Every download is `glb`, `no_mesh=True`, `in_place=True`, retargeted onto **this** character's `character_id`.

### Cost

Only newly generated locomotion seconds cost money. Reused motions, the library idle, downloads, renames and the root-motion speed downloads are not counted. `uthana_max_generated_seconds_per_run` (default 600 s) caps new generation; the estimate is `strides × 1.2 s` per motion.

### Failure behavior

The stage always **succeeds**; problems are per character:

- A motion that cannot be resolved or downloaded (API error, download quota, cost cap) → warning, the character is recorded `incomplete` with its `missing` keys and is **not merged** (it stays rigged-only).
- Merge error → `merge_failed` in the ledger, `status: failed` in `asset_animation.json`.
- Merge validation problems (feet off the bind sole) are warnings with `validation_ok: false`.

---

## Detailed stage deep dive

### Per-motion loop

For each character, for each `UthanaMotionSpec`:

1. **Resume check** — if the ledger entry has the same `motion_name` and `character_id`, the file exists and its `sha256` matches, it is reused (a missing root-motion speed is back-filled).
2. **Resolve** via the pluggable resolver for `spec.kind` (`build_resolvers`):
   - **`uthana_locomotion`** (`LocomotionResolver`): on first use per run, *List your motions* (`list_motions`, trimmed to `uthana_motion_list_limit`, with a warning if the organisation has more). **Motion Verification** = exact name match → `reused`. Otherwise it looks for an orphan with Uthana's default name for the same parameters (`<style>-<speed>-<N>steps-<angle>deg`, a generated-but-never-renamed motion) and renames it for free → `reused`. Otherwise checks the cost cap, calls **Generate Locomotion** (`create_locomotion(character_id, strides, move_speed, style_id, travel_angle)`), then **Rename** to the canonical name only after the create returned its id, and re-reads the motion to confirm the name → `created`.
   - **`uthana_library`** (`LibraryResolver`): the fixed `motion_id` (the shared idle) → `library`.
3. **Download** (`download_motion`): checks `is_download_allowed` (quota), downloads `no_mesh=True, in_place=True`, rejects anything that is not a GLB, writes atomically to `uthana_motions/<slug>.glb`.
4. **Root-motion speed** (gaits only): the same motion is downloaded again with `in_place=False` to `<name>.root.glb` and `measure_root_speed` reads its real average travel speed. Failure here just leaves the value empty.
5. The ledger is saved after every motion.

New motion sources only need a new `MotionSource` value and a resolver; download and merge only see the resolved `motion_id`.

### Merge (`glb_merge.merge_character`)

Runs only when no motion is missing:

- Base must have a skin and unique node names (channels are re-pointed **by name**); channels whose node is not a joint of the base skin are skipped and listed.
- Each motion's samplers are copied into the base buffer; clip names stay the **motion names** (intent → clip is recorded separately).
- **Hips offset**: one constant per character, `k = bind_lowest_toe_y − median(per-clip lowest toe height)`, added to the Hips translation keys of every clip and converted into the Hips parent's local frame (Uthana's `Armature` is scaled 0.01 and rotated 90° about X). If no toe/foot joints exist, no offset is applied (warning).
- Loops with a seam above 8° get a 0.25 s crossfade toward frame 0.
- Validation: each clip's lowest toe must be within 3 cm of the bind toe (else `ok = false`), and the bind sole should sit within 5 cm of −h/2 (warning).

Also a CLI: `python -m ptv_orchestrator.pipeline.uthana.glb_merge base.glb out.glb walk=<file>.glb idle=<file>.glb`.

### `asset_animation.json` entry

`status: animated`, `provider: uthana`, `character_id`, `animations` (key → motion name), `clip_names`, `hips_offset_m`, `validation_ok`, `warnings`, `animated_glb`, `animate_in_place: true`, and `clip_speeds` per gait key: `requested_mps`, `root_motion_mps`, `measured_mps` (in-place foot-slip estimate), `used`, `source_height_m`, `engine_scale`, `engine_mps`. The speed used prefers root motion, then foot-slip, then the requested speed — a reading outside 0.5–1.5× the request is ignored — and `engine_mps = speed × engine_scale`.

Both the ledger and the report are pruned to current manifest ids.

### Settings

| Setting | Env var | Default |
|---------|---------|---------|
| `uthana_api_key` | `UTHANA_API_KEY` | — |
| `uthana_motion_list_limit` | `UTHANA_MOTION_LIST_LIMIT` | `500` |
| `uthana_max_generated_seconds_per_run` | `UTHANA_MAX_GENERATED_SECONDS_PER_RUN` | `600.0` |

### Quest notes

Nothing Quest-specific. The merged GLB goes through the same skin-preserving LOD path in `asset_optimization`.

## See also

- [`docs/uthana.md`](../../../uthana.md) — the full Uthana flow
- [`docs/npc-and-ai.md`](../../../npc-and-ai.md) — locomotion clips and speeds
- [`docs/rendering.md`](../../../rendering.md) — skinned LODs
