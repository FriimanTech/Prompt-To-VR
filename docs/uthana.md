# Uthana: humanoid rigging and locomotion

Tripo creates every mesh. After that, humanoid characters can be rigged and
animated by **Uthana** instead of Tripo. Uthana's auto-rigger adds finger
joints, so the player's avatar hands curl with the controllers. Its locomotion
generator makes a gait in any of ~110 styles (zombie, crouching, limp, robot,
strutting, punching while moving, ...) at a chosen speed.

## Where it sits in the pipeline

```
tripo_models -> character_routing -> uthana_rigging -> asset_rigging (Tripo)
             -> asset_animation (Tripo) -> uthana_motion_plan -> uthana_animation
             -> asset_normalization -> ...
```

**`character_routing`** (`character_routing.json`, `tripo_rig_check.json`)
decides, per character, which provider rigs and animates it:

- The player's avatar body always goes to Uthana (Full Player Avatar games).
- A biped humanoid that Tripo's free rig-check cannot rig goes to Uthana.
  This is decided in code.
- A non-humanoid creature (quadruped, hexapod, octopod, avian, serpentine,
  aquatic) goes to Tripo.
- A biped humanoid that both providers can rig is decided by the routing
  agent. It compares what the NPC needs against Tripo's 101 v1.0 presets
  and Uthana's live style list, and picks the provider covering the most.

"Biped humanoid" means upright with two legs and two arms, human or not: a
fish-man, an alien, a robot and a transforming car all count. The agent sees
the reference image and the exact prompt Gemini drew it from.

**`uthana_rigging`** (`uthana_rig.json`) uploads the un-rigged `model.glb` with
`auto_rig`, `auto_rig_front_facing`, `include_fingers`,
`auto_rig_strip_namespace` and `auto_rig_feet_on_floor`. It downloads the result
to `assets/<id>/model_uthana_rigged.glb` and records the `character_id` and
`auto_rig_confidence`.

- **Low confidence:** at or below `uthana_avatar_min_confidence` (0.20), or on
  a failed upload or validation, the avatar falls back to Tripo (rig plus
  walk/run, no finger curls) with a loud warning. An NPC falls back to Tripo
  when Tripo can rig it, and to no rig otherwise.
- **Upload dedupe:** Uthana returns the FIRST `character_id` when the same file
  is uploaded twice, with that upload's flags. So the stage never assumes
  `feet_on_floor` took effect. It measures the rest pose and centres the
  origin itself.
- **Superseded Tripo output:** a character that Tripo rigged in an earlier run
  has those files moved to `assets/<id>/.superseded/`, so two rigs are never
  mixed.

**`uthana_motion_plan`** (`uthana_motion_plan.json`) generates nothing. It
measures each rigged GLB's world height, queries the live locomotion styles,
and asks the motion-plan agent for each gait's `style_id`, `strides` (5),
`travel_angle` (0) and `move_speed`. Code then checks and corrects the plan:

- **Speed:** walk is `0.78 x height` and run is `2.2 x height`, the engine's
  own factors. Speeds are clamped to 0.25-8.0 m/s and rounded to 0.1.
- **Name:** each motion gets its canonical name.
- **Idle:** every character also gets the shared library idle.

**`uthana_animation`** (`uthana_animation.json` ledger) works through each
character one motion at a time. Once every motion is on disk it merges them.
Details follow below.

## Motion names

Locomotion motions are named `style-str{N}-ms{speed}-deg{angle}`, for example
`zombie-str5-ms1.4-deg0`. The name is the motion's identity across your whole
Uthana organisation. Before anything is generated:

1. **List your motions:** the organisation's motions, `[:500]`
   (`uthana_motion_list_limit`).
2. **Motion Verification:** an exact-name search. It prints
   `match found: <name> <id>` or
   `no match is found in the Uthana Organization for that motion requested`.
3. **On a match:** the existing motion is reused. Downloads are free, and
   Uthana retargets on download.
