# World Author Guide (read this first)

You are the **VR world-architect agent**. Your job: write C++ that builds a full,
creative, walkable VR **environment** for the user's prompt. This guide is your
contract with the engine. Ground every decision in it so your code actually runs.

## What you are building on top of (the engine, DO NOT touch it)

The runtime `ptv_runtime` is stable, hand-authored C++20 (Vulkan + OpenXR + EnTT
ECS + Jolt physics). Its subsystems are OFF-LIMITS - you may not read, include,
modify, or depend on any of them:

- `Application` (main loop), `Window`, `VulkanContext`, `XrRuntime`, `Renderer`,
  `GltfMeshLoad`, `World`/ECS, `SceneLoader`, `PhysicsWorld`,
  `MechanicRegistry` / `MechanicHost`, `BuiltinMechanics`, `SaveSystem`.
- Any Vulkan, OpenXR, Jolt, GLFW, EnTT, tinygltf, ImGui, volk, or VMA header.

You never edit these. You only *describe* a world; the engine renders and
simulates it. The player already gets VR locomotion (teleport + smooth slide +
snap turn), grab/throw, head/hand tracking, lighting, sky, and physics for free.

## The ONLY header you may include from the engine

```cpp
#include "ptv/api/IWorldBuilder.h"
```

Plus the C++ standard library and GLM (`<glm/...>`) for math. Nothing else.
Any other engine include will be REJECTED by the bounds checker and your task
fails. You may create your own `.h`/`.hpp`/`.cpp` files (own structs, classes,
algorithms) as long as they only include the allowed set.

## How your code is run (offline, safe)

Your builder is compiled into a tiny offline host (`ptv_worldgen`) that links
none of the engine core, run once, and it emits `world.scene.json`. The shipped
game then loads that JSON like any scene. So: you cannot crash the headset, and
you cannot reach engine internals - there is literally nothing dangerous linked
into the process you run in.

## What to implement

Subclass `ptv::world::IWorldBuilder` and register it EXACTLY once:

```cpp
#include "ptv/api/IWorldBuilder.h"
namespace ptv::world {
class MyWorld : public IWorldBuilder {
public:
  void build(WorldContext& w) override {
    // ... author the whole environment here ...
  }
  std::string id() const override { return "generated.world.<project_id>"; }
};
PTV_REGISTER_WORLD_BUILDER("generated.world.<project_id>", MyWorld)
}  // namespace ptv::world
```

The registration macro MUST be inside `namespace ptv::world` with the
UNQUALIFIED class name, and ONLY in a `.cpp` file (never a header).

## The WorldContext API (your building blocks)

All types are in `namespace ptv::world`. `Vec3{x,y,z}`, `Quat{x,y,z,w}`,
`Color{r,g,b}` (0..1), `Transform{position, rotation, scale}` (scale may be
non-uniform; it is baked into geometry for you).

Core (implemented by the host):

- `w.sky(SkyDesc{top, horizon, sun_direction, sun_color, sun_intensity,
  ambient_color, ambient_intensity, fog_density, exposure})` - gradient sky
  PLUS all the global environment/atmosphere knobs. Call once. YOU own every
  one of these values; the engine renders exactly what you set (there is no
  hidden brightness floor). See the rendering contract below.
- `w.ground(GroundDesc{size_m, y, color, grid, collision})` - a floor slab with
  collision so the player stands on it. Call once (a floor is auto-added if you
  forget).
- `w.light(LightDesc{kind:"directional"|"point", position, direction, color,
  intensity, range})` - add as many as you want (up to 8 point lights render).
  `range` (meters) is the point-light falloff radius YOU control; directional
  lights ignore it.
- `w.player_spawn(Vec3 position, Vec3 facing)` - where the player starts. MUST be
  on/above walkable ground and inside the playable area.
- `w.spawn_primitive(PrimitiveDesc{kind:Box|Plane|Cylinder|Sphere|Ramp, transform,
  size, color, collision, tags})` - the workhorse for structure.
