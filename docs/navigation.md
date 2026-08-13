# Navigation

Getting an NPC from where it is to where it wants to be, without it walking
into walls or getting stuck beside doorways.

## One interface, layered backends

Generated gameplay code only ever sees `ctx.nav`
([INavigation](../engine/runtime/include/ptv/api/MechanicContext.h)):

```cpp
Vec3  next_waypoint(agent, target);          // one step, avoidance applied
bool  is_reachable(from, to);
bool  path_distance(from, to, float& out);
Vec3  project_to_ground(pos);
bool  line_of_sight(from, to);
```

Behind it sit two layers with very different costs. Because everything goes
through these five calls, changing or upgrading a layer never touches a line of
generated code.

### Layer 0 — local steering

Always on. Obstacle whiskers cast forward and 35 degrees to each side; when
the way ahead is blocked the desired direction blends toward whichever side has
more room. A separation push keeps agents off each other, without which ten
enemies converging on the player stack into one flickering mass. A downward ray
keeps them on the floor.

This is enough for an open arena and cannot solve a building. Steering is
reactive — it knows only what its whiskers hit this frame — so an NPC told to
chase the player into a room presses against the wall beside the door and
oscillates there. That is not a tuning problem; finding a door is a global
question.

### Layer 1 — routing over declared topology

The global layer is a graph of walkable rectangles (`NavRegion`) joined by
crossing points (`NavLink`), searched with A*. `find_path` returns the
corridor's corners; `next_waypoint` steers toward the next one and re-plans
only when the destination moves more than a metre or the current corner is
reached. A* every frame for every NPC is exactly the cost that makes
navigation unaffordable on a Quest, and the answer barely changes between
frames.

## Why the world builder declares it

The usual way to build a navigation graph is to voxelise finished geometry
(Recast). That exists to *reconstruct* topology nobody wrote down.

In this engine somebody did write it down. Levels are not scanned — they are
authored in C++ by the world-architect agent, which knows it is placing a room
here and a doorway there. So `IWorldBuilder` gained two calls:

```cpp
w.nav_region(NavRegionDesc{"hall", min, max, floor_y});
w.nav_link(NavLinkDesc{"hall", "vault", door_centre});
```

Reconstructing what the author already knew is wasted work and a source of
error. Declaring it is exact, costs no bake step, and adds no dependency.

Cover the floor the player and NPCs are meant to use. Overlap at doorways is
fine. A ramp or staircase is several regions, one per landing. A single open
arena is one region and no links.

## Validation

The declaration comes from an LLM, so it is checked before it can ship. The
`world_generation` stage rejects a world when:

- a link names a region that does not exist;
- a link's crossing point lies outside both regions it joins (reads fine in
  the JSON, sends every NPC into a wall);
- the player spawn is not inside any region;
- any region is unreachable from the spawn;
- a region is too small to stand in, which is almost always a units mistake.

The failure text goes straight back into the architect's repair loop, so the
agent fixes its own declaration. Each of these produces a level that looks
correct and has NPCs standing still in it, which is why they fail the build
rather than warn.

Declaring **no** navigation is legitimate and not an error: an open arena
needs none, and NPCs fall back to steering.

## Data flow

```
IWorldBuilder::nav_region / nav_link
        -> world.scene.json  "navigation": { regions, links }
        -> stage_scene_compile merges it into scene.json
        -> SceneLoader parses into SceneDoc.nav_regions / nav_links
        -> Application::build_nav_graph resolves ids to indices
        -> ptv::nav::NavGraph, queried through ctx.nav
```

Ids are strings in the authored data because they are what an agent can write
reliably; they become indices once, at load.

## Using it from generated code

```cpp
const Vec3 wp = ctx.nav->next_waypoint(self, target_pos);
Vec3 dir = wp - my_pos;
dir.y = 0.0f;
if (glm::length(dir) > 1e-4f) {
    intent.desired_velocity = glm::normalize(dir) * speed;
}
```

`next_waypoint` is cheap enough to call every frame. `is_reachable` and
`path_distance` run a graph search — call them when a decision changes, not
per frame per NPC. Use `is_reachable` before committing to a target, or an
NPC will set off confidently toward a rooftop it can never arrive at.

## The layer that does not exist yet

Recast/Detour would add navigation over geometry the world builder did not
author. Nothing currently produces that: GLB props are discrete objects the
steering layer avoids anyway. If it is ever needed it drops in behind the same
`INavigation` interface, and no generated game changes.
