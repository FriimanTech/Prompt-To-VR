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

**Input map.** Left thumbstick X = strafe; -Y = forward; +Y = backward.
Right thumbstick X = snap turn (45° per push, 0.30 s cooldown). This
matches Meta's ["Locomotion input maps"](https://developers.meta.com/horizon/design/locomotion-input-maps/)
recommendations for left-handed locomotion.

**Circular dead zone.** We use a single magnitude dead zone of 0.15 (re-scaled
linearly afterwards). A square per-axis dead zone causes diagonal-near-rest
strafing, which Meta's design notes call out as a comfort issue.

**Comfort speed ramp.** The slide speed lerps toward
`stick_magnitude * 1.5 m/s` (Meta's recommended walking pace) using
exponential smoothing with τ ≈ 0.12 s on accel and 0.18 s on decel. This
removes the "jolt" at engagement / release that triggers vection sickness in
sensitive users.

**Per-frame direction update.** Both sliding modes recompute their forward
basis every frame:

* Head-Relative reads the HMD pose during the OpenXR render callback into
  `last_head_forward_xz_` (XZ component, normalised). Turning the head
  while moving smoothly steers the slide.
* Controller-Relative rebuilds the world-space rotation of the left grip
  every frame from the synced action set, projects -Z onto XZ, and uses
  that as forward. Head yaw is irrelevant.

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
Mechanic bindings such as `builtin.locomotion.slide.head_relative` on the
`player_rig` entity are **catalog-aligned stubs** in
`BuiltinMechanics.cpp` so the scene graph and planner stay consistent; they
do not re-implement movement (that would double-apply input).

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