- `w.add_mesh(MeshDesc{positions, normals?, indices, transform, color, collision,
  tags})` - ARBITRARY procedural geometry. Build any shape you want in C++
  (towers, arches, terrain, organic forms) by emitting triangles. `normals` may
  be empty (computed for you). `collision: Mesh` makes it solid.
- `w.place_object(ObjectPlacement{asset_id, transform, tags})` - OPTIONAL: anchor
  an existing pipeline-generated prop/character GLB by its asset_id.
- `w.nav_region(NavRegionDesc{id, min, max, floor_y})` - a rectangle of floor
  NPCs may walk on. One per room / corridor / courtyard / street; overlap at
  openings is fine; a ramp or staircase is several regions, one per landing.
- `w.nav_link(NavLinkDesc{from_id, to_id, point, cost_multiplier?,
  bidirectional?})` - a way through between two regions. Put `point` IN the
  opening (the middle of the doorway), because that is the spot NPCs aim at.
- `w.seed()` - a deterministic per-world seed; use it for any randomness so runs
  are reproducible.

### Why navigation is your job

You are the only one who knows this level's layout, because you just built it.
NPCs cannot infer it from geometry, and without a declaration they fall back to
local obstacle avoidance: they feel their way around whatever is directly in
front of them. That is enough for one open arena and useless anywhere with
rooms -- an enemy chasing the player into a building will press against the
wall beside the door and never find the opening.

Declaring it is cheap. A single open arena is one `nav_region` and no links. A
two-room building is two regions and one link at the doorway. The build FAILS
if a link names a region that does not exist, if a link's `point` is outside
both regions it joins, if the player spawn is not inside a region, or if any
region is unreachable from the spawn -- all of which produce a level that looks
right and has NPCs standing still in it.

Header-only convenience builders (in `ptv::world::helpers`, all built on the core
API - use them or ignore them): `building_grid`, `wall_shell`, `path`,
`platform_set`, plus a deterministic `Rng`.

`Collision` values: `None` (visual only), `Box`, `Sphere`, `Mesh`. Use `Box` for
blocky structure, `Mesh` for `add_mesh` geometry the player must not pass
through, `None` for distant decoration.

## Rendering & lighting contract (READ THIS - it decides how bright things look)

The engine is a faithful renderer: it draws EXACTLY what you author and imposes
NO brightness floor. That means a scene is only as visible as YOU make it - a
dark game can be genuinely dark, a bright game bright. It also means a world
lit poorly (near-black albedos + weak/absent lights) will correctly render
near-black. Author lighting on purpose. Full details in
[`../docs/rendering.md`](../docs/rendering.md).

How your values reach the screen:

- Color space: all `Color`/`color` values are LINEAR RGB in `0..1`. The engine
  applies `exposure` -> ACES filmic tone map -> sRGB. A consequence: a surface's
  final brightness is roughly `albedo x (sun + ambient + point lights)`. If the
  albedo is near-black (`< ~0.05`) the surface reads black NO MATTER how bright
  the lights are - so keep visible surfaces around `0.1..0.85` unless you truly
  want black.
- `sky.top` / `sky.horizon`: the gradient dome. `horizon` is ALSO the distance-
  fog color, so distant geometry melts into it - pick a horizon that reads well.
- `sky.sun_direction`: the direction sunlight TRAVELS (points away from the sun,
  downward-ish). It positions the sun in the sky AND shades every surface.
  A near-vertical sun barely lights vertical walls; angle it so it rakes the
  surfaces you care about. Typical `sun_intensity` is `1..4`.
- `sky.ambient_color` + `sky.ambient_intensity`: the fill light that reaches
  surfaces the sun and point lights DON'T (walls facing away, undersides,
  interiors). This is your main lever for baseline visibility. Bright, readable
  world: `ambient_intensity ~0.3..0.6`. Dark/moody world: lower it - but then
  add point lights so focal surfaces still read. `ambient_color` is the lower-
  hemisphere (ground-bounce) tint; the upper hemisphere blends toward `sky.top`.
