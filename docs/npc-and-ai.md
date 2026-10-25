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
   snaps it to the floor. The floor probe starts on top of the character's
   own capsule, so it passes through that body and every other character
   (`IPhysicsAccess::raycast_excluding(self, ..., floor_only=true)`): Jolt
   treats convex shapes as solid, and a plain ray hit the NPC's own capsule
   at distance zero and lifted it by its height every frame -- every NPC in
   every game left the level on its first frame. The rise per frame is also
   capped, so no probe result can launch a character;
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

### Humanoids animated by Uthana

A biped humanoid NPC (human or not) can be rigged and animated by Uthana
instead of Tripo. `character_routing` decides this after the meshes exist; see
[uthana.md](uthana.md). Such a character has:

- `idle`, the shared library idle;
- `walk` and `run` in a style that fits it (zombie, crouching, limp, robot,
  ...);
- optionally gait variants such as `run.punching`.

It has no one-shot action clips. Those are listed as `missing` in the codegen
agent's animation inventory and use `animation_fallback`.

A behaviour that wants the punching gait while charging sets
`CLocomotionIntent::gait_variant = "punching"`. The body still moves at
`run_speed_mps`; only the clip changes. A one-shot `play_clip(self, "attack",
false)` on any character now plays to the end before locomotion takes the clip
back. A character with no `idle` clip settles into its rest pose when it
stops, instead of walking in place.

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
special-cases them. It is ONE entity: when the scene has a `player_rig` (it
always does for compiled games), the engine adopts that entity as the player,
keeping its id, tags, health, faction and bindings, and drives its transform
from the headset every frame. It used to create a separate `__player`, so two
entities were tagged `player` and the scene's rig stood still at its authored
spot -- generated code that found "the player" by name chased the world
origin, and health bound to the rig never saw a hit.

Damage is observable. `ctx.combat->damage_seq(entity)` is the sequence number
of the latest hit that landed on it, and `damage_events_since(entity, seq,
out, max)` returns the hits themselves (instigator, amount, health after,
where the hit came from). A health bar, damage vignette, hit haptic or
game-over flow reads these, so it reacts to a built-in enemy, another task's
projectile or a trap alike. A private "hit inbox" only its own task writes,
or a private health pool that never reads `ctx.combat`, is rejected at
codegen -- the built-in enemies never touch either.

## Perception and navigation

`ctx.perception` answers "who can I see": nearest hostile, hostiles in range,
field-of-view and line-of-sight checks. All of it skips dead entities, which
is the difference between a squad that advances and a squad standing around
aiming at a corpse. Sight rays pass through the looker's and the target's own
bodies; the eye sits inside the looker's capsule, and a self-hit used to count
as "clear", so NPCs saw and hit through walls. Melee reach is horizontal but
only on roughly the same level (within 2 m vertically).

`ctx.nav` answers "how do I get there". See [navigation.md](navigation.md).

## Population: unique types, instances and waves

"A goblin game" needs two different numbers, and conflating them is how a
game ends up with one goblin or with fifty paid-for models:

- **Unique types** are models. Each costs a reference image, a mesh, a rig
  and animation clips. They are decided once, in the BuildSpec's
  `npc_roster`.
- **Instances** are copies of one rigged, animated model at runtime. They
  cost nothing in the pipeline and are decided by the NPC plan.

### Who decides what

| Question | Stage | Artifact |
|---|---|---|
| Which NPC families exist and how many unique variants each | `build_spec` | `npc_roster` |
| One model per variant | `asset_manifest` | one `AssetEntry` per variant |
| Role, faction, stats and behaviour per variant | `npc_plan` | `NpcSpec` |
| Copies placed at load (a companion) | `npc_plan` | `NpcSpec.persistent_count` |
| Who arrives during play, in which waves, how many at once | `npc_plan` | `NpcPlan.encounters` |
| Where NPCs may enter the level | `world_generation` | `npc_spawn_zones` |
| Where a placed-at-load NPC stands (a guard post, a counter) | `world_generation` | `npc_anchors` (optional) |
| One hidden template per spawned variant, the director, per-variant behaviour params | `scene_compile` | `scene.json` |
| That each placed NPC stands on walkable floor, at floor + foot offset | `scene_compile` | `scene.json` |
| Spawning, wave progression, follow and melee behaviour at runtime | engine | the built-ins below |
| That it all actually happens on the headset (Quest only, report-only) | `quest_runtime_verify` | `runtime_verification.json` |

