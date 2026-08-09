# Architecture

## High-level

```
+----------------+       +--------------------+       +-----------------+
| Web (Vite/TS)  |<----->| Orchestrator (FastAPI) |<->| Provider APIs   |
|                |  SSE  |                    |  HTTP | Anthropic        |
| projects / UI  |       | pipeline / storage |       | Google (Imagen) |
+----------------+       +----------+---------+       | Tripo            |
                                    |                  +-----------------+
                                    |  writes to projects/<id>/
                                    v
                      +---------------------------+
                      | Local filesystem         |
                      | JSON artifacts, images,  |
                      | GLB assets, generated    |
                      | C++, logs, builds        |
                      +---------------------------+
                                    |
                                    |  scene.json + generated C++
                                    v
                     +-------------------------------+
                     | Engine runtime (C++/Vulkan/XR) |
                     | ptv_starter_room.exe           |
                     | ptv_<project>.exe              |
                     +-------------------------------+
                                    |
                                    v
                        Meta Quest Link (OpenXR)
                        Quest 3 as PC VR HMD
```

## Why this split?

- The **engine core is stable and hand-authored**. Rendering, XR, memory, and
  ECS are never touched by generated code.
- The **pipeline is deterministic where possible**. The only places we allow
  LLM non-determinism are the planning stages and the `engine/generated/`
  C++ modules, both of which are bounded and schema-validated.
- **Schema-first** means each downstream stage reads a validated JSON
  artifact, not the original prompt. This prevents drift and lets the user
  resume a partial run.

## Data flow (per project)

```mermaid
flowchart TD
  Prompt[prompt.txt] --> BuildSpec[build_spec.json]
  BuildSpec --> AssetManifest[asset_manifest.json]
  BuildSpec --> MechanicPlan[mechanic_plan.json]
  AssetManifest --> MechanicPlan
  MechanicPlan --> SceneSpec[scene_spec.*.json]
  BuildSpec --> SceneSpec
  AssetManifest --> SceneSpec
  SceneSpec --> Refs[images/*/ref_NN.png]
  Refs --> Tripo[assets/*/model.glb]
  Tripo --> Normalize[asset_normalization.json]
  Normalize --> Contract["asset_contract: derive sockets"]
  Contract --> Optimize["asset_optimization: decimate + LOD chain"]
  Contract --> CppGen
  MechanicPlan --> CppGen["engine/generated/<kind>/<project_id>/*.cpp"]
  SceneSpec --> SceneCompile[scene.json]
  Optimize --> SceneCompile
  Contract --> SceneCompile
  SceneCompile --> CMake[builds/cmake-*/*.exe]
  CppGen --> CMake
  CMake --> Launch[exe launch via OpenXR]
```

## Engine subsystems

| Subsystem | Files | Responsibility |
|---|---|---|
| Platform | `src/platform/Window.cpp` | GLFW mirror window |
| Render | `src/render/VulkanContext.cpp` | Instance / device / queue / VMA |
| XR | `src/xr/XrSession.cpp` | Instance / session / action set / swapchains / frame loop |
| ECS | `src/ecs/World.h` | EnTT registry + stable id map |
| Transform FX | `src/ecs/TransformOffset.cpp` | Draw-time visual offsets (recoil kick, flinch) that never touch physics |
| Skeletal | `src/ecs/Skeleton.cpp` + `src/render/GltfSkinnedLoad.cpp` | Rig loading, clip sampling, procedural joint override, joint palettes |
| Scene | `src/scene/SceneLoader.cpp` | JSON scene → ECS |
| Physics | `src/physics/PhysicsWorld.cpp` | Jolt PhysicsSystem |
| Mechanics | `src/mechanics/MechanicRegistry.cpp` + `src/mechanics/builtin/<mechanic>/` | Data-driven mechanics; each built-in is its own `IMechanic` module |
| Debug | `src/debug/DebugUI.cpp` | Dear ImGui overlay (mirror-window only) |
| Save | `src/save/SaveSystem.cpp` | JSON world snapshot / load |
| Application | `src/core/Application.cpp` | Main loop |

Every subsystem exposes its public API through `engine/runtime/include/ptv/`.
Generated C++ may only include headers rooted at `ptv/api/`, `ptv/ecs/`,
`ptv/scene/`, `ptv/math/`, `ptv/mechanics/`, `ptv/dialogue/`, `ptv/ai/`,
plus common STL and well-known third-party prefixes (`glm/`, `entt/`,
`nlohmann/`).

## The socket contract

A **socket** is a named local transform on an asset -- "the muzzle is here,
pointing that way". Sockets exist because of a hard constraint in the
pipeline: the code-generation agent never sees the GLB, so it cannot know
where any particular barrel ends, and a pistol, a rifle and a rocket launcher
all differ. Three stages each own one third of the answer, and no single stage
can hold the whole thing:

| Clause | Owner | Artifact |
|---|---|---|
| "this mechanic needs a muzzle" | `mechanic_plan` -> `CodeTask.required_sockets` | `mechanic_plan.json` |
| "this asset's muzzle is at (x, y, z)" | `asset_contract` (deterministic, `ptv_glb_sockets`) | `asset_contract.json` |
| "this mechanic is bound to this entity" | `scene_spec` | `scene_spec.*.json` |

`scene_compile` is the only stage that runs after all three, so it is both the
join point (writing `mesh.sockets` in metres) and the validator: a mechanic
requiring a socket its asset cannot supply fails the build with every
violation listed at once.

