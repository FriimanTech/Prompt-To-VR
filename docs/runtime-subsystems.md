# Runtime subsystems

This doc describes how the hand-authored engine core is laid out.

## Public headers

All engine-public headers live under `engine/runtime/include/ptv/`. Generated
code is restricted to these prefixes (enforced by
[`engine/tools/check_generated_bounds.py`](../engine/tools/check_generated_bounds.py)
and the orchestrator's
[`bounds.py`](../apps/orchestrator/ptv_orchestrator/pipeline/bounds.py)):

- `ptv/api/` — `IMechanic`, `IBehaviour`, `IDialogueHook` and the register
  macros, plus `InputSnapshot` (live controller/headset input) and
  `MechanicContext` (locomotion / physics / debug-draw / hand / transform-fx /
  haptics / socket / skeleton services).
- `ptv/ecs/` — `World`, core ECS components (`CTransform`, `CMesh`, `CPhysics`, …),
  plus `TransformOffset.h` (the draw-time visual-offset layer) and
  `Skeleton.h` (the per-frame pose tick).
- `ptv/scene/` — `SceneDoc`, `LoadSceneJson`, `ApplySceneToWorld`.
- `ptv/math/` — `Vec3`, `Quat`, `Transform` (glm wrappers).
- `ptv/mechanics/` — `MechanicRegistry`, `MechanicHost`.
- `ptv/dialogue/`, `ptv/ai/` — reserved for future dialogue / AI systems.

## Subsystems

### Platform (GLFW)

`ptv::Window` wraps `GLFWwindow` for the desktop *mirror* window. The mirror
is not a VR surface — OpenXR owns the HMD swapchain.

### Vulkan (`ptv::VulkanContext`)

Instance + physical device selection + logical device + graphics queue + VMA
allocator. Validation layers are auto-detected and enabled in Debug builds.

### OpenXR (`ptv::XrRuntime`)

Wraps `xrCreateInstance`, `xrGetSystem`, `xrCreateSession` (via
`XR_KHR_vulkan_enable2`), a stage reference space, one action set
(`gameplay`) and per-eye color swapchains. The frame loop (`poll_events` /
`begin_frame` / `end_frame_no_render` / `sync_input`) is public so the
application can drive the XR session without leaking OpenXR types upward.

The `gameplay` action set carries left/right hand + aim poses, trigger, grip,
thumbstick, A/B/X/Y buttons, capacitive `trigger/touch` and
`thumbrest/touch`, and one `XR_ACTION_TYPE_VIBRATION_OUTPUT` haptic action.
The same binding set is suggested against both
`/interaction_profiles/oculus/touch_controller` and
`/interaction_profiles/meta/touch_controller_plus`; a runtime that does not
know the second profile rejects it, which is expected rather than an error.
Haptics use core `xrApplyHapticFeedback` and need no extension.

### ECS (EnTT, `ptv::World`)

Thin wrapper around an EnTT registry with a stable `string_id → entity` map.
Core components: `CTransform`, `CMesh`, `CPhysics`, `CMechanicBinding`,
`CTag`, `CTransformOffset`, `CSkeleton`, `CAnimator`.

### Transform FX (`ptv::tick_transform_offsets`)

`CTransformOffset` is a purely visual, springy displacement applied on top of
`CTransform` **at draw time only** — recoil kick, hit flinch, impact shove.

It exists because `CTransform` is an *output* of the physics step for any
entity with a body: `PhysicsWorld::sync_to_world` overwrites it every frame,
and `builtin.grab` drives held objects by commanding physics rather than by
setting the transform. A mechanic writing a kick into `CTransform` would
either have it erased on the next sync or silently desync the drawn pose from
the physical one.

`tick_transform_offsets` runs once per frame, right after the Simulate phase
(so a kick requested this frame is drawn this frame) and before the draw list
is built. `Renderer::build_items_from_world` composes the offset into the
model matrix via `entity_visual_matrix`. Physics, grab, raycasts and collision
never see it. Mechanics drive it through `ctx.fx`, which clamps magnitudes.

### Skeletal animation (`ptv::tick_animators`)

Rigged assets get two components. `CSkeleton` holds the hierarchy, bind pose
and inverse bind matrices parsed from the GLB, the pose currently being
rendered, and the joint palette the renderer uploads. `CAnimator` holds the
asset's animation clips plus per-joint override flags.

Two things can drive a joint, and they meet in `CSkeleton::local_pose`:

- **A clip** — keyframe tracks advanced by a time cursor, with `STEP`,
  `LINEAR` (SLERP for rotations) and `CUBICSPLINE` interpolation. This is what
  Tripo's animation-retarget output produces, and what a walk cycle is.
- **A procedural override** — a pose a mechanic computes this frame from
  something live, typically finger curl from `input.hands[h].trigger`. There is
  no clip for that: the pose is a function of current input, not of time.

Overrides win per joint, so a character can play its idle clip while code
steers only its head or its trigger finger. Overrides are per-frame statements,
not sticky state: a mechanic that stops writing a joint hands it straight back
to the clip (or to bind pose) on the next tick.

`tick_animators` runs in the same slot and for the same reason as
`tick_transform_offsets` — right after the Simulate phase, before the draw list
is built — so an override requested this frame is on screen this frame. Per
entity it resets non-overridden joints to bind, samples the active clip,
composes world matrices parent-first, and writes
`jointWorld * inverseBind` into the palette.

Joint indices match the source glTF skin's `joints` array exactly, because the
mesh's `JOINTS_0` attribute references that ordering. `SkeletonData::order`
supplies a parent-first traversal over those indices rather than renumbering
them.

Mechanics drive all of this through `ctx.skeleton`, never by touching the
components. See [rendering.md](rendering.md#skinned-geometry-scene_skinnedvert)
for the GPU side.

### Scene loader (`ptv::LoadSceneJson`, `ptv::ApplySceneToWorld`)

Reads the engine-facing JSON emitted by the Python scene compiler and
populates the ECS. No additional LLM call; purely deterministic.

Each entity's `mesh` block may carry `sockets` — named local transforms in
metres (`{"muzzle": {"position": [...], "rotation": [x,y,z,w]}}`) that reach
gameplay through `ctx.sockets`. See
[architecture.md](architecture.md#the-socket-contract).

It may also carry `"skinned": true` with a `joint_count`, written by the scene
compiler from the optimizer's report. `Application::attach_skeletons()` uses it
to decide which entities get a `CSkeleton`, so an all-static scene never opens
a GLB just to find out. Scenes predating the flag (hand-authored samples) fall
back to probing the GLB's JSON chunk, which still decodes no geometry.

### Physics (`ptv::PhysicsWorld`)

Jolt Physics. Two layers (Static, Moving). `rebuild_from_world` creates
rigid bodies for every entity with a `CPhysics` component, using its
`collision_shape`. `step()` advances the simulation; `sync_to_world()`
writes transforms back.

### Mechanics (`ptv::MechanicRegistry`, `ptv::MechanicHost`)

A static registry of `IMechanic` / `IBehaviour` / `IDialogueHook`
factories. Each built-in mechanic lives in its own module under
`src/mechanics/builtin/<mechanic>/` and registers itself at static-init
time; generated modules do the same from
`engine/generated/{mechanics,ai,dialogue}/`.
`src/mechanics/BuiltinMechanics.cpp` holds no mechanics any more — it is
purely the link anchor that stops MSVC discarding the module objects (they
are referenced by nothing but their static constructors).

`MechanicHost::attach_bindings(world, ctx)` walks every `CMechanicBinding`
and instantiates the matching mechanic, collapsing `MechanicScope::Player`
mechanics to a single shared instance.
`update(world, dt, input, ctx, phase)` ticks the instances in one phase; the
frame calls it twice, `Input` before the snapshot freezes and the physics
step, `Simulate` after. See [`docs/input-and-bindings.md`](input-and-bindings.md).

### Debug UI (`ptv::DebugUI`)

Dear ImGui via the GLFW backend. Renders into the mirror window only.

### Save system (`ptv::SaveSystem`)

JSON snapshots of per-entity state. The scene is always authoritative on
load; the save file only overlays state (e.g. inventory, health).

## Generated-code rules

Every generated C++ file MUST:

- live under `engine/generated/{mechanics,ai,dialogue}/<project_id>/<name>.cpp`,
- implement `ptv::IMechanic`, `ptv::IBehaviour`, or `ptv::IDialogueHook`,
- register itself via `PTV_REGISTER_MECHANIC("<id>", TypeName)` or sibling
  macros,
- only include whitelisted headers (see [`bounds.py`](../apps/orchestrator/ptv_orchestrator/pipeline/bounds.py)).

The orchestrator's code-gen stage strips non-whitelisted `#include` lines
and rejects paths outside the three allowed subtrees.
