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
previous-frame `prev_trigger` / `prev_grip`, `hand_*`/`aim_*` poses,
`velocity` (smoothed world m/s), and `held_entity`. Global:
`button_a/b/x/y` (+ `prev_*`), `head_*`, `dt`.

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

Every mutating call is validated and CLAMPED inside the engine: non-finite
values are rejected, teleport distance is capped to the scene's limit and
never lands below the floor, and impulse magnitude and per-call rig
translation are bounded. Generated code is untrusted, so a bad value has to
degrade into "nothing happens" rather than fling the player out of the world.
Clamps log once per site so they are debuggable.

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