Socket positions are stored in `asset_contract.json` in **unit space**
(normalised by the mesh's longest-axis extent) and converted to metres at
compile time by multiplying `target_scale_meters` -- the same convention
`bounds_radius_m` already uses. Unit space is what makes a socket survive the
LOD chain, since decimation shifts the AABB slightly.

Generated C++ reads them through `ctx.sockets->socket_world(entity, "muzzle")`
and never hard-codes an offset. An unknown socket falls back to the front of
the entity's bounding sphere rather than identity.

## Skinning

A socket is a fixed point on a rigid mesh. A **skin** is the other case: the
mesh itself deforms. Rigged assets carry a joint hierarchy, per-vertex bone
indices and weights, and inverse bind matrices; the engine renders them through
a second Vulkan pipeline that skins in the vertex shader off a shared
joint-matrix storage buffer.

The chain is deliberately narrow so a static scene pays nothing:

| Stage | Owner | Result |
|---|---|---|
| Add a skeleton to a generated creature | `asset_rigging` (Tripo rig-check + rig) | `model_rigged.glb` |
| Retarget preset clips onto that skeleton | `asset_animation` (Tripo retarget) | `model_animated.glb` + intent→clip map |
| Parse skin, hierarchy, `JOINTS_0`/`WEIGHTS_0`, IBMs, clips | `GltfSkinnedLoad.cpp` | `SkinnedVertex` (48 B) + `SkeletonData` |
| Preserve the rig through decimation | `ptv_glb_optimize` | LOD chain with a byte-identical skeleton at every level |
| Record that an asset is rigged | `asset_optimization` → `scene_compile` | `mesh.skinned` / `mesh.joint_count` in `scene.json` |
| Attach the rig, evaluate the pose | `Application::attach_skeletons`, `tick_animators` | `CSkeleton` / `CAnimator`, joint palette per frame |
| Draw | `scene_skinned.vert` | Skinned geometry |

Two rules make the pieces fit:

- **Vertices stay in skin space.** Per the glTF spec a skinned mesh node's
  transform is ignored, since the joint matrices already place the mesh. The
  palette is therefore `jointWorld * inverseBind` with no mesh-node term, which
  has the useful property that a bind-pose palette is the identity — so a
  rigged mesh drawn un-posed is pixel-identical to the same mesh loaded as
  static geometry, and that is exactly what the smoke tests assert.
- **Joint indices are never renumbered.** They match the source skin's `joints`
  array, because `JOINTS_0` references that ordering; a parent-first traversal
  order is carried alongside instead.

Generated C++ poses a rig through `ctx.skeleton`, either by playing a baked
clip or by writing individual joints each frame from live input. See
[runtime-subsystems.md](runtime-subsystems.md#skeletal-animation-ptvtick_animators)
and [rendering.md](rendering.md#skinned-geometry-scene_skinnedvert).

Two rigs come from different places, and the split is not arbitrary:

- **Creatures** are rigged by Tripo in the `asset_rigging` stage. Its rig
  types (`biped`, `quadruped`, `hexapod`, `octopod`, `avian`, `serpentine`,
  `aquatic`) all describe a whole animal, and the free `rig-check` call gates
  the paid one.
- **The player's hands** are generated locally by
  [engine/tools/make_hand_fixture.py](../engine/tools/make_hand_fixture.py),
  because a disembodied hand is not any of those rig types and Tripo would
  refuse it. They are ordinary ECS entities carrying the rigged mesh, so the
  same skinned pipeline draws them, and `builtin.hand_pose` curls their
  fingers from the controller.

## The clip contract

Sockets solved "the codegen agent cannot see the mesh". Animation clips have
the identical problem one layer up: **Tripo's retargeter names the clips**, so
a generated mechanic writing `play_clip(self, "walk")` would be guessing.

The fix is the same shape. `AssetItem.animations` holds semantic intents;
`asset_animation` maps each to a preset, retargets, then reads back the names
that are really in the returned GLB and records intent → real name;
`scene_compile` writes that map as `mesh.animations`; and
`ISkeleton::play_clip` resolves through it before falling back to an exact
name match.

| Clause | Owner | Artifact |
|---|---|---|
| "this enemy needs to walk and shoot" | `asset_manifest` → `AssetItem.animations` | `asset_manifest.json` |
| "walk is the clip called Walk_01 on this asset" | `asset_animation` | `asset_animation.json` |
| "this asset is on this entity" | `scene_spec` | `scene_spec.*.json` |

An asset that ships an `idle` clip starts it looping the moment the scene
loads. That is the one clip choice with an obvious answer, and without it a
room of enemies holds its bind pose until some mechanic thinks to start
them — which reads as broken rather than idle.

## Grabbing by the grip socket

`builtin.grab` used to freeze whatever relative pose an object happened to
have when the player squeezed, so a weapon approached from above stayed held
from above. It now solves for the pose that puts the asset's derived `grip`
socket in the hand:

```
local_rotation = inverse(socket.rotation)
local_offset   = -(inverse(socket.rotation) * (scale * socket.position))
```

The hand cancels out of that solve entirely, which is the point: the object
lands the same way no matter how it was reached for. An asset with no `grip`
socket keeps the old relative hold, and `snap_to_grip: false` opts out for
things with no correct orientation (a ball, a rock). This is the socket
system paying off a second time, with no new stage and no new schema field.

## Threading

The orchestrator runs stages in a FastAPI background task. The native engine
has a single main thread for rendering + XR + ECS + mechanics; Jolt spawns its
own worker threads internally.

## Resumability

Every stage is idempotent: rerunning it rewrites its artifacts. The
orchestrator's `resume_pipeline()` skips stages whose last `StageRecord`
status is `succeeded`. See
[`apps/orchestrator/ptv_orchestrator/pipeline/orchestrator.py`](../apps/orchestrator/ptv_orchestrator/pipeline/orchestrator.py).