- `light(...)`: the first `directional` light acts as the sun if you omit a full
  `sky`. Up to **8** `point` lights render, each with smooth falloff to zero at
  its `range`. Place them NEAR the surfaces/focal points you want lit (lamps,
  glows, walls, hero props) - not only high overhead, or only the floor lights.
  `intensity` is a linear multiplier; a handful in the `1..8` range reads better
  than one giant value.
- `sky.fog_density`: `0` = none, `~0.01` subtle, higher = thicker haze toward
  the horizon color.
- `sky.exposure`: global brightness multiplier before tone mapping (`1.0`
  neutral, `>1` brighter, `<1` darker). Use it to dial the whole scene at once.
- `spawn_primitive` `kind` renders as the REAL shape (Sphere, Cylinder, Ramp,
  Plane), not a box - pick the right kind; collision matches the shape.
- The engine adds MSAA, a cheap contact-AO term, and the tone map for polish.
  Don't bake fake shadows into flat dark albedo; let your lights create contrast.

## Minimum requirements for a well-formed environment (MUST satisfy)

These are the non-negotiable minimums for any world you author. They are mood-
aware: "visible" means visible for the intended mood, not "bright" - a horror
crypt can be dim, but the player must still be able to see and traverse it.

1. Call `sky(...)` once with a deliberate `sun_direction` AND deliberate
   `ambient_color` / `ambient_intensity`. Do not leave ambient at accidental
   near-zero unless the design is intentionally near-black.
2. Provide enough light that the player can actually SEE and navigate the main
   play surfaces: the sun, ambient, and/or point lights together must illuminate
   the floor and the structures the player walks among. If the mood is dark, use
   point lights near key surfaces so they still read.
3. Keep albedos (`color` / `color_rgb`) of surfaces the player sees in a visible
   range (roughly `0.1..0.85`) unless a surface is intentionally black. Never
   rely on near-black albedo + a huge light to fake brightness (it renders
   black).
4. Call `ground(...)` with collision and `player_spawn(pos, facing)` with the
   spawn standing ON that ground, clear of geometry, facing something
   interesting.
5. Give walls / buildings / obstacles real collision; leave open, walkable floor
   where the player should move.
5b. Declare that walkable floor with `nav_region(...)`, and every opening
   between parts of it with `nav_link(...)`. One region and no links is the
   right answer for a single open arena.
6. Human scale (player ~1.7 m): room ~6-12 m, plaza ~20-40 m, city block
   ~30-60 m.
7. Real density and composition, not a few boxes: use procedural loops, noise,
   color palettes, verticality, and landmarks specific to the prompt (an Iron
   Man city, a Green Lantern plaza, a bioluminescent alien field). The
   environment is ALL your C++; pickup objects come from the separate asset
   pipeline.

Set `exposure` and per-light `range` as needed to make the above true. You have
complete creative control over the look; these minimums only ensure the result
is a coherent, visible, walkable place.

## The scene.json contract you ultimately produce (for reference)

The host turns your calls into entities like these (you don't write JSON; this is
just what the runtime consumes):

- Primitive entity: `{ transform, primitive:{kind,size,color_rgb}, physics:{kind,
  collision_shape}, tags }`.
- Inline mesh entity: `{ transform, mesh_inline:{positions,indices,color_rgb},
  physics:{collision_shape:"mesh"}, tags }`.
- Object entity: `{ transform, mesh:{asset_id,glb_path}, physics, tags }`.
- Plus top-level `sky` (`top_color`, `horizon_color`, `sun_direction`,
  `sun_color`, `sun_intensity`, `ambient_color`, `ambient_intensity`,
  `fog_density`, `exposure`), `lights` (each with `color_rgb`, `intensity`,
  `direction`, and point `range`), and `player.spawn`.

The runtime renders primitives/inline-meshes/GLBs, builds Jolt collision from the
physics block, and drops the player at `player.spawn`. That is the whole surface
you target.
