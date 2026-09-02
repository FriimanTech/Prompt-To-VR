# World generation (agentic VR environments)

The VR **environment** is authored entirely in C++ by a single ChatGPT "world
architect" agent, compiled and run **offline**, and emitted as `scene.json` that
the engine loads like any scene. The environment is **no longer** a Tripo GLB;
Tripo still generates the discrete **objects** the player interacts with
(characters, props, fx), which are merged in on top of the environment.

## Pipeline

```
build_spec -> asset_manifest -> npc_plan -> mechanic_plan -> scene_spec
  -> reference_images -> tripo_models -> asset_rigging -> asset_animation
  -> asset_normalization -> asset_contract
  -> code_generation (mechanics DAG)
  -> world_generation (this stage)
  -> asset_optimization (decimation + LOD chain)
  -> scene_compile (merge world + gameplay)
  -> cmake_build | quest_build -> engine_promotion
```

`world_generation` (`apps/orchestrator/ptv_orchestrator/pipeline/stage_world_generation.py`):

1. Builds the **world architect** agent (`world_agent.py`) via the OpenAI Agents
   SDK: model `world_model` (default `gpt-5.5`), `ModelSettings(reasoning=
   Reasoning(effort=world_reasoning_effort))` (default `high`), `WebSearchTool`
   enabled, a persistent `Session`, and `max_turns = world_agent_max_turns`.
2. Hands it `engine/world_author_guide.md` (the "understand the engine" input),
   the BuildSpec brief, and the list of available object asset ids.
3. Writes the agent's bounded C++ into `engine/generated/world/<project_id>/`.
4. Compiles the offline `ptv_worldgen` host and, on compile failure, feeds the
   errors back to the same session to fix; retries up to `world_max_repair_rounds`.
5. Runs the host to emit `projects/<id>/world.scene.json` and validates it
   (ground present, spawn on ground, sane entity count); refines on invalid.
6. If the budget is exhausted, emits a deterministic **fallback world** so the
   project still boots.

`scene_compile` then merges `world.scene.json` (environment entities + sky +
lights + spawn) into the entry-point scene, with gameplay entities on top.

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
`nav_link` (declare the walkable space), `seed()`. Header-only
convenience builders live in `ptv::world::helpers` (`building_grid`, `wall_shell`,
`path`, `platform_set`).

`nav_region` / `nav_link` exist because the builder is the only thing that
knows this level's layout -- it just created it. NPCs cannot infer it, and
without a declaration they fall back to local obstacle avoidance, which
cannot find a doorway. Reconstructing the topology from finished geometry
(the Recast approach) would rebuild something the author already knew. A
deterministic validator then checks the declaration against itself before the
world is accepted. See [navigation.md](navigation.md).

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

- `world_model` (default `gpt-5.5`)
- `world_reasoning_effort` (default `high`; one of none|minimal|low|medium|high|xhigh;
  keep at `high`/`xhigh` — `medium` risks first-try compile failures in the generated C++)
- `world_max_repair_rounds` (default 6)
- `world_agent_max_turns` (default 80)

## Deferred / follow-ups

- Renderer: a dedicated lights UBO + a separate gradient-skybox pass. Today the
  framebuffer clears to the sky horizon color and the fragment shader uses a
  fixed 3-point + fog model; scene lights are parsed and available.
- Locomotion: a full Jolt `CharacterVirtual` player capsule so **smooth**
  locomotion is blocked by walls. Today teleport already lands on real generated
  geometry via physics raycast; smooth locomotion is free-fly on XZ.
- GPU texture / PBR decode for Tripo object GLBs (still vertex-color).
