# Input bridge & controller bindings

How live Meta Quest 3 Touch Plus input reaches generated gameplay
mechanics, and how each mechanic learns which control it responds to.

## The input bridge

OpenXR input flows into generated mechanics every frame:

```
XrRuntime::sync_input  ->  InputState (raw OpenXR: triggers, grips,
                            thumbsticks, A/B/X/Y, hand/aim/head poses)
Application.cpp         ->  composes rig-applied WORLD-space poses and
                            smoothed hand velocities
                        ->  provisional InputSnapshot
MechanicHost::update    ->  Phase::Input mechanics (locomotion, grab, throw)
Application.cpp         ->  FREEZES the InputSnapshot (held_entity is now
                            this frame's value)
                        ->  physics step
MechanicHost::update    ->  Phase::Simulate mechanics -- the default
```

`IMechanic::on_update(world, entity, dt, input, ctx)` is called in whichever
phase the mechanic declares.

### Why there are two phases

Some mechanics PRODUCE state that others CONSUME in the same frame. Grab is
the canonical case: which entity a hand holds must be settled before the
snapshot is frozen. Were grab to run after, every other mechanic would read
last frame's `held_entity`, and a weapon mechanic would not notice it had
been picked up until a frame later. Running grab in `Phase::Input` also puts
its kinematic target in place *before* the physics step, so held objects do
not lag or jitter.

Generated mechanics should stay on the default `Phase::Simulate`, where ECS
transforms are already settled for the frame. Use `Phase::Input` only to move
the player or to decide what a hand is holding.

`InputSnapshot` (`engine/runtime/include/ptv/api/InputSnapshot.h`) is a
plain read-only POD containing NO OpenXR types (glm + std only), so it
stays inside the generated-code include whitelist. It exposes, per hand
(`InputSnapshot::Left` / `Right`): `trigger`, `grip`, `thumbstick`,
previous-frame `prev_trigger` / `prev_grip`, `trigger_touched` /
`thumbrest_touched` (+ `prev_*`), `hand_*`/`aim_*` poses,
`velocity` (smoothed world m/s), and `held_entity`. Global:
`button_a/b/x/y` (+ `prev_*`), `head_*`, `dt`.

### Capacitive touch (capsense)

The Touch controllers report a finger **resting** on a control before it is
pressed, which is the difference between an index finger curled in mid-air and
one laid along the trigger. Two boolean actions carry this, bound per hand on
both `/interaction_profiles/oculus/touch_controller` and
`/interaction_profiles/meta/touch_controller_plus`:

| OpenXR path | Snapshot field |
| --- | --- |
| `/input/trigger/touch` | `hands[h].trigger_touched` |
| `/input/thumbrest/touch` | `hands[h].thumbrest_touched` |

Capsense is independent of the analog value: a resting finger reports touched
with a `trigger` of 0. Helpers: `trigger_touching(hand)`,
`trigger_resting(hand)` (touched but not pulled), `trigger_touch_started(hand)`,
`thumbrest_touching(hand)`, `thumbrest_touch_started(hand)`.

On hardware or runtimes that do not report capsense these stay false, which
reads as "finger not there" -- the safe degradation. A runtime that does not
recognise `touch_controller_plus` simply rejects that profile and keeps the
bindings it took from `oculus/touch_controller`.

Capsense exists for exactly one visible reason: `builtin.hand_pose` uses it to
give a finger that is RESTING on a control a slight bend. Without it every
idle hand curls its fingers only when a control is actually pulled, so the
player's hand looks like it is permanently pointing.

### Player hand poses (`builtin.hand_pose`)

The player's hands are rigged entities, not decoration: a 16-joint skinned
mesh generated at build time by
[`engine/tools/make_hand_fixture.py`](../engine/tools/make_hand_fixture.py)
(Tripo cannot rig a hand -- every rig type it offers is a whole creature).
`Application::create_hand_entities` loads them, keeps their `CTransform` on
the `/grip/pose` each frame, and exposes them through
`ctx.hands->hand_entity(h)`.

`builtin.hand_pose` is Player-scoped, runs in `Phase::Simulate`, and states
the finger pose every frame from the current input:

| Fingers | Driver |
|---|---|
| Index | `trigger`; `trigger_touched` without a pull gives a resting bend |
| Middle, ring, pinky | `grip`; pinned closed while the hand holds something |
| Thumb | `thumbrest_touched`, or a face-button touch on that hand |

The engine binds it automatically alongside the locomotion built-ins, so a
generated scene never has to ask for it and generated code must never
re-implement it. Curl is smoothed with the same exponential time constant the
locomotion built-ins use, so fingers do not snap between frames.

If the generated hand mesh is missing, `hand_entity` returns invalid, the
mechanic does nothing, and the renderer falls back to the old procedural
palm-and-wrist blocks.

A weapon that reacts to the trigger must NOT be coupled to the hand. Both read
the same `input.hands[h].trigger` scalar, which keeps them synchronised every
frame with no event plumbing and no ordering dependency between the two
mechanics.

### Engine services (`MechanicContext`)

`on_update` also receives `MechanicContext& ctx`
(`engine/runtime/include/ptv/api/MechanicContext.h`) -- the only sanctioned
way to move the player or touch physics, and the reason built-in locomotion
and grab can be ordinary mechanics rather than engine-core special cases:

| Service | Purpose |
| ------- | ------- |
| `ctx.locomotion` | Read/move the player rig, arbitrate the active locomotion mode, read the scene's comfort `tunables()`. |
| `ctx.physics` | `raycast`, `set_kinematic`, `move_kinematic`, `apply_impulse`. A narrow facade -- generated code never includes `ptv/physics/` or Jolt. |
| `ctx.draw` | One-frame lines, polylines and markers (the teleport arc uses this). |
| `ctx.hands` | Publish/read which entity each hand holds. |
| `ctx.fx` | Short-lived VISUAL transform juice: recoil kick, hit flinch, impact shove. |
| `ctx.haptics` | Controller vibration. `pulse(hand, duration_s, amplitude)`. |
| `ctx.sockets` | Named attachment points on an asset's mesh: `socket_world(entity, "muzzle")`. |

Every mutating call is validated and CLAMPED inside the engine: non-finite
values are rejected, teleport distance is capped to the scene's limit and
never lands below the floor, and impulse magnitude and per-call rig
translation are bounded. Generated code is untrusted, so a bad value has to
degrade into "nothing happens" rather than fling the player out of the world.
Clamps log once per site so they are debuggable.

#### `ctx.fx`: why not just write `CTransform`?

Because for any entity with a physics body, `CTransform` is an **output** of
the physics step. `PhysicsWorld::sync_to_world` overwrites position and
rotation from the Jolt body every frame, and `builtin.grab` drives held
objects by commanding physics (`move_kinematic`) rather than by setting the
transform. A recoil kick written into `CTransform` is either erased on the
next sync or silently desyncs the drawn pose from the one raycasts and
collision use.

`ctx.fx->impulse(entity, kick)` accumulates a per-entity offset
(`CTransformOffset`) that decays back to rest and is applied **only at draw
time**, in `Renderer::build_items_from_world`. Physics, grab and collision
never see it, so nothing fights over ownership of the entity's position.

Offsets are expressed in the entity's own local space, so a weapon kicks along
its own barrel however the player is holding it. Magnitudes are clamped to
25 cm / 45 degrees. This is also how a generated mechanic produces a hit
reaction with no damage-event system: whatever code detected the hit calls
`impulse`.

#### `ctx.haptics`: pulse on the edge

Pulse on a rising edge, never every frame -- overlapping pulses collapse into
a flat buzz that carries no information. The engine rate-limits each hand to
about 30 ms as a backstop. Duration is capped at 0.5 s and amplitude clamped
to 0..1. Underneath, one `XR_ACTION_TYPE_VIBRATION_OUTPUT` action with both
hand subaction paths drives `xrApplyHapticFeedback`; this is core OpenXR and
needs no extension.

#### `ctx.sockets`: never hard-code a muzzle offset

Generated code cannot see the GLB, so it must ask for a named point and let
the asset supply the numbers:

```cpp
const Mat4 m = ctx.sockets->socket_world(self, "muzzle");
const Vec3 origin    = Vec3(m[3]);   // position column
const Vec3 direction = Vec3(m[2]);   // +Z is the socket's forward
```