World generation never spawns or names NPCs. It declares SPACE -- walkable
regions, entrances, posts -- because it is the only stage that knows the
layout; the NPC plan decides who exists; the scene compiler joins the two and
validates the result against the merged world; the runtime director spawns.

### Variants

The prompt's wording sets the variant count (`NpcFamily.count_reading`):

- **singular** ("a dragon", "Green Lantern as my ally") — 1 variant.
- **explicit** ("two kinds of zombies") — that many.
- **plural, unspecified** ("goblins", "waves of zombies") — exactly 3: a
  `regular`, a `tall` and a `big_bulky` physique. Each variant carries stat
  multipliers (health, damage, speed), so a bulky goblin is visibly slower
  and hits harder.

`stage_build_spec` checks the roster against these rules and retries;
`stage_asset_manifest` checks that every variant has its own asset and retries
once with the list of missing ones.

### The NPC plan's population

Each `NpcSpec` carries its family and variant and its own stats. Two fields
say how many exist:

- `persistent_count` — copies placed at load that stay. A singular character
  and an ally are always placed once (`repair_npc_plan` enforces it).
- `encounters` — the types that arrive during play: `mode` (`waves`,
  `continuous`, `fixed`), a `start` trigger (`on_load`, `delay`,
  `player_grabs_tag`, `signal`), a list of `waves` made of `{asset_id,
  count}` groups, and the caps `max_concurrent`, `spawn_interval_s`,
  `inter_wave_delay_s`, `min_player_distance_m`. A wave's count can exceed
  `max_concurrent`; the rest arrive as earlier ones fall.

When the director writes no encounters for spawned enemies,
`default_encounters` builds a progression ordered by physique: the regular
variant first, mixed with the next, then the strongest. A single type gets
three growing waves. The same repair runs again in `scene_compile`, so a
project planned before encounters existed still gets waves.

### What the scene compiler does

`encounters.apply_npc_population` makes `scene.json` hold exactly the plan's
population, whatever the SceneSpec composed:

- every spawned type gets **one** entity tagged `template`, parked at
  y = -50, never `pooled`. Extra pre-placed copies are removed, because an
  enemy the director did not spawn never counts toward its wave;
- persistent NPCs are placed beside the player spawn (right and slightly
  behind, never in front of the view);
- each NPC gets health, faction, a kinematic capsule and **its own type's**
  speeds and damage on `builtin.character_locomotion` and on the follow or
  melee built-in. An NPC bound to a generated AI mechanic keeps it instead;
  two mechanics writing one locomotion intent fight every frame;
- a single `game_director` entity binds `builtin.encounter_director` with
  the waves, which reference the template entity ids.

After the world is merged, `finalize_encounter_zones` gives the director the
world's `npc_spawn_zones`, or, when the world declared none, up to six zones
derived from its nav regions, at least 12 m from the spawn and spread apart.

The SceneSpec's gameplay layer is authored around the origin, but the
generated world puts its own spawn somewhere else. `gameplay_relocation`
moves the whole layer rigidly (position and yaw) to the world spawn, then
nudges anything that lands inside a static collider out of it and onto a nav
region. Templates, the avatar, the rig and attached gear are left alone.

## Spawning

Generated code cannot build a mesh, a rig or a collider — all of that is
decided by the asset pipeline. So spawning clones a **template**: a hidden,
fully-formed entity the scene authored, tagged `"template"`, which the
renderer skips and physics ignores.

```cpp
const std::uint32_t e = ctx.spawn->instance(template_entity, pos, rot);
```

The clone carries the template's mesh, LOD chain, sockets, collider, skeleton
and clips (playback reset), tags (minus `template`), mechanic bindings,
health (reset to full), faction, equip slots, movable and interactive state.
Gear attached to the template — a goblin's club — is cloned too and attached
to the copy, and it is destroyed with it. The clone is registered with physics
and the mechanic host, so it collides and its mechanics tick from the next
frame.

Generated code does not spawn NPCs, though. That belongs to the director.

## The encounter director

`builtin.encounter_director` (one per game, on the `game_director` entity)
runs the NPC plan's encounters:

