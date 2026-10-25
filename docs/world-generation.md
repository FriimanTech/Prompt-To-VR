# World generation (agentic VR environments)

Every VR **environment** is authored in C++ by a ChatGPT "world architect"
agent, compiled and run **offline**, and emitted as a `scene.json` the engine
loads like any scene. The environment is **no longer** a Tripo GLB; Tripo still
generates the discrete **objects** the player interacts with (characters,
props, fx), which are merged in on top of the environment.

**One environment per scene, not one per project.** A game whose BuildSpec has
a hub, three missions and a boss arena gets five environments, generated in
parallel. The single-environment version of this stage was the reason a
six-scene game shipped one real place and five scenes of gameplay entities
floating in an empty void — and nothing reported it, because one world had
been produced successfully.

An image-to-3D asset is not an alternative for this. Tripo generates a mesh
seen from outside; a room has to be entered, so its walls face inward, it needs
a floor that holds a player, doorways that are actual holes, and walkable space
the navigation system can route through. None of that is recoverable from a
single reference image.

## Pipeline

```
build_spec -> asset_manifest -> npc_plan -> mechanic_plan -> scene_spec
  -> reference_images -> tripo_models -> asset_rigging -> asset_animation
  -> asset_normalization -> asset_contract
  -> world_generation (this stage)
  -> code_generation (mechanics DAG)
  -> asset_optimization (decimation + LOD chain)
  -> scene_compile (merge world + gameplay, per scene)
  -> cmake_build | quest_build -> engine_promotion
```

`world_generation` runs **before** `code_generation`, which is the reverse of
the original order. Gameplay code acts on the worlds: a mechanic that opens a
door or routes an NPC through a building needs the ids the world architect
actually declared, and nobody knows them until this stage has run. Generated
C++ used to be written first, so the codegen agent had to invent plausible
names for objects that did not exist yet — and an invented id is not a compile
error, it resolves to nothing at runtime and the mechanic silently does
nothing. Nothing points the other way: a world builder links no ECS and never
calls a mechanic, so it has nothing to learn from gameplay code.

### Which environments get generated

The scene list IS the environment list. `_environment_jobs` walks
`BuildSpec.scenes` and skips a scene that names `reuse_environment_from` (it
shares a sibling's environment; generating it again would pay for a second
xhigh-reasoning agent run to produce the same place twice). Past
`world_max_environments` the list is capped, and the cap always keeps the entry
point — a game that drops its first scene has nowhere to start.

### Per environment

Each environment runs the same loop, and **everything it touches is its own**:

| Per-environment | Value |
| --- | --- |
| Source directory | `engine/generated/world/<project_id>/<scene_id>/` |
| Builder id | `generated.world.<project_id>.<scene_id>` |
| C++ namespace | `ptv::world::ptv_world_<scene_id>` |
| Build tree | `projects/<id>/builds/worldgen/<scene_id>/` |
| Agent session | `ptv-world-<project_id>-<scene_id>` |
| Output | `projects/<id>/world.<scene_id>.scene.json` |
| Seed | derived from `<project_id>/<scene_id>` |

That isolation is the whole reason parallel generation is safe. Sharing one
build would mean sharing a translation-unit set: two agents fighting over
identical filenames, duplicate helper types colliding at namespace scope, and a
syntax error in the head interior failing the main world's compile and spending
its repair budget on someone else's mistake. `PTV_WORLD_ENV_ID` narrows the
CMake glob to one subdirectory to enforce it. Separate seeds keep two places in
one game from coming out with identical procedural variation.

It is affordable because `ptv_worldgen` links almost nothing — nlohmann_json
plus optional glm, with no Vulkan, OpenXR, Jolt, renderer or ECS — so N
configure-and-builds in parallel are cheap.

The steps, per environment:

1. Build the **world architect** agent (`world_agent.py`) via the OpenAI Agents
   SDK: model `world_model` (default `gpt-5.6-sol`), `ModelSettings(reasoning=
   Reasoning(effort=world_reasoning_effort))` (default `high`), `WebSearchTool`
   enabled, a per-environment `Session`, `max_turns = world_agent_max_turns`.
2. Hand it `engine/world_author_guide.md` (the "understand the engine" input),
   the BuildSpec brief, the available object asset ids, and a **per-environment
   brief** naming which scene it is authoring, that scene's role, its exits, and
   — for a nested environment — its host and authoring scale.
