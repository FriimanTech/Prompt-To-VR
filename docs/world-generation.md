# World generation (agentic VR environments)

The VR **environment** is authored entirely in C++ by a single ChatGPT "world
architect" agent, compiled and run **offline**, and emitted as `scene.json` that
the engine loads like any scene. The environment is **no longer** a Tripo GLB;
Tripo still generates the discrete **objects** the player interacts with
(characters, props, fx), which are merged in on top of the environment.

## Pipeline

```
build_spec -> asset_manifest (no environment GLB) -> mechanic_plan -> scene_spec
  -> reference_images -> tripo_models -> asset_normalization
  -> code_generation (mechanics DAG)
  -> world_generation (this stage)
  -> scene_compile (merge world + gameplay) -> cmake_build -> engine_promotion
```

`world_generation` (`apps/orchestrator/ptv_orchestrator/pipeline/stage_world_generation.py`):

1. Builds the **world architect** agent (`world_agent.py`) via the OpenAI Agents
   SDK: model `world_model` (default `gpt-5.5`), `ModelSettings(reasoning=
   Reasoning(effort=world_reasoning_effort))` (default `xhigh`), `WebSearchTool`
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
procedural triangles), `place_object` (anchor a prop GLB), `seed()`. Header-only
convenience builders live in `ptv::world::helpers` (`building_grid`, `wall_shell`,
`path`, `platform_set`).

## The offline host (`engine/worldgen/`)

- Standalone CMake project (its own `vcpkg.json`: nlohmann-json + glm). Configure
  with `-DPTV_WORLD_PROJECT_ID=<id>` to compile that project's world sources; the
  stage builds it into `projects/<id>/builds/worldgen`.
- `ptv_worldgen --out <scene.json> [--builder <id>] [--seed <n>] [--assets <map.json>]`.
- Always ships a `builtin.world.fallback` builder (the deterministic fallback).

## scene.json additions (version 2)

- `sky`: `{ top_color, horizon_color, sun_direction, sun_color, sun_intensity }`.
- Entities may carry `primitive: { kind, size, color_rgb }` or
  `mesh_inline: { positions, normals?, indices, color_rgb }` in addition to the
  existing GLB `mesh`. Physics `collision_shape` may be `box|sphere|mesh|none`.
- Lights carry an optional `direction`.

Runtime support: `SceneLoader` parses these into `CPrimitive` / `CInlineMesh`
components + `SkyGradient`; `Renderer` draws primitives (colored non-uniform
boxes) and uploaded inline meshes; `PhysicsWorld` builds non-uniform boxes and
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
- `world_reasoning_effort` (default `xhigh`; one of none|minimal|low|medium|high|xhigh)
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
