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

Each locomotion style is a real `IMechanic` in its own module:

| Mechanic | Module |
| -------- | ------ |
| `builtin.locomotion.teleport` (alias `builtin.teleport`) | `engine/runtime/src/mechanics/builtin/teleport/` |
| `builtin.locomotion.slide.head_relative` | `engine/runtime/src/mechanics/builtin/slide/` |
| `builtin.locomotion.slide.controller_relative` | `engine/runtime/src/mechanics/builtin/slide/` |
| `builtin.locomotion.snap_turn` (alias `builtin.snap_turn`) | `engine/runtime/src/mechanics/builtin/snap_turn/` |

The shared thumbstick dead zone lives in
`engine/runtime/src/mechanics/builtin/StickFilter.h` and the slide comfort
ramp in `builtin/slide/SlideComfort.h`. Only the startup **menu** (its panel
geometry and hit testing) is still in `Application.cpp`; it drives the
mechanics through `ILocomotion::set_active_mode()`.

All of them run in `MechanicPhase::Input` with `MechanicScope::Player`, and
move the player exclusively through `ILocomotion` (see
[`ptv/api/MechanicContext.h`](../engine/runtime/include/ptv/api/MechanicContext.h)),
never by touching the rig directly. Because each one early-outs unless
`ILocomotion::active_mode()` matches the style it implements, a scene can bind
all three and the player can switch at runtime without them fighting.

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
basis every frame from the LATEST tracked pose in the frame's
`InputSnapshot` (`head_rotation` for head-relative, the left hand's
`aim_rotation` for controller-relative). There is intentionally no fixed
world-space forward, no spawn-yaw fallback, and no fixed-direction fallback
ANYWHERE on the slide path. Head-relative keeps a one-entry cache of the last
good forward, and that cache starts as the **zero vector**, so until the
runtime has produced a valid VIEW pose at least once no slide direction
exists and the player simply does not move. (A previous build defaulted the
cache to `(0, 0, -1)`, which caused the infamous "world-based movement" bug:
pushing forward in head-relative mode slid the rig along world `-Z`
regardless of which way the player was physically looking.)

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

Mechanic bindings such as `builtin.locomotion.slide.head_relative` on the
`systems_global` entity attach the real mechanic listed in the table above.

**The engine guarantees all four are attached.** The runtime menu lets the
player select any style at any time, so a scene that binds only some of them
would leave the others dead. `Application::ensure_player_locomotion_bindings`
therefore adds any missing locomotion mechanic to a `player_locomotion`
entity at scene load. Whatever the scene DID bind is left untouched, params
included, so scene-authored tuning always wins.

> **Engine-wide.** The starter room and every generated game share the
> same `ptv_runtime.lib`, so a locomotion fix in the built-in modules
> automatically applies to all projects -- but each project's existing
> `builds/cmake-current/.../*.exe` was built from the engine source as
> it was at build time. After updating engine source you need to
> re-run the **CMake Build** stage on any pre-existing project to pick
> up the new behaviour. The next slide log line in
> `projects/<id>/logs/...` will look like
> `Slide[head_rel] fwd=(+0.00,-1.00) stick=(+0.00,+0.92) speed=1.38m/s
> rig=(+0.00,-1.20)` and `fwd` changes within ~1s when you physically
> rotate. See `docs/troubleshooting.md` for the verification checklist.

Optional keys under the top-level `"player"` object are read by
`SceneLoader` and surfaced to mechanics as `ILocomotion::tunables()`. A
mechanic defaults to these and lets its own binding `params` override them:

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

## Teleport onto generated environments

Teleport now raycasts the arc against the Jolt physics world (via
`PhysicsWorld::raycast`) instead of only intersecting a flat `teleport_floor_y`
plane. The player lands on the actual generated ground / platforms / terrain at
its real height (the commit sets the rig Y to the hit point), and near-vertical
surfaces (walls) are rejected as landing spots. This makes agent-generated
multi-level environments (see [`world-generation.md`](world-generation.md))
traversable with the default comfort locomotion. Smooth (slide) locomotion is
still free-fly on XZ; a Jolt `CharacterVirtual` capsule for wall-blocked
sliding is a tracked follow-up.

## Reference frames: riding things that move

`PlayerRig` position and yaw are expressed in a **reference frame**, which is
world space until something parents it. Once parented, the parent's motion
carries the player along *without their local position drifting* — which is the
property that matters. Standing still on a moving platform means zero stick
input and still arriving somewhere else.

```cpp
ctx.locomotion->set_reference_frame(lift_entity, "");        // ride the lift
ctx.locomotion->set_reference_frame(npc, "head");            // ride a joint
ctx.locomotion->clear_reference_frame();                     // step off
```

This is deliberately one generic feature rather than several specific ones.
Vehicles, elevators, boats, lifts, moving platforms and a miniature player
riding inside an NPC's head are the same problem, so none needs engine code of
its own. Generated code must **never** move the player every frame to follow a
platform by hand: that fights locomotion and drifts.

Details that are load-bearing:

- **The anchor may name a joint or socket.** A passenger inside a head has to
  ride the *animated* skull; the entity's root transform sits at a walking
  character's feet and does not turn with the neck.
- **The frame carries position and heading only.** A parent that pitches (a
  boat on a wave) must not pitch the player's horizon — tilting the player's
  frame is the most reliable way to make someone sick in VR. The parent's roll
  and pitch stay visible in the world around them, which is where they belong.
- **Boarding and disembarking do not move the player.** The local offset is
  recomputed on attach and detach, so stepping onto a lift does not teleport
  you by a bookkeeping detail.
- **`ILocomotion` still speaks world space.** `position()` and `yaw_radians()`
  return world values and `teleport_to` takes one; the conversion happens at
  the frame boundary. A world-space slide delta is rotated into the frame
  before it is applied, or walking forward on a turning vehicle would curve.
- **A scene transition clears the frame.** The vehicle belonged to the scene
  being torn down, and entt reuses handles, so a stale parent would silently
  resolve to whatever entity took that slot and drag the player around.

## Player scale

`ctx.locomotion->set_player_scale(s)` resizes the player: 1.0 is life size,
0.02 is a two-centimetre passenger inside a skull. The **rig** scales, never
the world — the world is what physics, navigation and every authored dimension
agree on, and rescaling it would invalidate all of them at once.

Scale is innermost in the composition, so it shrinks the player's own body and
their tracked motion but not their offset within the frame: a passenger two
metres along a deck is two metres along it whatever size they are. A one-metre
physical step becomes two centimetres of world travel at 0.02.

Clip planes scale with the player (`0.05 * scale` near, `1000 * scale` far). A
two-centimetre passenger with a fixed 5 cm near plane cannot see their own
hands or the floor they are standing on — both are inside it. The collision
capsule scales too, or a tiny player keeps colliding with a human-sized shape.
Scale is clamped to `[0.01, 100]`: below a centimetre the near plane collapses
toward zero and wrecks depth precision, and above 100 a giant's near plane
clips through everything within reach.