3. Write the bounded C++ into that environment's own directory.
4. Compile that environment's own `ptv_worldgen`; on failure feed the errors
   back to the same session, up to `world_max_repair_rounds`.
5. Run the host to emit `world.<scene_id>.scene.json` and validate it.
6. On an exhausted budget, emit a deterministic **fallback world for that
   environment only** — one bad place must not take a whole game down. The
   stage still succeeds, and its message names which environment fell back so
   it can be regenerated alone.

A single-environment project keeps the historical layout exactly: one
directory, `generated.world.<project_id>`, no namespace requirement, and
`world.scene.json`. Nothing about an existing project changes.

### The architects do not talk to each other

Each gets its whole contract up front in a deterministic brief, and
cross-environment consistency is checked deterministically afterwards. This is
deliberate: a shared brief cannot disagree with itself, whereas negotiation
between parallel agents can. Each environment carries its own nav graph, so
there is no shared structure for one to corrupt for another.

`scene_compile` then merges each scene's own `world.<scene_id>.scene.json`
(environment entities + sky + lights + spawn) into that scene, with gameplay
entities on top, recording `environment_builder_id` so a bad room can be traced
to the agent that authored it. A scene reusing a sibling's environment resolves
through `reuse_environment_from` — `needs_unique_environment: false` on its own
never said WHICH environment was being borrowed.

### Nested environments (interiors)

A scene with `environment_mode: nested` is not merged into a scene of its own.
It is **loaded at runtime into a live world**, mounted on a host entity's joint
and shrunk to fit — the inside of a character's skull, a dollhouse, a snow
globe. The outer world stays loaded and keeps running, which is the entire
point: the host walks around living its own life while the player inside
notices nothing, because they and the interior hang off the same anchor.

`ApplySceneToWorld` takes a `SceneNesting` for this rather than there being a
second loader — a parallel implementation would drift from this one the first
time a component was added to a scene document. Two things change on the way
in:

- **Ids are namespaced** (`skull_interior/front_door`). Two world architects
  will both name a door `front_door`, and ids are persistence keys, so a clash
  does not fail loudly — it silently restores one door's saved state onto the
  other. Attachments and interactives inside the interior resolve through the
  same scoping, so an interior-internal reference finds the interior's own copy
  rather than the outer scene's same-named entity.
- **Transforms are re-based** through the mount matrix, and the uniform fit
  scale is baked into each entity's `Transform::scale` rather than left in the
  matrix. The attachment system that carries the interior each frame rewrites
  position and rotation but deliberately never touches scale, so anything left
  in the matrix would be lost on the first frame.

Every nested entity is attached to the host anchor and tagged
`nested:<scene_id>`. Reusing the attachment system means the interior rides the
**animated** joint for free and its colliders are already moved kinematically,
so the miniature player can stand on a floor that is travelling through the
world. The tag is both how the loader's output is identified and how unloading
finds it later.

The interior is authored at **human scale** and shrunk on load, never authored
small: the engine measures what was actually built and derives the factor. A
level laid out inside a 22 cm box produces geometry and nav regions too
degenerate to walk on. `builtin.head_interior` names the interior's scene id
explicitly — the `world.<scene_id>` convention cannot apply, because a nested
interior has no scene of its own to be paired with.

## The generated-code boundary (safety)

Generated world C++ can never touch the engine core:

- It may `#include` only `ptv/api/IWorldBuilder.h`, the C++ standard library, and
  `<glm/...>`. Enforced by `bounds.py` (`validate_world_includes`) and
  `engine/tools/check_generated_bounds.py`.
- It lives only in `engine/generated/world/<project_id>/`.
- It runs offline in `ptv_worldgen`, which links **no** Vulkan/OpenXR/Jolt/
  renderer/ECS. There is nothing dangerous in that process to reach, and bad
  codegen fails on the dev machine, never in the headset.

## The authoring surface

`engine/runtime/include/ptv/api/IWorldBuilder.h` defines `IWorldBuilder` +
`WorldContext`. A builder subclasses `IWorldBuilder`, implements `build(WorldContext&)`,
and registers with `PTV_REGISTER_WORLD_BUILDER("generated.world.<id>", Type)`
inside `namespace ptv::world` in a `.cpp`.

