# Physics: measured colliders, the player's body and hands, grounded NPCs

Jolt 5.5 runs the physics on both targets (`engine/runtime/src/physics/
PhysicsWorld.cpp`). This page is what the engine does so that props rest on
their real bottoms, the player's invisible body never shoves them, the hands
DO touch things, and NPCs stand on the floor.

It replaced three defects that fed most of what went wrong in the Navy SEAL
and shooting-range APKs: every GLB prop had a 0.5 m fallback box (a 9 cm
grenade was a 1 m ball), the player's character and kinematic capsule
collided with everything on one layer (radius 0.29 m, wider than the
avatar), and the NPC floor probe hit the NPC's own rifle and climbed it.

## Measured colliders

`asset_optimization` measures every unskinned asset
(`pipeline/collision_shapes.py`, `COLLISION_VERSION`): the local AABB, a
convex hull of at most 64 points (96 sampled directions) from a low LOD,
and a collision-mesh LOD whose error is under 3 % of the asset's size.
`scene_compile` writes them in metres as `mesh.collision {aabb_min,
aabb_max, hull_points, mesh_path}`; `SceneLoader` keeps them on
`CCollisionGeom`, which clones copy.

`PhysicsWorld::add_body_for` builds the shape from that data:

| `collision_shape` | body | shape |
| --- | --- | --- |
| `mesh` | static | Jolt `MeshShape` from the collision LOD (`IAssetSource`) |
| `mesh` / `hull` | dynamic, kinematic | `ConvexHullShape` from the hull points |
| `aabb` / `box` | any | `BoxShape` in a `RotatedTranslatedShape` on the AABB centre |
| `sphere` | any | sphere of the measured bounds |

Bodies are placed by their ORIGIN and synced back with
`GetPositionAndRotation` (never the centre of mass), so an offset collider
never moves the drawn mesh. The old `0.25 * scale` box remains only for
legacy entities with no measurements, with a warning. `teleport` also zeroes
a dynamic body's velocity (`IPhysicsAccess::teleport_body`, used by a
restocked grenade).

## Object layers

| layer | what | collides with |
| --- | --- | --- |
| `STATIC` | level geometry | moving, prop, held, player body, debris |
| `MOVING` | NPC capsules, kinematic world | static, moving, prop, held, player body |
| `PROP` | dynamic props | static, moving, prop, held, player parts, debris |
| `HELD` | held / attached items (`CAttachment`) | static, moving, prop, held (a body filter skips the holder) |
| `PLAYER_BODY` | the player's character + capsule | static, moving |
| `PLAYER_PART` | hand / forearm / shin colliders | prop, debris |
| `DEBRIS` | broken pieces | static, prop, player parts |

(`layers_collide` in `PhysicsWorld.cpp` is the table.)

`PLAYER_BODY` does not collide with `PROP`, `HELD` or `DEBRIS`, so walking
into a table of guns moves nothing. A `CharacterContactListener` on the
player's `CharacterVirtual` rejects light props in `OnContactValidate` and,
for props heavier than 25 kg (which still block), sets
`mCanReceiveImpulses = false` / `mCanPushCharacter = false`.

Floors come from `STATIC` plus bodies tagged `walkable`
(`IPhysicsAccess::raycast_floor`); nothing on `HELD` is ever a floor.

## The player's body

* **Radius from the real body**: Full Player Avatar -- the torso half-depth
  measured in the asset contract's model frame (`model_frame.torso_radius`),
  clamped to 0.15-0.25 m; Hands Only -- 0.18 m
  (`player_bindings.player_body_radius`). Written as
  `player.body_radius_m`; the kinematic contact capsule uses the same radius.
* **Body parts** (`core/PlayerParts.cpp`) on `PLAYER_PART`: a palm and finger
  capsules per hand following the drawn hands; for a full avatar also the
  forearms and shins from the avatar's joints. They move with
  `MoveKinematic` under a 4 m/s clamp (teleporting past 0.35 m) and push
  props as a real hand or leg would. An untracked hand has no colliders.
  Their entities are tagged `player_part` + `collider_only` and are never
  drawn: untagged, each once drew as a 0.3 m white placeholder cube over the
  player's fingers and feet.
* **Reaching does not knock things over**: while a hand's grip is open, its
  parts ignore every grabbable within 14 cm (`PartContactFilter`); grab
  reach is measured from the palm to the closest point of the item's
  collider (`kGrabSurfaceReach` 10 cm), not to its origin.
* Dynamic props start asleep and settled at load.

## Grounded NPCs

* `builtin.character_locomotion` probes the floor with `raycast_floor`
  starting `kProbeAboveSoleM` (0.5 m) above the soles, ignoring the NPC and
  its whole attachment family, and refuses a "floor" more than a step above
  the current soles (the ledge rule).
* Every skinned NPC has one ground writer: `encounters.py` binds
  character_locomotion for `stationary` NPCs too (`stationary: true`,
  grounded motor, optional `face_player`), so a range goblin stands on the
  floor and turns to the player without travelling.
* The encounter director and the scene compiler place NPCs at
  `floor + foot_offset_m` explicitly.
* Telemetry measures the floor with a static-only probe from above
  (`vy`, `feet_gap`), and verification checks no vertical drift
  (`VERTICAL_DRIFT_MPS`) besides the floor gap.
* A grounded character that finds no floor within reach while standing
  more than 2 m above where it last stood returns to that spot (logged
  once: `character_locomotion: '<id>' lost its floor ...`) instead of
  holding a height nothing supports.
* `Application::watch_npc_heights` logs `npc_jump: '<id>' y a -> b at
  <phase>` for any skinned NPC that rises more than 1 m between two points
  of one frame (before physics, after the physics sync, after mechanics)
  during the first 600 frames, so a lift can be pinned on the subsystem
  that caused it.

## Kinematic targets

`PhysicsWorld::move_kinematic` records the target pose; `step(dt)` applies
every target with `MoveKinematic(target, dt)` using the STEP's own `dt`,
then stops any kinematic body that received no new target. Jolt turns a
target into a velocity (`(target - current) / dt`) and integrates it over
the next step, so applying it with the caller's `dt` overshot by the ratio
of two frame times: a 2 ms frame before a 65 ms load hitch turned a few
centimetres of ground snap into Jolt's 500 m/s velocity cap and launched
every NPC in the Navy SEAL huts 32.38 m into the air on frame 3. A
`teleport` drops any pending target; removing a body drops its target.

## Tests

`ptv_smoke`: `measured_colliders`, `npc_ground_snap`, `npc_held_item_floor`,
`player_never_pushes_props`, `player_parts`, `physics_kinematic_hitch` (a
2 ms target then a 65 ms step lands exactly; no target, no coasting), and
`scene_npc_ground <scene.json>` (a diagnostic that loads a real scene and
reports every NPC's soles against the floor). Orchestrator:
`test_collision_shapes.py`. On the headset, the interactions pass
(`docs/quest-build.md`) settles every prop, walks the player into each one
and reaches for it.
