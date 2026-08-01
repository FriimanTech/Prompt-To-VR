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
  `MechanicContext` (locomotion / physics / debug-draw / hand services).
- `ptv/ecs/` — `World`, core ECS components (`CTransform`, `CMesh`, `CPhysics`, …).
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
(`gameplay`) with left/right hand poses + trigger + grip + thumbstick +
A/B/X/Y buttons bound to the Oculus Touch interaction profile, and per-eye
color swapchains. The frame loop (`poll_events` / `begin_frame` /
`end_frame_no_render` / `sync_input`) is public so the application can drive
the XR session without leaking OpenXR types upward.

### ECS (EnTT, `ptv::World`)

Thin wrapper around an EnTT registry with a stable `string_id → entity` map.
Core components: `CTransform`, `CMesh`, `CPhysics`, `CMechanicBinding`,
`CTag`.

### Scene loader (`ptv::LoadSceneJson`, `ptv::ApplySceneToWorld`)

Reads the engine-facing JSON emitted by the Python scene compiler and
populates the ECS. No additional LLM call; purely deterministic.

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