That same code is correct on a pistol, a rifle and a rocket launcher. The
per-asset transforms come from the deterministic `asset_contract` stage --
see [architecture.md](architecture.md#the-socket-contract). Declare every
socket you read in the CodeTask's `required_sockets`; the scene compiler fails
the build on a mismatch. An unknown socket falls back to the front of the
entity's bounding sphere, which is a safety net rather than a design.

### Mechanic scope

`MechanicScope::Entity` (the default) creates one instance per bound entity.
`MechanicScope::Player` creates ONE shared instance no matter how many
entities bind it -- `on_attach` still runs per entity (that is how grab marks
each prop grabbable) but `on_update` ticks exactly once. Player-global systems
need this: `builtin.grab` is bound to every grabbable prop, and an
entity-scoped per-hand grab loop would grab and throw once per prop.

Convenience helpers: `trigger_pressed/released/down(hand)`,
`grip_pressed/released/down(hand)`, `button_a_pressed()` … (rising edge),
`button_down("a")`, `button_pressed("a")`, and
`hand_holding(entity) -> 0/1/-1`.

Touch layout: **A/B are on the RIGHT controller; X/Y on the LEFT.**
Triggers, grips, and thumbsticks exist on both hands.

## Controller bindings

A mechanic does not hard-code which button it listens to. The
MechanicPlan agent assigns a `ControllerBinding` (`control` +
`hand`) to every input-driven mechanic:

* If the prompt names a binding ("press A to jump", "left trigger
  fires"), it is used verbatim.
* Otherwise the agent picks a VR-best-practice default from the prompt
  and binding rules (no web search).

`control` is one of `trigger | grip | thumbstick | a | b | x | y | none`
(`none` = the mechanic is not directly controller-driven, e.g. a wave
timer or health system). `hand` is `left | right | either`.

### Validation

`binding_validator.validate_bindings` runs at the end of the MechanicPlan
stage and is deterministic:

* Normalizes face-button hands (A/B → right, X/Y → left) with a warning.
* **Warning** if a mechanic rebinds an engine-reserved control (left
  thumbstick = locomotion, right thumbstick = snap turn, both grips =
  grab/throw) unless it IS that locomotion/grab system.

Sharing a control is **allowed**: multiple mechanics may bind the same
control on the same hand (e.g. a UI pointer that clicks with the trigger
while that hand also grabs / fires a weapon). The validator never rejects a
plan on binding grounds — it only normalizes hands and emits warnings.

### Materialization

At scene compile, each resolved binding is written into the entity's
`mechanic_bindings.params` as `control` + `hand` (per-entity params win).
The running mechanic reads them at `on_attach` and checks the live
`InputSnapshot` in `on_update`:

```cpp
// on_attach(World&, std::uint32_t, const nlohmann::json& params,
//           MechanicContext&)
control_ = params.value("control", std::string("a"));
hand_str_ = params.value("hand", std::string("right"));

// on_update(World&, std::uint32_t, float dt, const InputSnapshot& input,
//           MechanicContext& ctx)
if (control_ == "a" && input.button_a_pressed()) { /* jump */ }
```

## Held weapons (GLB-asset mechanics)

A mechanic bound to a grabbable GLB acts only while held:

```cpp
int h = input.hand_holding(entity);           // -1 if not held
if (h < 0) return;
auto hand = static_cast<InputSnapshot::Hand>(h);
if (input.trigger_pressed(hand)) { /* fire */ }
```

Projectiles/VFX should leave the barrel: read a local-space
`muzzle_offset` from params and transform it by the holding hand's aim
pose. Give the projectile a `CPhysics` + `CTransform` so Jolt integrates
it and cover/collision blocks it.

## Scope

The bridge makes generated mechanics *capable* of reacting to real input;
per-game feel still depends on the generated C++.

`IMechanic` keeps context-free `on_attach` / `on_update` overloads that the
context-taking ones forward to, so mechanics generated before
`MechanicContext` existed still compile untouched. New code should override
the context-taking forms; re-run the code-generation stage on an older
project to regenerate it against them.
