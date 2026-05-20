# Locomotion (Meta-aligned terminology)

The PTV runtime ships with three first-class locomotion modes. Naming and
behaviour follow Meta's Horizon design guides so that prompts, menus, and
generated code stay consistent across games.

| Mode (canonical id)                              | Type     | Direction comes from                            | Confirmed by Meta term     |
| ------------------------------------------------ | -------- | ----------------------------------------------- | -------------------------- |
| `builtin.locomotion.teleport`                    | Discrete | Aim arc from left controller; commit on release | Teleportation              |
| `builtin.locomotion.slide.head_relative`         | Sliding  | HMD forward projected onto the floor (XZ)       | Sliding, head-relative     |
| `builtin.locomotion.slide.controller_relative`   | Sliding  | Left-controller -Z aim projected onto the floor | Sliding, controller-relative |

The starter room, and any generated game that opts into the
`settings.locomotion.menu` flag in its BuildSpec, displays a one-time picker
with the title **Pick your Movement Style** (Teleport / Sliding). Choosing
"Sliding" flips the panel to **Pick your Sliding Style**
(Head-Relative / Controller-Relative).

## Implementation notes

Source: `engine/runtime/src/core/Application.cpp`.

**Input map.** Left thumbstick X = strafe; **+Y = forward; -Y = backward**.
Right thumbstick X = snap turn (45° per push, 0.30 s cooldown). This
matches the OpenXR 1.1 spec ("Standard input identifiers": *"+1 means all
the way right for x axis or all the way up for y axis"*) and Meta's
["Locomotion input maps"](https://developers.meta.com/horizon/design/locomotion-input-maps/)
recommendations for left-handed locomotion. (A previous build used `-Y`
for forward, which caused the "push forward, move backward" report —
the runtime now uses `forward_axis = +ly`.)

**Circular dead zone + cardinal snap.** We apply a magnitude dead zone of
0.18 (re-scaled linearly afterwards), and immediately afterwards a
*cardinal snap*: when one axis dominates (`>0.5`) and the other is
small (`<0.18`), the small axis is forced to zero. This absorbs the
cross-axis bleed that Quest 3 Touch Plus thumbsticks accumulate after a
few months of use. Without the snap, "push pure right" reads as e.g.
`(+1.0, +0.10)` and the player drifts diagonally; with the snap, the
small `+0.10` is zeroed and motion is purely right. Intentional
diagonals (where both axes are large) are preserved unchanged.

**Comfort speed ramp.** The slide speed lerps toward
`stick_magnitude * 1.5 m/s` (Meta's recommended walking pace) using
exponential smoothing with τ ≈ 0.12 s on accel and 0.18 s on decel. This
removes the "jolt" at engagement / release that triggers vection sickness in
sensitive users.

**Per-frame direction update.** Both sliding modes recompute their forward
basis every frame from the LATEST tracked pose, BEFORE locomotion math
runs. There is intentionally no fixed world-space forward, no spawn-yaw
fallback, and no fixed-direction fallback ANYWHERE on the slide path.
The cached `last_head_forward_xz_` member defaults to the **zero
vector** so that until the runtime has produced a valid VIEW pose at
least once, no slide direction exists and the player simply does not
move. (A previous build defaulted this cache to `(0, 0, -1)`, which
caused the infamous "world-based movement" bug: pushing forward in
head-relative mode slid the rig along world `-Z` regardless of which
way the player was physically looking.)

**Frame timing.** `XrRuntime::begin_xr_frame()` is called at the top of
the application loop *before* `sync_input`. It performs `xrWaitFrame`
+ `xrBeginFrame`, stores the predicted display time, and returns
whether the runtime wants the frame rendered. As a direct consequence,
`sync_input` locates the head and hand spaces against the stage at the
*current* frame's predicted display time — not the previous frame's,
which is what happened when the wait/begin pair lived inside
`render_frame`. This is the difference between "head-relative slide
re-steers within the same frame you turn" and "head-relative slide
ignores rotations until the next render tick".

* Head-Relative reads the HMD pose from `XrRuntime::InputState::head` —
  populated each frame by locating
  [`XR_REFERENCE_SPACE_TYPE_VIEW`](https://registry.khronos.org/OpenXR/specs/1.1/html/xrspec.html#reference-spaces)
  against the play-space stage at the predicted display time, in the
  same coordinate system as the hands. We project the resulting `-Z`
  onto the XZ ground plane (vertical pitch is discarded so looking
  up/down does not move the player). Turning the head 180° while
  holding the stick re-steers the slide on the very next frame.
* Controller-Relative reads the freshly synced left-controller **`/aim/pose`**
  (NOT `/grip/pose`), composes it through the rig yaw, projects `-Z`
  onto XZ, and uses that as forward. Head yaw plays no role; the user
  can look anywhere while gliding along the controller's aim. Pointing
  the controller up/down does not produce vertical movement.

  We use `/aim/pose` instead of `/grip/pose` because Quest Touch
  controllers have an angled grip handle: the `/grip` pose's `-Z` is
  along the user's hand (~30° above horizontal when the controller is
  held flat), while the `/aim` pose's `-Z` is along the controller
  body — i.e. where the user intuitively expects to "point". Using
  `/grip` here was what made an earlier build feel like the controller
  was tilted up: pushing the stick "forward" along the angled hand
  axis included an upward component, which (combined with the
  forward-sign bug) read as `move backward`. The fix binds a separate
  `aim_pose` action in `XrSession::create_action_set` and exposes it
  via `InputState::aim_left/right`. Teleport arcs and the locomotion
  menu pointer also switched to `/aim`, for the same reason.

**Right-handed strafe basis.** Right-vector for the slide basis is
`right = cross(fwd, up)` (Y-up, RH convention). The previous code used
`cross(up, fwd)`, which is the negation, and silently swapped the
left/right strafe direction once the slide bug was fixed.

**Reference-space hygiene.** Hand poses (grip pose action space), the HMD
pose (VIEW reference space), and the player rig translation are all
expressed in / composed into the play-space stage (`STAGE`, with `LOCAL`
fallback). The stick input is converted into world-space velocity using
this single coordinate system and applied to the rig root — not the HMD
or eye matrices. The OpenXR runtime keeps driving the head/eye poses
exclusively; gameplay only ever moves the rig.

**No-basis warning.** When the user pushes the stick (`magnitude > 0.05`)
but the slide path declines to move (no fresh head pose, or no fresh
left-controller pose), the runtime emits one
`PTV_WARN("Slide[...] no basis ...")` line per second so that "the
player isn't moving" is always traceable in the logs to a specific
missing tracked pose rather than appearing as silent inaction.

**Teleport.** Push the left stick past 0.5 to draw a cyan parabolic arc
launched from the controller (`launch_speed = 6 m/s`, `g = 9.8 m/s²`,
horizontal cap = 12 m). When the arc clears the floor a translucent
landing disc appears. Releasing the stick (magnitude < 0.1) commits.

## Source documents

* [Locomotion Types](https://developers.meta.com/horizon/design/locomotion-types/)
* [Artificial locomotion (sliding, teleport, dash)](https://developers.meta.com/horizon/design/locomotion-types/#artificial-locomotion-types)
* [Locomotion input maps](https://developers.meta.com/horizon/design/locomotion-input-maps/)

## Catalog IDs and the planner

`apps/orchestrator/ptv_orchestrator/engine_catalog.py` exposes these three
IDs to the mechanic-planner agent. When a prompt is ambiguous about
locomotion (e.g. "Superman Flight Sim, the user can walk in the city"),
the planner is instructed to add **both** sliding modes plus the menu
binding so the player can choose at runtime, exactly the way the starter
room does. See [`docs/engine-growth.md`](engine-growth.md) for the
catalog-and-promotion lifecycle.

## Scene tuning (`scene.json` → runtime)

The **actual** thumbstick locomotion (teleport arc, sliding, snap turn,
startup menu) is implemented in `engine/runtime/src/core/Application.cpp`.
Mechanic bindings such as `builtin.locomotion.slide.head_relative` and
`builtin.locomotion.slide.controller_relative` on the `systems_global`
entity are **catalog-aligned stubs** in
`engine/runtime/src/mechanics/BuiltinMechanics.cpp` -- their `tick`
methods are deliberately empty. They exist only so a scene's mechanic
binding list can name them without `MechanicHost` logging
"no mechanic registered". They do **not** re-implement movement (that
would double-apply input).

> **Engine-wide.** The starter room and every generated game share the
> same `ptv_runtime.lib`, so a fix to locomotion in `Application.cpp`
> automatically applies to all projects -- but each project's existing
> `builds/cmake-current/.../*.exe` was built from the engine source as
> it was at build time. After updating engine source you need to
> re-run the **CMake Build** stage on any pre-existing project to pick
> up the new behaviour. The next slide log line in
> `projects/<id>/logs/...` will look like
> `Slide[head_rel] fwd=(+0.00,-1.00) ... head_fresh=yes` and `fwd`
> changes within ~1s when you physically rotate. See
> `docs/troubleshooting.md` for the verification checklist.

Optional keys under the top-level `"player"` object are read by
`SceneLoader` and drive Application behaviour:

| JSON field | Default | Effect |
| ---------- | ------- | ------ |
| `locomotion_menu` | `true` | When `false`, the startup picker is skipped and `default_locomotion_mode` (or BuildSpec-style `locomotion` string) selects the initial mode. |
| `default_locomotion_mode` | `""` | When the menu is off: substring match on `controller` / `head` / `teleport`, else fall back from `locomotion` (`mixed` → head-relative sliding). |
| `slide_max_speed_mps` | `1.5` | Comfort cap for sliding (Meta walking pace). |
| `slide_accel_tau_s` | `0.12` | Acceleration smoothing time constant. |
| `slide_decel_tau_s` | `0.18` | Deceleration smoothing time constant. |
| `teleport_launch_speed_mps` | `6.0` | Parabolic arc launch speed. |
| `teleport_max_horizontal_m` | `12.0` | Arc range cap. |
| `teleport_floor_y` | `0.0` | Floor plane height for arc intersection tests. |
| `snap_turn_degrees` | `45` | Right-thumbstick snap increment. |
| `snap_turn_cooldown_s` | `0.30` | Minimum time between snaps. |

## Visual quality expectations (vs AAA titles)

Shipping titles such as *Half-Life: Alyx* rely on years of art direction,
full PBR materials, shadows, post-processing, and bespoke animation — far
beyond what the current PTV runtime draws.

What we have today (engine grew up one step):

- `GltfMeshLoad` merges **geometry + normals + per-vertex baseColor**.
  Tripo outputs **textured PBR** GLBs; we don't sample the
  `baseColorTexture` yet, but we DO bake each primitive's
  `pbrMetallicRoughness.baseColorFactor` (multiplied by the optional
  `COLOR_0` vertex stream) into the per-vertex color attribute. So a
  green lantern object renders green, a star skybox renders dark blue,
  etc. — without the renderer needing samplers or descriptor sets.
- `tinygltf` is built with `TINYGLTF_NO_STB_IMAGE`. We register a no-op
  `SetImageLoader` callback so loading never fails on embedded images
  (the pre-fix error was: `No LoadImageData callback specified.`).
- The fragment shader runs a 3-point light rig (key + fill + sky bounce)
  with half-Lambert wrap, rim term, and exponential fog on top of the
  per-vertex color. Material 0 (default), 1 (floor grid), and 2 (hand
  highlight) are kept distinct.

Next-step engine work (tracked separately):

1. Sample `baseColorTexture` instead of just `baseColorFactor`. Adds a
   sampler / descriptor set per material; biggest single quality win.
2. Add metallic/roughness + normal maps for proper PBR.
3. Cascade shadow maps + tonemapping pass.

Locomotion and **hiding invisible trigger/template entities** remove the
biggest "prototype on top of my game" artefacts; per-vertex baseColor
is the bridge between flat-tinted cubes and full PBR sampling.