- **Start.** On load, after a delay, when the player grabs something with a
  given tag (picking up the blaster), or on `ctx.encounters->start()`.
- **Spawning.** It clones the wave's templates interleaved by type, one per
  `spawn_interval_s`, while fewer than `max_concurrent` are alive and the
  animated-agent budget allows. A refused spawn retries half a second later.
- **Placement.** It picks a spawn zone at least `min_player_distance_m` away,
  preferring one the player cannot see, and spawns facing the player. With no
  zones it uses a ring around the player and keeps only reachable points.
- **Progress.** A defeat is counted once, when an instance dies or is
  destroyed. A cleared wave starts the inter-wave countdown; clearing the last
  wave is victory (`waves`), the last wave repeats (`continuous`), or play
  simply continues (`fixed`). The player's death stops it.

Everything else reads it through `ctx.encounters`: `configured()`, `phase()`
(`Idle`, `Active`, `InterWave`, `Victory`, `Failed`), `wave_index()`,
`wave_count()`, `alive()`, `remaining_in_wave()`, `defeated_total()`,
`countdown_s()`, and the counters `wave_started_seq()`, `wave_cleared_seq()`
and `victory_seq()` that tick once per event. A menu may `pause()` or
`reset()` it. A wave HUD is a generated mechanic that reads these; it never
spawns, pools or counts NPCs itself.

## Built-in NPC behaviours

Two behaviours every game needs are built in, and the scene compiler tunes
them per NPC type from the NPC plan:

- **`builtin.npc.follow`** — a companion keeps a band behind its leader
  (the player by default), walks when the gap is small and runs when it
  grows, navigates around walls, and is placed behind the leader again when
  it falls more than `catchup_distance_m` behind or stays stuck for
  `stuck_catchup_s`.
- **`builtin.npc.melee_chase`** — an enemy acquires the nearest visible
  hostile (or a tagged target), chases it through the nav graph keeping clear
  of its siblings, telegraphs an attack with a wind-up, deals damage if the
  target is still within reach, then cools down. Taking damage staggers it.
  Speed, damage, range, cooldown and sight come from that type's `NpcSpec`,
  so the bulky variant really is slower and harder-hitting.

Both only write `CLocomotionIntent`; `builtin.character_locomotion` moves and
animates the body. The mechanic planner maps "follows the player" and
"chases and hits" onto these, and generates code only for what they cannot
express: ranged attacks, flight, special abilities.

That is enforced, not just requested. `reconcile_npc_ai_items`
([npc_population.py](../apps/orchestrator/ptv_orchestrator/pipeline/npc_population.py))
runs after the mechanic-plan agent AND at the start of code generation: an
AI item (`generate` or a code edit) whose NPCs' behaviours a built-in fully
covers -- `follow_leader` (follow); `chase`, `melee_attack`,
`react_to_damage` (melee chase) -- becomes `builtin+config` of that
built-in. Its config carries over, minus keys the built-in does not read
(named in the run log). Stationary and flying NPCs, and anything with an
uncovered behaviour (`reposition`, `ranged_attack`, `flee`, `patrol`, ...),
keep their generated AI. Running it at codegen start is what lets a project
planned before the rule resume from `code_generation`: the board drops the
task and deletes its files.

## Generated NPC AI

Generated AI (`kind: ai`) implements `IBehaviour` and registers with
`PTV_REGISTER_BEHAVIOUR`. The engine hosts it exactly like a mechanic --
`MechanicHost` falls back to the behaviour registry and wraps the result --
attached per bound entity and ticked once per frame after physics. It used to
register into a factory map nothing ever read: every generated AI compiled,
linked, logged "No mechanic registered" at load and did nothing.

Behaviours override the context-taking forms,
`on_attach(world, e, params, MechanicContext&)` and
`on_tick(world, e, dt, const InputSnapshot&, MechanicContext&)`, and so get
the same services a mechanic does. The context-free forms remain only so old
projects keep compiling; `codegen_contracts.py` rejects a new AI written
against them, a player looked up by name (`find("player_rig")`), and a private
damage inbox, and feeds the violations back to the agent once. The same rules
mark already-generated code stale (`API_CONTRACT_VERSION` on the codegen
board), so fixing a rule regenerates exactly the tasks it applies to.

