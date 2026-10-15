> **Shared content:** Identical to [PC_VR_Stages/09c_uthana_animation](../../PC_VR_Stages/09c_uthana_animation/README.md). Update both copies when this stage changes.

# Stage 09c: Uthana Animation (check, generate, rename, download, merge)

> **Pipeline position:** after `uthana_motion_plan` -> before `asset_normalization`  
> **Target:** shared (PC VR and Quest)  
> **Primary code:** [`apps/orchestrator/ptv_orchestrator/pipeline/stage_uthana_animation.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/stage_uthana_animation.py)  
> **Primary artifacts:** `assets/<id>/uthana_motions/*.glb`, `assets/<id>/model_animated.glb`, `uthana_animation.json`, `asset_animation.json`

## Main stage summary

### What this stage is

Turns every planned motion into a downloaded clip, then merges a character's clips with its rig into one animated GLB.

### Per motion (repeats until every motion of the character is on disk)

- **Locomotion**: List your motions `[:500]` -> Motion Verification by exact name -> reuse on a match; otherwise parse the name into Generate Locomotion parameters, create the motion, then rename it to the canonical name.
- **Library** (the shared idle): use the fixed `motion_id`.
- **Download**: `output_format="glb", no_mesh=True, in_place=True`, with THIS character's `character_id`.

Resolvers are pluggable (`pipeline/uthana/motion_library.py`): a new motion source adds a resolver and nothing else here changes.

### Merge (only when every motion is present)

`pipeline/uthana/glb_merge.py`:

- copies samplers into the rigged GLB and re-points channels by node name;
- keeps motion names as clip names;
- adds one per-character Hips offset (in the Hips parent's local axes) so the origin is at the character's middle, like Tripo;
- validates that toes land at the bind sole;
- crossfades a loop seam above 8 degrees.

### Outputs

- `model_animated.glb`.
- An `asset_animation.json` entry (`provider: uthana`, key -> clip map, `clip_speeds` with engine-space m/s).
- `uthana_animation.json` ledger (created / reused / library per motion). A Resume never regenerates or re-downloads.
- `uthana_max_generated_seconds_per_run` caps new generation.

See [docs/uthana.md](../../../uthana.md) for the full Uthana flow.
