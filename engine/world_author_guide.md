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

- `w.sky(SkyDesc{top, horizon, sun_direction, sun_color, sun_intensity})` -
  gradient sky. Call once.
- `w.ground(GroundDesc{size_m, y, color, grid, collision})` - a floor slab with
  collision so the player stands on it. Call once (a floor is auto-added if you
  forget).
- `w.light(LightDesc{kind:"directional"|"point", position, direction, color,
  intensity})` - add as many as you want.
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
- `w.seed()` - a deterministic per-world seed; use it for any randomness so runs
  are reproducible.

Header-only convenience builders (in `ptv::world::helpers`, all built on the core
API - use them or ignore them): `building_grid`, `wall_shell`, `path`,
`platform_set`, plus a deterministic `Rng`.

`Collision` values: `None` (visual only), `Box`, `Sphere`, `Mesh`. Use `Box` for
blocky structure, `Mesh` for `add_mesh` geometry the player must not pass
through, `None` for distant decoration.

## Requirements for a good, walkable world

1. Always establish `sky`, at least one `light`, a `ground` with collision, and a
   `player_spawn` standing on that ground.
2. Give walls/buildings/obstacles real collision so the player can't walk through
   them; leave open, walkable floor where the player should move.
3. Keep the player spawn clear (not inside geometry) and facing something
   interesting.
4. Scale to human proportions (player height ~1.7 m). A room ~6-12 m, a plaza
   ~20-40 m, a city block ~30-60 m.
5. Be creative and specific to the prompt (an Iron Man city, a Green Lantern
   plaza, a bioluminescent alien field) - use procedural loops, noise, color
   palettes, verticality. The environment is ALL your C++; objects the player
   picks up come from the separate asset pipeline.

## The scene.json contract you ultimately produce (for reference)

The host turns your calls into entities like these (you don't write JSON; this is
just what the runtime consumes):

- Primitive entity: `{ transform, primitive:{kind,size,color_rgb}, physics:{kind,
  collision_shape}, tags }`.
- Inline mesh entity: `{ transform, mesh_inline:{positions,indices,color_rgb},
  physics:{collision_shape:"mesh"}, tags }`.
- Object entity: `{ transform, mesh:{asset_id,glb_path}, physics, tags }`.
- Plus top-level `sky`, `lights`, `player.spawn`.

The runtime renders primitives/inline-meshes/GLBs, builds Jolt collision from the
physics block, and drops the player at `player.spawn`. That is the whole surface
you target.