`WorldContext` (pure data builder): `sky`, `ground`, `light`, `player_spawn`,
`spawn_primitive` (box/plane/cylinder/sphere/ramp), `add_mesh` (arbitrary
procedural triangles), `place_object` (anchor a prop GLB), `nav_region` /
`nav_link` (declare the walkable space), `npc_spawn_zone` (declare where NPCs
enter during play), `npc_anchor` (declare where an NPC placed at load
stands), `seed()`. Header-only
convenience builders live in `ptv::world::helpers` (`building_grid`, `wall_shell`,
`path`, `platform_set`).

`nav_region` / `nav_link` exist because the builder is the only thing that
knows this level's layout -- it just created it. NPCs cannot infer it, and
without a declaration they fall back to local obstacle avoidance, which
cannot find a doorway. Reconstructing the topology from finished geometry
(the Recast approach) would rebuild something the author already knew. A
deterministic validator then checks the declaration against itself before the
world is accepted. See [navigation.md](navigation.md).

`npc_spawn_zone(NpcSpawnZoneDesc{id, position, radius, tags})` marks a gate,
tunnel mouth or ridge where arriving NPCs appear. When the NPC plan has
encounters, the architect's brief asks for at least three zones, 12 m or more
from the player spawn, on walkable floor, and for 3 m around the spawn to be
kept clear for companions. Zones are written to the world's `npc_spawn_zones`
and copied into `scene.json` for the encounter director. A world with no zones
still works: the scene compiler derives zones from the nav regions. See
[npc-and-ai.md](npc-and-ai.md).

`npc_anchor(NpcAnchorDesc{id, position, facing, asset_id, tags})` marks where
an NPC that is PLACED AT LOAD stands: a guard by the gate, a vendor behind the
counter, a turret on the wall. The brief asks for one per copy whenever the
NPC plan places NPCs that are not companions. Anchors are written to the
world's `npc_anchors`, validated to sit inside a declared `nav_region` (as
spawn zones now are too), and copied into `scene.json`, where the scene
compiler puts each matching NPC on one. Companions that follow the player
need none; they start beside the spawn.

