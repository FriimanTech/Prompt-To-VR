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

**Cross-fading.** Switching clips used to be a hard cut, so every gait change
popped. `CAnimator` keeps a `prev_clip` and a blend timer; the outgoing clip
keeps advancing on its own clock during the fade, because blending a moving
pose against a frozen one is still a hitch, just a softer one. With no
incoming clip the target is the bind pose, which makes a faded `stop_clip`
settle to rest rather than snap there.

**Distance LOD.** `tick_animators(world, dt, viewer, lod)` throttles by
distance: every frame under 8 m, every other frame under 15 m, every fourth
beyond that, and not at all past 40 m (the bands come from
`scene.performance.animation_lod_distances_m`). Posing a 65-joint skeleton
costs the same whether the character fills the view or is a speck, and Meta
call out animation and skinning as work that delays the render thread.
Skipped time is banked in `lod_debt_s` and repaid on the next sample, so a
distant character still covers the same ground per second — only the
smoothness drops.

**Sockets on a rigged mesh.** `CSkeleton::socket_joint` binds each named
socket to its nearest joint at attach, and `socket_world` pushes the socket
through that joint's palette entry, exactly as the surrounding vertices are
pushed. Without it a `muzzle` derived from a goblin holding a rifle would
hang in mid-air while the arm swung through the walk cycle. In bind pose the
palette is identity, so static props are unaffected.

Mechanics drive all of this through `ctx.skeleton`, never by touching the
components. See [rendering.md](rendering.md#skinned-geometry-scene_skinnedvert)
for the GPU side and [npc-and-ai.md](npc-and-ai.md) for how clip choice is
driven by movement.

### Player hands

The player's hands are the engine's own rigged entities, `__hand_left` and
`__hand_right`, created by `Application::create_hand_entities` from the
16-joint meshes generated at build time under `PTV_ENGINE_ASSET_DIR`. Being
ordinary entities with `CTransform` + `CMesh` + `CSkeleton` means the normal
draw path renders them through the skinned pipeline with no special-casing;
the only per-frame work the application does is keep their `CTransform` on the
`/grip/pose`.

`builtin.hand_pose` curls the fingers from the controller, and
`ctx.hands->hand_entity(h)` is how any mechanic reaches them. When the
generated mesh is absent the entities are never created, `hand_entity` returns
invalid, and the render loop falls back to the procedural palm-and-wrist
blocks — so a checkout without Python still shows hands. See
[input-and-bindings.md](input-and-bindings.md#player-hand-poses-builtinhand_pose).

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

Shapes are box, sphere, triangle mesh (static, from `CInlineMesh`) and
**capsule**. A capsule takes its height from the mesh's
`target_scale_meters` — for an upright character the longest axis IS the
height, and that is exactly what the asset pipeline normalises. Its radius is
a fraction of height rather than a second measured extent, because a
T-posed character's arm span is roughly its height and a measured X extent
would produce a capsule too wide to fit any doorway. Dynamic capsules lock
their X/Z rotation DOFs so a character does not topple on first contact.

`add_body_for` / `remove_body` create and destroy a single entity's body.
`rebuild_from_world` runs once at load, so without them anything spawned
during play was a ghost — visible with no collider. Entities tagged
`"template"` are skipped: they are hidden spawn sources parked off-map and
should not leave invisible obstacles lying around.

Two queries exist beyond the raycast:

- `slide_character(centre, height, delta, ignore)` sweeps a capsule and
  returns the movement actually available, sliding along whatever it hits.
  This is what stops the player and NPCs walking through walls. It fails
  **open**: if the sweep resolves to almost nothing the mover is probably
  already inside geometry, and in VR being frozen is worse than clipping for
  one frame.
- `overlapping(entity, out)` reports every collider intersecting one entity.
  Deliberately a query rather than a Jolt contact callback: Jolt only reports
  contacts when at least one body is DYNAMIC, and the pair gameplay most
  wants to know about — the player and an NPC — is kinematic on both sides,
  because their motion is authored rather than simulated.

The player is an ordinary kinematic capsule entity (`__player`, tagged
`"player"`) that follows the headset horizontally and the rig vertically.
Before that they were a bare transform, so nothing could collide *with* them
and an enemy that charged the user simply passed through.

### Navigation (`ptv::nav::NavGraph`)

Walkable rectangles and the crossings between them, declared by the world
builder and searched with A*. Local steering lives in the navigation service
in `Application.cpp`; the graph is the global layer that gets an NPC through
a doorway instead of pressing it against the wall beside one. See
[`docs/navigation.md`](navigation.md).

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

`attach_entity` / `detach_entity` do the same for one entity that appeared
after load, so a spawned enemy's mechanics actually tick. The player-scoped
instance map persists across calls, so a mid-game spawn joins the existing
shared instance rather than starting a second copy of grab.

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