Unresolvable bindings no longer pass silently either: the scene compiler
checks every `builtin.*` / `generated.*` binding against the registrations in
the engine source and this project's generated code, drops generated NPC AI
that is not in the build (so the NPC gets the built-in behaviour instead of
standing still) and fails the stage, listing every binding that would do
nothing. At load the engine logs the same list as an error.

## Templates are inert

A spawn template is a blueprint: hidden, without a collider, parked at
y = -50. Its mechanics are not attached either -- `MechanicHost` skips
entities tagged `template`, and each clone attaches its own copy when the
director spawns it. Ticking the template's mechanics used to run an invisible
enemy under the floor that chased the player.

## Placing NPCs in the world

After the world is merged, `place_persistent_npcs`
([encounters.py](../apps/orchestrator/ptv_orchestrator/pipeline/encounters.py))
puts every NPC placed at load:

- at the first unused world `npc_anchor` naming it (by `asset_id`, or a tag
  equal to its asset id, family or role) -- the guard at the gate;
- otherwise where the population step put it (a companion beside the spawn,
  carried there by the gameplay relocation);
- moved onto the nearest declared walkable floor if it is off it;
- with its origin at floor height plus its foot offset, so its feet touch
  the floor on frame one. That matters most for a STATIONARY NPC, which has
  no locomotion to snap it down later.

An NPC the game requires whose model is missing fails `scene_compile` (the
asset safety net already re-adds any NPC the SceneSpec forgot, so reaching
this means the GLB itself is gone), and `quest_build` refuses to package a
scene whose NPC GLBs (mesh or LOD) are not in the APK.

## Verifying on the headset

Every NPC bug above built, packaged and installed cleanly, so the engine now
reports what its NPCs do:

- once per scene, an `npc_manifest` line per NPC (skeleton loaded, each
  animation intent resolved to a clip, bound and attached mechanics, any that
  did not resolve) and an `npc_manifest_summary`;
- every second, an `npc` line per live NPC (position, `feet_gap` above the
  floor, measured speed, intent speed, current clip, alive, distance to the
  player) and an `npc_summary` (encounter phase, alive, wave, hits on the
  player);
- a `spawn:` line per clone and a `combat:` line per hit that lands.

Verification mode (Android property `debug.ptv.verify` = 1, or `PTV_VERIFY=1`
on desktop) walks the player around a 6 m square (5 s per side, then a 3 s
pause) and keeps them alive while still recording hits (published to the
damage feed with amount 0, so generated health that mirrors the feed stays
full and does not end the run the engine is keeping alive). An enemy that lands a
hit on the player is defeated 0.8 s later (`verify: defeated ...`), standing
in for the player's weapon, so the waves keep advancing and every NPC type in
the plan appears during one unattended run.

The `quest_runtime_verify` stage (Quest projects only) uses both; see
[quest-build.md](quest-build.md#on-headset-verification).

## Spawn zones

The world architect declares where NPCs come in with
`w.npc_spawn_zone(NpcSpawnZoneDesc{id, position, radius, tags})`: gates,
tunnel mouths, the far side of a ridge. These are written to the world's
`npc_spawn_zones` and copied into `scene.json`. The world-generation brief
asks for at least three zones, 12 m or more from the spawn, whenever the NPC
plan has encounters, and to keep 3 m around the spawn clear for companions.
A world with no zones still works; see the fallback above.

## Animation clip pairing

Tripo returns a character's clips sorted by name, not in the order they were
requested. `asset_animation` therefore pairs each requested intent (`run`,
`attack`, `hurt`, `die`) with a clip by name and only falls back to order for
what is left. A project animated before this fix is re-paired on the next
run, from the files it already has, without paying Tripo again.

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
- Spawn, pool or despawn NPCs, or tag anything `pooled`. The encounter
  director owns the population; a HUD reads `ctx.encounters`.
- Steer an NPC that is bound to `builtin.npc.follow` or
  `builtin.npc.melee_chase`.
- Guess a raw glTF clip name. Ask for the intent; the engine resolves it.
- Hard-code a socket offset. Ask for the socket by name.
- Find the player by name or tag. Use `ctx.hands->player_entity()`.
- Invent a damage inbox or a health pool of its own. Read
  `ctx.combat->health` / `damage_seq` / `damage_events_since`.
- Implement only the context-free `IBehaviour` forms.
