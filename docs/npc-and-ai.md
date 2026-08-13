# NPCs: from a rigged model to something that plays like an enemy

Tripo can produce a goblin, rig it, and give it walk, run and shoot clips.
None of that makes an enemy. This document covers what turns those assets into
characters that move, fight, take damage and die — and, just as importantly,
where the line falls between what the engine does and what generated C++ does.

The rule that shapes everything below: **the engine owns the parts every game
needs and every game would otherwise get subtly wrong; generated code
expresses only what makes this game different.**

## The pipeline stage that reads intent

`npc_plan` runs after `asset_manifest` and before `mechanic_plan`. It reads the
user's own prompt and decides, per character, what it is for: its role,
faction, how it moves, what behaviours it needs, and which animation clips
those behaviours actually use. The result is `npc_plan.json`
([NpcPlan](../packages/schemas/ptv_schemas/npc_plan.py)).

It sits where it does for two reasons.

**Before rigging, because that is where the money goes.** A rig costs about 30
Tripo credits and each animation clip about 20 more. Whether a character needs
any of that is a question only the prompt can answer — "the ducks should just
slide across the ground, no animations" is an instruction about intent, not
about the asset. The stage also reconciles the manifest with its own decision,
clearing `rigged` and `animations` on anything it marked
`slide_no_animation`, so the rigging stage never sees it. Without that
reconciliation the two artifacts drift and the project pays for animation
nothing will play.

**Before mechanic and scene planning, because both need the same answer.**
Three agents reading the same prose independently will reach three different
conclusions about the same goblin. Reading it once and handing the result
downstream is what keeps the scene composer, the mechanic planner and the
codegen agents describing the same game.

A game with no characters produces an empty plan and skips the agent call
entirely. That is a complete answer, not a degraded one.

### Reading a prompt correctly

Explicit instructions win. If the prompt says the enemies are not animated,
they are not animated, however odd it looks.

Silence is not "do nothing". A prompt about Green Lanterns flying together
never states that the *allied* Lanterns can fly; they get flight anyway,
because a Green Lantern that walks is not the game that was asked for. Both
readings can apply within one plan: detailed about one character, inferred for
another.

## Movement and animation are one system

The single most visible failure in a generated character is foot sliding: the
feet cycle at one speed while the body travels at another, and the character
skates. It happens because Tripo retargets with `animate_in_place=True`, so
the clip carries no root motion — the engine moves the character, and nothing
automatically ties the two together.

`builtin.character_locomotion` is what ties them together. Bind it to every
character that moves. Generated AI code writes a desired velocity and stops:

```cpp
auto& want = world.emplace_or_replace<CLocomotionIntent>(self);
want.desired_velocity = direction * speed;
```

The built-in then, every frame:

1. sweeps the body through the world so it cannot walk through walls, and
   snaps it to the floor;
2. turns it toward travel at a configured rate (or toward `face_target`, for
   an enemy that strafes while keeping its weapon on you);
3. picks idle, walk or run from the speed it **achieved** — a character
   pressed against a wall falls back to idle rather than running on the spot;
4. sets clip playback rate to `achieved_speed / reference_speed`, clamped to
   0.8–1.25.

Step 4 is the industry-standard fix, and the clamp is the important part.
Scaling a clip more than about 25% reads as moon-walking or scurrying rather
than as speed, so past the clamp the right answer is to change clip tier
instead of stretching one clip further. Tier changes use hysteresis so a
character hovering at the boundary does not flicker between two clips.

`reference_speed` — how fast a clip *looks* like it is travelling — cannot be
measured from an in-place clip, so the scene compiler derives it from the
character's height and emits it as `mesh.animation_speeds`. A 1.8 m biped gets
about 1.4 m/s walking and 4.0 m/s running; a knee-high creature gets
proportionally less.

Setting `animated: false` turns steps 3 and 4 off and keeps 1 and 2. That is
the ducks: the same mechanic, no separate code path, and nothing for a codegen
agent to choose wrongly.

### When Tripo has no clip for something

The ability is never blocked on the animation. `NpcSpec.animation_fallback`
picks the strategy:

- `nearest_preset` — accept the closest clip Tripo offers.
- `procedural_cpp` — generated code animates the rig itself through
  `ctx.skeleton->set_joint_rotation`, which is right for simple repetitive
  motion like wing flaps or a turret's recoil.
- `none` — ship it unanimated.

A flying NPC still flies in all three cases.

## Health, factions and death

`CHealth` and `CFaction` are engine components, and damage goes through
`ctx.combat`. They have to be shared: an enemy authored by one CodeTask must be
able to hurt a player set up by another, and an allied NPC has to recognise
that enemy without the two tasks having agreed on anything.

