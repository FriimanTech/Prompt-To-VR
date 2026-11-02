# Runtime subsystems

This doc describes how the hand-authored engine core is laid out.

## Public headers

All engine-public headers live under `engine/runtime/include/ptv/`. Generated
gameplay code may include (enforced by
[`engine/tools/check_generated_bounds.py`](../engine/tools/check_generated_bounds.py)
and the orchestrator's
[`bounds.py`](../apps/orchestrator/ptv_orchestrator/pipeline/bounds.py)) the
`ptv/api/` headers listed exactly in those files (`IMechanic.h`,
`IBehaviour.h`, `IDialogueHook.h`, `InputSnapshot.h`, `MechanicContext.h`),
anything under `ptv/ecs/`, `ptv/math/`, `ptv/mechanics/`, `ptv/dialogue/`,
`ptv/ai/`, `glm/`, `entt/`, `nlohmann/`, an allowlist of pure std headers,
and bare-filename headers of its own task or a declared `depends_on` task.
World builders (`engine/generated/world/`) get only `ptv/api/IWorldBuilder.h`,
std and glm.

- `ptv/api/` — `IMechanic`, `IBehaviour`, `IDialogueHook` and the register
  macros, `InputSnapshot` (live controller/headset input) and
  `MechanicContext`, whose services are: `locomotion`, `physics`, `contacts`,
  `spawn`, `attach`, `combat`, `perception`, `nav`, `draw`, `hands`, `fx`
  (transform juice), `haptics`, `sockets`, `skeleton`, `equip`,
  `interactive`, `persistence`, `transition`, `ui`, `comfort`, `encounters`,
  `game` / `app` (`GameFlow.h`: the game loop, Leave Game, fades, respawn
  placement), `text` (world-space SDF text), `vfx` (`Effects.h`),
  `projectiles` (`Projectiles.h`) and `weapons` (`Weapons.h`).
  `MechanicContext.h` pulls in `Effects.h`, `GameFlow.h`,
  `InteractiveState.h`, `Projectiles.h` and `Weapons.h`. Also here:
  `Facing.h` -- the single yaw convention (`yaw_from_forward`,
  `forward_from_yaw`, `rotation_facing`, `facing_of`, `heading_error`),
  `BodySlots.h` (canonical body-slot vocabulary) and `IWorldBuilder.h`.
- `ptv/ecs/` — `World`, core ECS components (`CTransform`, `CMesh`, `CPhysics`, …),
  plus `TransformOffset.h` (the draw-time visual-offset layer) and
  `Skeleton.h` (the per-frame pose tick).