4. **Otherwise:** the name is parsed back into `style_id` / `strides` /
   `move_speed` / `travel_angle`, Generate Locomotion creates the motion, and
   only after it returns is the new motion renamed to the canonical name.

Standalone versions of each step live in `Sequential_Runs/Uthana_R_A/`:
`list_motions_500.py`, `motion_verification.py` and `measure_rigged_height.py`.

## The shared idle

Uthana's locomotion API has no standing-still clip. Every Uthana-animated
character, NPC or player avatar, gets the same existing organisation motion,
**`msf9nhQ4yyTJ`**. It is downloaded with that character's own `character_id`,
so it is retargeted to the character's skeleton, and it is never generated.

Replace it by setting `UTHANA_IDLE_MOTION_ID` in `.env`. The motion-plan stage
fails with a clear message if the id does not exist.

## Motion sources (how to add more)

Motions come from pluggable **sources** (`MotionSource` in
`ptv_schemas/character_routing.py`). Each source has one resolver in
`pipeline/uthana/motion_library.py` with the contract
`resolve(spec, character_id) -> motion_id`. Download and merge only ever see
the `motion_id`.

Today there are two sources:

- **`uthana_locomotion`:** check, generate, rename.
- **`uthana_library`:** a fixed existing id, such as the shared idle.

A future source (for example, actions described in words: an attack, a
roundhouse kick) needs four additions:

- a new `MotionSource` value, plus the fields it needs on the motion specs;
- a resolver, plus its naming convention so Motion Verification can reuse it;
- prompt sections telling the routing and motion-plan agents what it covers;
- an animation key convention.

Download, merge and the engine do not change. Until then, one-shot actions on
a Uthana character are recorded as `unmet` and use the NPC's
`animation_fallback`.

## Download and merge

Every download uses `output_format="glb", no_mesh=True, in_place=True`. Files
land in `assets/<id>/uthana_motions/`. Only when every motion of a character is
present does the merge run (`pipeline/uthana/glb_merge.py`):

- **Copy:** samplers and their keyframe bytes are copied into the rigged GLB.
- **Re-point:** each channel is re-pointed to the base node with the same name.
- **Names:** clips keep their Uthana motion names; the key-to-clip map goes to
  `asset_animation.json`.
- **Origin contract:** Uthana clips put the feet at the origin, while the
  engine (and every Tripo character) puts the origin at the character's
  middle. One constant per character is added to the Hips translation keys
  of every clip:

  `k = bind_lowest_toe_y - median(per-clip lowest toe height)`

  It is converted into the Hips parent's local frame, because a Uthana
  `Armature` is scaled by 0.01 and rotated 90 degrees about X. Validation:
  each clip's lowest toe lands within 3 cm of the bind pose's (about `-h/2`).
- **Loops:** a loop whose seam exceeds 8 degrees gets a 0.25 s tail-to-head
  crossfade.
- **Output:** the result is `model_animated.glb`. Its `asset_animation.json`
  entry carries `provider: "uthana"`, the `character_id` and per-clip
  `clip_speeds`. Those speeds are requested or measured from the in-place
  clip's foot slip, then scaled to the asset's in-scene size; scene_compile
  writes them as `animation_speeds`.

## Engine support

- **Ancestor bake:** the skinned loader bakes a root joint's non-joint
  ancestors (the `Armature`) into its animation keys, exactly as it folds them
  into the bind pose. This fixes the runtime and every decimated LOD.
- **Gait variants:** `CLocomotionIntent::gait_variant` (for example
  `"punching"`) plays `run.punching` at the run speed the behaviour asked
  for. Travel speed and clip choice are independent.
- **No idle clip:** a standing character with no `idle` fades to the rest pose
  instead of walking in place.
- **One-shots:** a non-looping clip (attack, hurt, die) is not replaced by the
  gait clip until it finishes.
- **Foot offset:** `mesh.foot_offset_m` (origin-to-sole distance) plants
  grounded characters exactly on the floor.