Hostility is **symmetric** — it is enough for either faction to name the other.
An asymmetric rule produces the classic bug where enemies shoot your squad and
your squad never shoots back, because only one of the two tasks filled in
`hostile_to`.

Damage carries an invulnerability window, because contact damage is tested
every frame and without one a single touch empties a health bar in a few
frames.

Death is the engine's job. Reaching zero health plays the `die` clip, removes
the collider so corpses stop blocking the living, drops the locomotion intent
so the body stops walking through its own death animation, and destroys the
entity when the clip ends. Generated code never writes a death timer. The
player is the exception: their death stops there and `is_alive` goes false, so
whatever restart flow the prompt asked for is gameplay, not engine policy.

The player is an ordinary combat entity — `ctx.hands->player_entity()`, with
health, a faction and a collision capsule that follows the headset. Nothing
special-cases them.

## Perception and navigation

`ctx.perception` answers "who can I see": nearest hostile, hostiles in range,
field-of-view and line-of-sight checks. All of it skips dead entities, which
is the difference between a squad that advances and a squad standing around
aiming at a corpse.

`ctx.nav` answers "how do I get there". See [navigation.md](navigation.md).

## Spawning

Generated code cannot build a mesh, a rig or a collider — all of that is
decided by the asset pipeline. So spawning clones a **template**: a hidden,
fully-formed entity the scene authored, tagged `"template"`, which the
renderer skips and physics ignores.

```cpp
const std::uint32_t e = ctx.spawn->instance(template_entity, pos, rot);
```

The clone carries the template's mesh, LOD chain, sockets, collider, skeleton,
clips, tags and mechanic bindings, and — unlike an entity created by hand — it
is registered with physics and the mechanic host, so it collides and its
mechanics tick from the next frame. Before this existed, everything about an
entity was decided once at scene load, and anything spawned mid-game was a
ghost: visible, with no collider and no behaviour.

## Equipping: two shapes of the same problem

Tripo sometimes returns "goblin holding a rifle" as one mesh and sometimes
returns a goblin and a rifle as two. Both happen, so both are supported.

**One mesh.** The `muzzle` socket is derived from geometry by
`asset_contract`, which knows nothing about skeletons — so the socket is a
fixed offset in the bind pose, and on an animated character it would hang in
mid-air while the arm swings past it. Each socket is therefore bound to its
nearest joint at load, and `socket_world` pushes it through that joint's
palette entry, exactly as the surrounding vertices are pushed. In bind pose
the palette is identity, so static props behave exactly as before.

**Two meshes.** `ctx.attach` parents one entity to a point on another. The
anchor resolves as a named socket first, then an exact joint name, then a
loose alias (`hand_r`, `hand_l`, `head`, `spine`, `chest`, `hips`, `root`)
matched against whatever Tripo called the bones. Attachments resolve after the
pose system, so a rifle in a hand swings with the walk cycle. A scene can also
declare this up front with `EntitySpec.attached_to`.

## Performance

Animated characters are the most expensive thing a Quest 3 renders. Meta's own
figures put a "busy simulation" — their term for an app with many animated
NPCs — at 200–300 draw calls and 1.3–1.8 M triangles per frame, and they call
out animation and skinning specifically as work that delays the render thread.

Three mechanisms keep that in bounds:

- **A separate budget.** `PerformanceProfile.max_animated_agents` (12 on a
  standard Quest scene, fewer on larger ones, roughly triple on PC VR). The
  scene compiler warns when a scene places more than that at load, and
  `ctx.spawn` refuses to clone a skinned entity past the cap. Enemies stop
  appearing; the headset does not stutter.
- **Animation LOD.** Characters within 8 m are posed every frame, within 15 m
  every other frame, then every fourth, and beyond 40 m not at all. Skipped
  time is banked and repaid on the next sample, so a distant character still
  covers the same ground per second — only the smoothness drops, which is
  invisible at that range.
- **Telemetry.** The per-second `perf:` line reports `rigs N (M animating,
  budget B)` next to the draw-call and triangle counts, so a frame-rate
  problem caused by skeletons does not look like a rendering problem.

## What generated code must never do

- Move a character by writing `CTransform`, or play its locomotion clips.
  `builtin.character_locomotion` owns both, and a second writer gets the
  foot-sliding wrong.
- Write its own death timer, corpse cleanup or damage invulnerability.
- Guess a raw glTF clip name. Ask for the intent; the engine resolves it.
- Hard-code a socket offset. Ask for the socket by name.