- `ptv/scene/` — `SceneDoc`, `LoadSceneJson`, `ApplySceneToWorld` (engine
  side; not on the generated-code allowlist).
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
thumbstick, A/B/X/Y buttons, the LEFT menu button (`menu/click`, the game
loop's pause control; the right one belongs to the system), capacitive
`trigger/touch` and `thumbrest/touch`, and one
`XR_ACTION_TYPE_VIBRATION_OUTPUT` haptic action. `request_exit()` calls
`xrRequestExitSession` for Leave Game; `poll_events` ends the session at
STOPPING and reports EXITING, and on Quest `android_main` then calls
`ANativeActivity_finish` and waits for `destroyRequested` before returning,
so the app launches again (KhronosGroup/OpenXR-SDK-Source#329).
The same binding set is suggested against both
`/interaction_profiles/oculus/touch_controller` and
`/interaction_profiles/meta/touch_controller_plus`; a runtime that does not
know the second profile rejects it, which is expected rather than an error.
Haptics use core `xrApplyHapticFeedback` and need no extension.

### ECS (EnTT, `ptv::World`)

Thin wrapper around an EnTT registry with a stable `string_id → entity` map.
Core components: `CTransform`, `CMesh`, `CPhysics`, `CMechanicBinding`,
`CTag`, `CTransformOffset`, `CSkeleton`, `CAnimator`. Gameplay components
(all under `ptv/ecs/`): `CHealth`, `CFaction`, `CLocomotionIntent` (an NPC's
desired velocity, consumed by `builtin.character_locomotion`), `CAttachment`,
`CEquipSlots` / `CEquipped`, `CHoldProfile`, `CSocketInfo`, `CInteractive`,
`CMovable`, `CHighlight`, `CUiPanel`, `CMirrorSurface`, `CSelfViewHide`,
`CStaticBatch`, plus `CInlineMesh` / `CPrimitive` for world-builder geometry.

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

An entity may also carry `equip_slots` — places on a WEARER (a hip holster, a
back sheath) that `builtin.equip` snaps items into, as opposed to sockets,
which are points on an item. The loader turns them into `CEquipSlots`;
without that component `EquipMechanic::pick_slot_near()` has no candidate and
an authored holster accepts nothing. Entries with an empty `name` are
skipped, and an entity declaring no slots gets no component at all — an empty
`CEquipSlots` would make a bench look like a wearer.

`Scene.h` declares its own plain-data `EquipSlotSpec` rather than reusing
`ptv::EquipSlot` from `ptv/ecs/World.h`, and `SceneLoader.cpp` converts
between them. Same reason `HealthProps` mirrors `CHealth`: the scene document
must stay free of entt, because `ptv_worldgen` parses it and links no ECS.

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

`raycast(origin, dir, max, RayFilter, ...)` takes a filter: bodies to pass
through (`ignore`), `static_only`, and `skip_characters` (every capsule body:
NPCs and the player's contact capsule). Jolt treats convex shapes as solid, so
a ray that starts inside or on a body hits it at fraction 0 -- any probe cast
on behalf of an entity must ignore that entity. Gameplay reaches this through
`IPhysicsAccess::raycast_excluding(self, ..., floor_only)`; the character
locomotion floor snap, perception sight lines and the navigation whiskers,
ground projection and point-to-point line of sight all use it.

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

The player is an ordinary kinematic capsule entity, tagged `"player"`, that
follows the headset horizontally and the rig vertically. When the scene has a
`player_rig` entity the engine adopts it as that entity (keeping its id,
tags, health, faction and bindings; its authored collider is replaced by the
capsule); only a scene without one gets an engine-made `__player`. Before
this, both existed and the rig stood still, so code that found the player by
name followed the world origin. Before that the player was a bare transform,
so nothing could collide *with* them and an enemy that charged the user
simply passed through.

`ICombat` records every hit that lands (`DamageLog`, 64 most recent, in
`src/core/NpcRuntime.h`): `damage_seq(entity)` and `damage_events_since`.
Each hit is also logged as a `combat:` line.

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
`engine/generated/{mechanics,ai,dialogue}/`. The engine injects no player
mechanic: every built-in that runs is bound in `scene.json`.
`src/mechanics/BuiltinMechanics.cpp` holds no mechanics any more — it is
purely the link anchor that stops MSVC discarding the module objects (they
are referenced by nothing but their static constructors).

Registered built-in ids (`PTV_REGISTER_MECHANIC` under
`src/mechanics/builtin/`):

| Area | Ids |
|---|---|
| Player locomotion / turning | `builtin.locomotion.teleport` (alias `builtin.teleport`), `builtin.locomotion.slide.head_relative`, `builtin.locomotion.slide.controller_relative`, `builtin.locomotion.snap_turn` (alias `builtin.snap_turn`), `builtin.locomotion.smooth_turn`, `builtin.movement_settings_menu` |
| Player body | `builtin.hand_pose`, `builtin.avatar_body`, `builtin.mirror_surface`, `builtin.head_interior` |
| Interaction | `builtin.grab`, `builtin.throw`, `builtin.equip`, `builtin.ui_panel`, `builtin.recoil` (legacy; stripped by the weapon reconcile when an interaction design exists) |
| Weapons | `builtin.weapon`, `builtin.bow`, `builtin.melee_weapon`, `builtin.shield`, `builtin.item_source`, `builtin.npc.ranged_attack` |
| NPCs | `builtin.character_locomotion`, `builtin.npc.follow`, `builtin.npc.melee_chase`, `builtin.encounter_director` |
| World affordances | `builtin.door`, `builtin.container`, `builtin.mover`, `builtin.switchable`, `builtin.breakable`, `builtin.harvestable` |
| Game loop | `builtin.game_flow`, `builtin.game_hud` |

The planner-facing descriptions, params and player `role:` mapping live in
`apps/orchestrator/ptv_orchestrator/engine_catalog.py`; see
[engine-growth.md](engine-growth.md).

`MechanicHost::attach_bindings(world, ctx)` walks every `CMechanicBinding`
and instantiates the matching mechanic, collapsing `MechanicScope::Player`
mechanics to a single shared instance. An id with no mechanic factory falls
back to the behaviour registry: an `IBehaviour` is wrapped in an adapter and
runs as a Simulate-phase, per-entity mechanic (its context-taking `on_tick`
gets the same `MechanicContext`). Entities tagged `template` are skipped --
their clones attach at spawn. Ids that resolve to nothing are collected in
`unresolved()` and logged once as an error.
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
load; the save file only overlays state (e.g. inventory, health). Keyed on
**stable string ids**, not entt handles — handles are reassigned on every load,
so a save keyed on them restores the wrong objects. Only **deltas** are stored:
a room the player never touched costs nothing. One file per scene
(`save.<scene_id>.json`), so a temple's puzzle state cannot collide with a
laboratory's.

### Scene transitions (`ptv::ISceneTransition`)

A game with several environments needs a way from one to another. Two paths
reach the same place:

- **Portals**, declared by the world architect as a volume plus a target, are
  handled in the frame loop (`tick_portals`) like teleport targets. A portal is
  a region of space with a meaning, not an object the player picks up or breaks,
  so it needs no entity and no mechanic. `requires_interaction` defaults to
  **true**: walking through a doorway you did not mean to take, and losing the
  room you were in, is worse than having to squeeze. The squeeze must also
  *start* while inside — a player who walked in already gripping did not ask to
  leave.
- **`ctx.transition->go_to(scene_id, spawn_point_id)`**, for a transition
  gameplay decides: a menu choosing a mission, a completed objective ending a
  chapter.

The service can only **request**. A mechanic is ticked from inside a loop over
the entities a transition destroys, so performing the swap there invalidates the
caller's iterator. Requests are recorded and honoured at a frame boundary
(`apply_pending_transition`), after every entity loop has finished. Only the
first request in a frame is honoured; two portals touched together would
otherwise chain the player through two loads and land them somewhere neither
named.

Arrival is **named**, not positional, because the right place to stand depends
on where the player came from: returning to a hub from a mission belongs at that
mission's door, not at the hub's front gate. The author knows that and declares
a `spawn_point`; the engine only looks it up, and falls back to the scene's
default spawn with a warning if the name is missing.

`load_scene_at` rebuilds everything derived from the scene, in the same order
`initialize` does, because each step depends on the one before: entities, then
physics bodies, then the player capsule, then the mechanics that sweep against
it. Two ordering details matter — mechanics are detached *first* (an instance
caches an entity id, and ticking once against the new world would drive whatever
entity reused that id), and the incoming save is overlaid *before* physics is
rebuilt (a door left open must be open when its collider is built, or the player
walks into a body that is not where the door is).

`World::clear()` drops the id map along with the entities. entt reuses handles,
so a stale name would resolve *successfully* to whatever took that handle — a
lookup for `front_door` quietly returning a lamp.

Scene files are located through the **scene index**, a `scene_id -> relative
path` map the compiler writes into every document. The runtime must resolve a
sibling without knowing how a project is laid out, and identically inside a
Quest APK where there is nothing to go looking around in. Every scene carries
the whole index, since a menu may reach a scene no portal in the current room
points at. `quest_build` packages `scene.json` plus every `scene.*.json`
sibling; packaging only the entry point leaves a build that runs and looks right
until the player walks into the first doorway.

## Generated-code rules

Every generated C++ file MUST:

- live under `engine/generated/{mechanics,ai,dialogue}/<project_id>/` (world
  builders under `engine/generated/world/<project_id>/`), as `.cpp` plus
  optional `.h` / `.hpp` task-local headers,
- implement `ptv::IMechanic`, `ptv::IBehaviour` (context-taking `on_attach` /
  `on_tick`), `ptv::IDialogueHook`, or `ptv::IWorldBuilder` (world/ only),
- register itself via `PTV_REGISTER_MECHANIC("<id>", TypeName)` or sibling
  macros, in a `.cpp` only,
- only include whitelisted headers (see [`bounds.py`](../apps/orchestrator/ptv_orchestrator/pipeline/bounds.py)).

An include violation fails the CodeTask loudly (it is fed back to the agent
as repair context, never silently stripped), and paths outside the allowed
subtrees are rejected. `codegen_contracts.py` (API contract v5) additionally
rejects code that compiles but fights the engine: context-free AI, finding
the player by name, private damage inboxes, private lives / respawn /
restart, and private weapons, ammo or muzzle raycasts.