**Who places NPCs.** The world architect never spawns, names or counts NPCs:
it declares SPACE (walkable regions, entrances, posts) because it is the only
stage that knows the layout it just built. The NPC plan decides who exists and
how many; the scene compiler joins the two and validates every placed NPC
against the merged world (walkable floor, floor height + foot offset); the
runtime encounter director spawns arrivals into the zones. See
[npc-and-ai.md](npc-and-ai.md#placing-npcs-in-the-world).

The gameplay layer the SceneSpec composed (weapons, panels, companions) is
authored around the origin. The scene compiler moves it rigidly to the world's
`player_spawn`, position and yaw together, and nudges anything that lands
inside a static collider out of it.

`MeshDesc` (for `add_mesh`) uses FLAT float buffers: `positions` and `normals`
are `std::vector<float>` xyz triples (`{x0,y0,z0, x1,y1,z1, ...}`), NOT
`std::vector<Vec3>`; `indices` is a `std::vector<std::uint32_t>` triangle list.
`WORLD_BUILDER_SYSTEM` teaches this layout so the architect gets it right on the
first attempt.

The environment look is **fully author-controlled** — the engine renders exactly
what the builder specifies and imposes no brightness floor. `SkyDesc` carries the
whole global look: `top` / `horizon`, `sun_direction` / `sun_color` /
`sun_intensity`, `ambient_color` / `ambient_intensity` (baseline fill),
`fog_density`, and `exposure` (global pre-tonemap multiplier). `LightDesc`
carries a per-point-light `range` (authored falloff, not derived). See the
"Rendering & lighting contract" in
[`../engine/world_author_guide.md`](../engine/world_author_guide.md) for how these
values read on screen.

## The offline host (`engine/worldgen/`)

- Standalone CMake project (its own `vcpkg.json`: nlohmann-json + glm). Configure
  with `-DPTV_WORLD_PROJECT_ID=<id>` to compile that project's world sources; the
  stage builds it into `projects/<id>/builds/worldgen`.
- `ptv_worldgen --out <scene.json> [--builder <id>] [--seed <n>] [--assets <map.json>]`.
- Always ships a `builtin.world.fallback` builder (the deterministic fallback).

## scene.json additions (version 2)

- `sky`: `{ top_color, horizon_color, sun_direction, sun_color, sun_intensity,
  ambient_color, ambient_intensity, fog_density, exposure }` (`ambient_intensity`
  is also mirrored at the top level for back-compat).
- Entities may carry `primitive: { kind, size, color_rgb }` or
  `mesh_inline: { positions, normals?, indices, color_rgb }` in addition to the
  existing GLB `mesh`. Physics `collision_shape` may be `box|sphere|mesh|none`.
- Lights carry an optional `direction` and, for point lights, an optional
  `range` (falloff radius in meters; the runtime uses 20 m when unset).
- `navigation`: `{ regions: [{id, min, max, floor_y}], links: [{from, to,
  point, cost_multiplier?, bidirectional?}] }`. Omitted entirely when the
  builder declared none, which the runtime reads as "steering only" rather
  than "nowhere is walkable". Carried through the scene-compile merge and
  loaded into `ptv::nav::NavGraph`.
- A GLB `mesh` may carry `sockets`: named local transforms in metres, e.g.
  `{"muzzle": {"position": [0, 0.02, -0.18], "rotation": [0, 0, 0, 1]}}`.
  Written by the scene compiler from `asset_contract.json` and read by
  gameplay through `ctx.sockets`. See
  [architecture.md](architecture.md#the-socket-contract). World builders do
  not author sockets; `place_object` emits meshes without them, and the scene
  compiler fills them in from the asset's contract during the merge.

Runtime support: `SceneLoader` parses these into `CPrimitive` / `CInlineMesh`
components + `SkyGradient` (incl. the ambient/fog/exposure knobs) and per-light
`range`; `Renderer` draws primitives as their real shapes (box/sphere/cylinder/
ramp/plane) and uploaded inline meshes, lit by the scene-driven lighting UBO
(see [`rendering.md`](rendering.md)); `PhysicsWorld` builds non-uniform boxes and
Jolt triangle-mesh shapes; teleport raycasts against the physics world so the
player lands on the generated ground/platforms/terrain at any height.

## Validating without API spend

A hand-authored sample world exercises the whole path with no LLM:

```
cmake -S engine/worldgen -B engine/worldgen/build -DPTV_WORLDGEN_SAMPLES=ON \
  -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
cmake --build engine/worldgen/build --config Release --target ptv_worldgen
engine/worldgen/build/Release/ptv_worldgen.exe \
  --out world.scene.json --builder sample.starter_city
```

Source: `engine/worldgen/samples/starter_city.cpp`.

## Settings

`apps/orchestrator/ptv_orchestrator/settings.py`:

- `world_model` (default `gpt-5.6-sol`)
- `world_reasoning_effort` (default `high`; gpt-5.6 accepts none|low|medium|high|xhigh|max;
  keep at `high`/`xhigh` — `medium` risks first-try compile failures in the generated C++)
- `world_max_repair_rounds` (default 6, **per environment**)
- `world_agent_max_turns` (default 80)
- `world_max_parallel_workers` (default 3) — how many architects run at once.
  Each is an independent agent plus a CMake configure-and-build, so this trades
  wall-clock time against local CPU and API concurrency limits.
- `world_max_environments` (default 12) — hard cap on environments per project.
  Every one is a full agent run with its own compile loop, so this is the
  stage's cost ceiling.
- `world_max_interactives_per_env` (default 512) — cap on declared interactives
  in one environment. Each carries runtime bookkeeping, and Meta's guidance for
  a busy Quest 3 scene is roughly 200–300 draw calls, so this is a performance
  bound as much as a correctness one.

## Deferred / follow-ups

- Renderer: a dedicated lights UBO + a separate gradient-skybox pass. Today the
  framebuffer clears to the sky horizon color and the fragment shader uses a
  fixed 3-point + fog model; scene lights are parsed and available.
- Locomotion: a full Jolt `CharacterVirtual` player capsule so **smooth**
  locomotion is blocked by walls. Today teleport already lands on real generated
  geometry via physics raycast; smooth locomotion is free-fly on XZ.
- GPU texture / PBR decode for Tripo object GLBs (still vertex-color).
