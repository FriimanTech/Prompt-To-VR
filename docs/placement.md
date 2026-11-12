# Placement: where things go, in the world that was actually built

Gameplay entities (guns, a red button, a challenge's targets) are placed IN
the world the world architect built -- on its tables, under its poles, on
its stands -- by a contract between `scene_spec`, `world_generation` and
`scene_compile`, then checked by a validator that fails loudly.

It replaced a rigid merge: `scene_spec` laid everything out around the
spawn point knowing nothing of the world, `scene_compile` pasted both layers
together, and the shooting range shipped with four tables (two of each
layer's), guns beside the wrong ones and lane-2 targets on the floor.

## Base alignment

A position means where the object's BOTTOM rests. Generated GLBs are centred
on their origin, so "origin at the floor" sank every table halfway in.
`WorldContext::place_object` takes `align` (`Base` default, `Center`, `Top`)
and aligns by the asset's measured bounds (rotated), writing
`mesh.aabb_min/aabb_max` and `align` on the entity. `asset_size(id)` gives
an asset's real size (`size`, `bottom`, `top`) so a builder can put a lamp on
a crate at the crate's real height.

## Ownership

`placement.world_owned_assets` decides deterministically who places what:
static, non-interactive assets (tables, consoles, posts, stands, shelves)
are the WORLD's -- marked `world_builds_it: true` in the architect's asset
list and exempt from scene_spec coverage; interactive items (anything the
interaction design gives a usage, NPCs, pickups) are GAMEPLAY's. A
world-owned asset that scene_spec also places is dropped from gameplay
(`drop_world_owned_copies`), named.

## Placement intents (scene_spec)

`EntitySpec.placement` says where in WORDS, never invented coordinates for
things resting on world furniture:

| relation | means |
| --- | --- |
| `on_surface` | rests on a support's top face (`anchor`) |
| `on_floor` | stands on the floor |
| `hang_from` | its TOP hangs from a pole / hook point |
| `mount_on_wall` | its back on a wall face, facing the normal |
| `at_anchor` | its origin exactly at the anchor |
| `free` | the spec's own transform (legacy) |

plus `anchor` (one point) or `anchor_set` (many, for runtime spawns),
`count`, `group` and `description`. `scene_spec` fails its gate when an
interactive entity resting on furniture has no anchor. The composer writes
through the strict `WireSceneSpec` (`scene_spec_wire.py`), which carries
`placement` and `satisfies` too -- a field missing there is a field the
agent cannot emit, and a test asserts the wire covers every `EntitySpec`
field.

## Anchors (world_generation)

`world_generation` receives the placement requests (entity, asset, measured
size, relation, group, the prompt's words) and must declare each:

* `w.gameplay_anchor({id, position, facing, normal, relation, support})` --
  `position` is ON the support's top face for `on_surface`, the bottom end
  of the pole for `hang_from`, on the wall face for `mount_on_wall`;
* `w.anchor_set({id, relation, points, facing})` for spawn sets (the hang
  points under lane 2's drop rods, pickup pads);
* `w.satisfies(req_id, how)` for the layout requirements it builds.

`placement.validate_anchors` checks every request has its anchor (or a set
with enough points), each anchor sits on / under real geometry within 1 cm,
and the item's footprint fits the pad. Problems go back into the world's
compile-and-repair loop; the last attempt is kept with warnings and
scene_compile then fails naming what is missing.

## Resolve, settle, validate (scene_compile)

Right after merging the world:

1. `resolve` computes each entity's transform from its anchor, relation,
   measured bounds and rotation (`_place_on_anchor`; a mount uses
   `p = anchor - c + n * (c . n - back)`), tagging it `settled`.
2. `settle` rests everything on its support (static first, then dynamic):
   a prop's bottom on the highest support under it. A loose dynamic
   grabbable standing on edge (its height more than `UNSTABLE_RATIO` 1.5x
   its thinnest footprint side -- a rifle balanced on its side) is first
   laid flat (`lay_flat`: thinnest local axis onto +Y, yaw kept, bottom
   kept), both when resting on an anchor and for unanchored props, so it
   does not topple the moment physics wakes. "On edge" is also judged by
   the measured hull (`tips_over`): the box centre must stand over the
   footprint of the hull points within a contact band of the bottom -- a
   rifle upright on its magazine is under the ratio and still falls. A free
   loose item (not attached, not a template) floating more than 3 cm above
   the support under it is rested on it and tagged `settled`, instead of
   being dropped by physics on frame one. Last, anchored resting items are
   re-rested on the MEASURED top of the support under them when it is
   within `REANCHOR_TOL_M` (12 cm) of the anchor: the world's anchor height
   is the architect's idea of a table, and a table prop settled after the
   anchors resolved can stand a few centimetres taller (every gun on the
   rerun's tables was 4 cm into them).
3. `violations` fails the stage (`PlacementContractError`) on an item sunk
   more than 2 cm, floating more than 3 cm, with nothing under it, hanging
   from nothing, with a missing anchor, or interpenetrating another.

The report is `builds/scene/placement_report.<scene>.json`. Anchors and
anchor sets are copied into `scene.json` (`anchors`, `anchor_sets`).

## Runtime

`ctx.anchors` (`IAnchors`, `core/GameBoards.h`): `count(set)`,
`point(set, i, &pos, &facing)`, `relation(set)`, `anchor(id, &pos,
&facing)`, and `pose_at(entity, set, i, &pos, &rot)` -- the pose that rests /
hangs / mounts that entity there by its collider. `builtin.target_challenge`
spawns on it; generated code must too (API contract v6 rejects spawns at
typed-in coordinates when the world declares anchors).

## Tests

`test_placement.py` (resolve, settle, violations, ownership, the anchor
validator), `test_worldgen_anchors.py` (builds `ptv_worldgen` with the
`sample.anchor_range` builder and validates what it emits), `ptv_smoke
target_challenge` (hangs targets from anchor points).
