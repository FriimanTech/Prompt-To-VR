> **Shared content:** Identical to [Quest_Stages/09b_uthana_motion_plan](../../Quest_Stages/09b_uthana_motion_plan/README.md). Update both copies when this stage changes.

# Stage 09b: Uthana Motion Plan

> **Pipeline position:** after `asset_animation` -> before `uthana_animation`  
> **Target:** shared (PC VR and Quest)  
> **Primary code:** [`apps/orchestrator/ptv_orchestrator/pipeline/stage_uthana_motion_plan.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/stage_uthana_motion_plan.py)  
> **Primary artifacts:** `uthana_motion_plan.json`

## Main stage summary

### What this stage is

Plans every Uthana motion before anything is generated.

### Flow

1. Measure each Uthana-rigged GLB (`pipeline/uthana/glb_measure.py`): `height_for_motion_m` is the world mesh height Uthana animates at; `engine_scale` is `target_scale_meters / raw longest axis`.
2. Query the live locomotion styles.
3. The motion-plan agent picks each gait's `style_id` (and any `<gait>.<variant>` such as `run.punching`).
4. Code enforces:
   - `strides` 1-5 (default 5) and `travel_angle` 0;
   - `move_speed` = 0.78 x height (walk) or 2.2 x height (run), within 5%, clamped to 0.25-8.0 and rounded to 0.1;
   - the canonical name `style-str{N}-ms{speed}-deg{angle}`.
5. Code appends the shared library idle `msf9nhQ4yyTJ` (name read from Uthana) to every character, and fails clearly if that motion id does not exist.

Agent: `uthana_motion_plan_model` @ `uthana_motion_plan_reasoning_effort` (default `medium`).

See [docs/uthana.md](../../../uthana.md) for the full Uthana flow.
