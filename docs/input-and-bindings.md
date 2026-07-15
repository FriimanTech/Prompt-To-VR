# Input bridge & controller bindings

How live Meta Quest 3 Touch Plus input reaches generated gameplay
mechanics, and how each mechanic learns which control it responds to.

## The input bridge

OpenXR input flows into generated mechanics every frame:

```
XrRuntime::sync_input  ->  InputState (raw OpenXR: triggers, grips,
                            thumbsticks, A/B/X/Y, hand/aim/head poses)
Application.cpp         ->  builds InputSnapshot (rig-applied WORLD-space
                            poses + per-hand held entity) and calls
                            mechanics_.update(world, dt, snapshot)
MechanicHost::update    ->  IMechanic::on_update(world, entity, dt, input)
```

`InputSnapshot` (`engine/runtime/include/ptv/api/InputSnapshot.h`) is a
plain read-only POD containing NO OpenXR types (glm + std only), so it
stays inside the generated-code include whitelist. It exposes, per hand
(`InputSnapshot::Left` / `Right`): `trigger`, `grip`, `thumbstick`,
previous-frame `prev_trigger` / `prev_grip`, `hand_*`/`aim_*` poses, and
`held_entity`. Global: `button_a/b/x/y` (+ `prev_*`), `head_*`, `dt`.

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
* Otherwise the agent picks a VR-best-practice default (it may
  `web_search` for current Quest 3 ergonomics).

`control` is one of `trigger | grip | thumbstick | a | b | x | y | none`
(`none` = the mechanic is not directly controller-driven, e.g. a wave
timer or health system). `hand` is `left | right | either`.

### Validation

`binding_validator.validate_bindings` runs at the end of the MechanicPlan
stage and is deterministic:

* Normalizes face-button hands (A/B → right, X/Y → left) with a warning.
* **Hard error** if two different mechanics claim the same control on the
  same hand (`either` conflicts with both specific hands).
* **Warning** if a mechanic rebinds an engine-reserved control (left
  thumbstick = locomotion, right thumbstick = snap turn, both grips =
  grab/throw) unless it IS that locomotion/grab system.

### Materialization

At scene compile, each resolved binding is written into the entity's
`mechanic_bindings.params` as `control` + `hand` (per-entity params win).
The running mechanic reads them at `on_attach` and checks the live
`InputSnapshot` in `on_update`:

```cpp
// on_attach
control_ = params.value("control", std::string("a"));
hand_str_ = params.value("hand", std::string("right"));

// on_update
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
per-game feel still depends on the generated C++. Existing projects
authored before the bridge must re-run the code-generation stage so their
mechanics are regenerated against the new `on_update` signature.