- **Centimetre root scale:** an Uthana rig scales its root joint (Hips) by
  0.01. Joint frames used for placement (`ISkeleton::joint_world`, attachment
  anchors in `tick_attachments`) are scale-free, so an attachment offset of
  5 cm stays 5 cm instead of shrinking to 0.5 mm.
- **Player avatar walk/run:** the avatar's measured walk and run clip speeds
  become the slide's walk and sprint speeds, and the run clip plays only
  while the player holds the left stick in. See
  [`locomotion.md`](locomotion.md#walking-and-running-sprint).
- **Neck follows the clip:** `builtin.avatar_body` shifts the body by the
  clip's smoothed horizontal neck offset (up to 15 cm). An idle that settles
  the neck forward, or a run that leans it, no longer leaves the camera
  behind the avatar's shoulders.
- **The body hangs from the neck, not the eyes:** the eyes sit in front of
  and above the neck, so turning or tilting the head swings the headset
  through an arc. The body is placed under the neck pivot (the headset minus
  the eye-from-neck offset, turned with the head), so looking around leaves
  it still. The legs read their speed from that pivot. They start walking
  above 0.25 m/s and stop below 0.12 m/s, and snap-turn and teleport frames
  are skipped, so leaning and looking never start a walk cycle.
- **Arms never blink out:**
  - The shoulder (clavicle) reach assist is rebuilt from the bind pose every
    frame (`two_bone_ik::shoulder_assist`) and capped at 25 degrees.
    Stacked on its own previous result, it wound the shoulder round and
    snapped it back, so the arm whipped out of view for a few frames at a
    time.
  - A controller that loses tracking is held for 0.3 s, then eased back to
    the clip over 0.25 s.
  - Non-finite IK results are never written.
  - The player's own body (`CSelfViewHide` or the `player_avatar` tag) is
    never frustum- or distance-culled and always draws LOD0.
- **Height calibration** (`avatar_body/HeightCalibration.h`):
  - It waits for a still, standing head before sizing the avatar. An eye
    height under 1.25 m isn't treated as standing; after 10 s it uses what
    it has.
  - It grows the avatar when you stand up taller (held for 1.5 s), and a
    crouch never shrinks it.
  - logcat: `builtin.avatar_body: eye height 1.62 m -> avatar scale 1.000
    (standing)`.
- **Arm diagnostics:** once a second,
  `builtin.avatar_body arms L reach=.. assist=..deg held=.. | R ...`. Any arm
  joint that jumps more than 25 cm in one frame, measured in the body's own
  frame, is logged by name.

## Costs

Uthana is pay-as-you-go, billed per generated second; downloads are
unlimited. Reused motions and the library idle cost nothing.
`uthana_max_generated_seconds_per_run` caps new generation per run.

## Settings

- `UTHANA_API_KEY`: loaded from `.env` with python-dotenv.
- `uthana_enabled`
- `uthana_avatar_min_confidence`, `uthana_npc_min_confidence`
- `uthana_locomotion_strides`
- `uthana_motion_list_limit`
- `uthana_idle_motion_id`
- `uthana_max_generated_seconds_per_run`
- `character_routing_*`, `uthana_motion_plan_*`: model, reasoning effort,
  verbosity, turns and retries for the two agents.

## Troubleshooting

- **Floating or sinking character:** check the merge validation in
  `uthana_animation.json` (`merge.warnings`) and `foot_offset_m` in
  scene.json.
- **Hands do not curl:** check `uthana_rig.json`. `finger_joints` must be
  non-empty; a fallback to Tripo has no fingers.
- **The player accelerates far past the walk speed, or the body runs ahead
  of the camera:** something with a collider is following the player. The
  body family (worn, attached and held items) is ignored automatically;
  look for `player motor: body was pushed` in logcat, which names the
  entity.
- **"Maximum number of characters reached":** delete unused characters in
  your Uthana organisation.
- **A motion was regenerated that you expected to reuse:** names must match
  exactly, so compare the planned name with `list_motions_500.py`.
