# AGENTS.md

Authoritative agent instructions for this repository. These rules apply to every
AI-driven change in this codebase, in any IDE, CLI, or CI.

## Mission

Take a detailed VR game prompt and produce a **playable, native C++ VR
executable** on Windows PC VR (Meta Quest 3 via Quest Link) through a local
orchestrator and a reusable Vulkan + OpenXR engine core.

## Hard constraints (never violate)

1. Runtime is a native C++ executable. **No Unity, Unreal, Godot, WebXR,
   Three.js, Babylon.js, or browser-rendered VR.**
2. Rendering is Vulkan. XR is OpenXR. Build system is CMake. Dependencies are
   managed with vcpkg manifest mode.
3. Local-first only. **No Supabase, S3, Temporal, cloud auth/storage,
   multiplayer, speech-to-text, marketplace publishing, Steam packaging** in
   this version. Quest standalone packaging IS supported: the `quest`
   target_device builds a local debug APK (Gradle + NDK) that is sideloaded to
   the headset over `adb` for local testing only. Meta Horizon **Store**
   submission / release signing remains out of scope.
4. Schema-first development. Once a schema exists in `packages/schemas`,
   downstream stages MUST consume the structured artifact, not the raw prompt.
5. Reuse built-in VR mechanics. The engine ships reusable, data-driven
   modules under `engine/runtime/src/mechanics/builtin/<mechanic>/`
   (teleport, head-relative and controller-relative slide, snap turn, grab,
   throw, recoil, hand_pose). Each is a real `IMechanic` written against the same public
   API generated code uses — `InputSnapshot`, `World` and `MechanicContext`
   (locomotion / physics / debug-draw / hand / transform-fx / haptics / socket
   / skeleton services, all engine-clamped) —
   so a `builtin+config_code_edit` task can copy one and edit it into a NEW
   generated mechanic. Generated code adds mechanics; it does not
   reimplement basic VR behavior, and it never modifies a built-in.
   `builtin.smooth_locomotion` has been REMOVED; use the explicit
   `builtin.locomotion.slide.*` ids. The player's own hands are rigged
   entities posed by `builtin.hand_pose`, which the engine binds
   automatically — generated code must never re-implement finger curl, and
   must never couple a weapon to the hand (both read the same
   `input.hands[h].trigger` and stay in sync on their own).
6. Generated C++ (`.cpp`, `.h`, `.hpp`) is confined to
   `engine/generated/{mechanics,ai,dialogue,world}/`. It MUST NOT modify the
   renderer, XR session loop, platform layer, memory core, or base ECS.
   Generated headers are allowed for same-task use and for including a
   shared header owned by a declared `depends_on` task (same project
   directory, built first — e.g. a weapon profile including
   `shooting_hub_shared.h`); both via bare-filename `#include "foo.h"`
   includes. Registration macros
   (`PTV_REGISTER_MECHANIC`, etc.) must only appear in `.cpp` files.
   Include violations cause the CodeTask to fail loudly (not silently
   strip).
   The VR **environment** is authored in C++ by a single world-architect
   agent under `engine/generated/world/` implementing `IWorldBuilder`; that
   surface has a tighter whitelist (only `ptv/api/IWorldBuilder.h` + std +
   glm) and is compiled + run OFFLINE in the `ptv_worldgen` host (no
   Vulkan/OpenXR/Jolt/renderer/ECS linked), emitting `scene.json`. The
   environment is NOT a Tripo GLB.
7. Use Claude for the hardest gameplay planning and hardest C++ generation
   (`claude-opus-4-7`). Use `claude-sonnet-4-6` for lighter work. If Claude
   is unavailable (missing key or API error) the pipeline transparently
   falls back to OpenAI via the shared `LlmClient`
   (`OPENAI_HARD_MODEL=gpt-5`, `OPENAI_LIGHT_MODEL=gpt-5-mini`).
8. Tripo image-to-3D calls MUST enable textured, PBR output with image-aligned
   textures (`texture=True`, `pbr=True`, `texture_alignment="original_image"`)
   and MUST target the **V3** API; V2 is retired 2026-11-01.
9. Verify provider SDK signatures and model names with live docs before
   changing provider client code.
10. When code structure changes, update `/docs/` in the same change.

## Repository map

- `apps/web` – React + Vite + TypeScript control panel.
- `apps/orchestrator` – FastAPI backend that runs the pipeline.
- `engine/runtime` – **hand-authored** reusable C++ VR engine. Stable.
  Platform-split under `src/platform/{desktop,android}`; a `ptv::IAssetSource`
  seam feeds assets from the filesystem (PC VR) or the APK (Quest).
- `engine/android` – Gradle + NDK template the `quest_build` stage
  instantiates per project to package a standalone Quest APK. See
  `docs/quest-build.md`.
- `engine/generated/{mechanics,ai,dialogue,world}` – the **only** directories
  where LLM-produced C++ lives (`world/` = VR environment builders).
- `engine/worldgen` – offline `ptv_worldgen` host + `WorldContext` that runs
  generated `IWorldBuilder` code and emits `scene.json`. Links no engine core.
- `engine/tools` – scene compiler, GLB validator, bounds checker.
- `packages/schemas` – Pydantic v2 models + JSON Schemas (single source of
  truth for BuildSpec, AssetManifest, MechanicPlan, SceneSpec, CodeTask,
  ProjectState, ArtifactMetadata, AcceptanceTestPlan).
- `packages/provider-clients` – Anthropic, Google GenAI, Tripo wrappers.
- `projects/<project_id>/` – per-project local artifact workspace.
- `docs/` – user-facing docs. Must stay current.
- `samples/starter-room/` – hand-authored sample scene that boots without any
  LLM calls, used to validate XR + Vulkan before spending API credits.

## Generation pipeline (order is fixed)

1. Prompt → **BuildSpec** (Claude Opus, structured output).
2. BuildSpec → **AssetManifest** (Claude Sonnet, structured output). Only
   OBJECTS (characters, props, fx, ui) — the environment is not a GLB.
3. AssetManifest + BuildSpec → **MechanicPlan** (OpenAI Agents SDK agent,
   `gpt-5.5`, no tools). Each mechanic is classified `builtin`,
   `builtin+config`, `builtin+config_code_edit` (copy a built-in's source and
   edit it into a NEW generated mechanic — never replaces the built-in), or
   `generate`. The agent also assigns a controller `binding`
   (control + hand) to every input-driven mechanic; a deterministic
   validator normalizes face-button hands (sharing a control is allowed,
   not a conflict). BuildSpec labels each mechanic `regular` or
   `hub` (a shared foundation like a shooting hub).
4. All above → **SceneSpec**, one per scene (OpenAI Agents SDK agent,
   `gpt-5.5`, no tools) with a deterministic validate-and-repair loop
   (asset coverage, environment presence, valid mechanic-binding ids);
   problems are fed back into the same agent session up to
   `scene_spec_max_repair_rounds`. Scenes fan out across
   `scene_spec_max_parallel_workers` agents.
5. Reference images for each object `AssetItem` → Gemini / Imagen.
6. Reference images → Tripo image-to-3D (textured, PBR). The generation
   `task_id` is recorded as `assets/<id>/tripo_task.json` so rigging can
   reference it instead of re-uploading the mesh.
6b. **Auto-rigging** (`asset_rigging`): for every `AssetItem` with
   `rigged: true`, call `POST /v3/animations/rig-check` (free) and, only if
   it answers `riggable: true`, `POST /v3/animations/rig` (~30 credits).
   Both endpoints take a single `input` field — a task_id, a `file_token`,
   or a URL — NOT V2's `original_model_task_id`. The rig `model` is derived
   from the returned `rig_type`: `v1.0-20240301` for `biped`,
   `v2.5-20260210` for every non-humanoid shape (`quadruped`, `hexapod`,
   `octopod`, `avian`, `serpentine`, `aquatic`). `spec="tripo"` and
   `out_format="glb"` always — Mixamo-spec skeletons make every follow-on
   `/animations/retarget` call fail with Tripo error 1004. Output lands as
   `model_rigged.glb` beside the
   original, and `asset_rig.json` records the outcome so a Resume never
   re-bills. Runs BEFORE normalization so the rigged mesh is what every
   later stage validates, sockets, decimates and compiles. Every rig type
   Tripo offers describes a WHOLE creature, so props, weapons and body
   parts are not riggable — the free pre-check is what keeps that from
   costing anything.
6c. **Animation retarget** (`asset_animation`): for every successfully rigged
   asset whose `AssetItem.animations` is non-empty, `POST
   /v3/animations/retarget` (~20 credits PER animation, ≤5 per call).
   `AssetItem.animations` holds semantic INTENTS (`idle`, `walk`, `run`,
   `attack`, `shoot`, `hurt`, `die`, `jump`, `turn`, `climb`, `dance`,
   `wave`, `sit`), never raw preset ids — which presets exist depends on the
   `rig_type` and rig `model`, neither known until rig-check has run, and
   `resolve_animations` maps intent→preset from what `asset_rig.json`
   recorded. A creature rig has only locomotion, so `walk` and `run` share
   one preset (billed once) and other intents are dropped loudly.
   `input` MUST be a rig `task_id`; there is no upload fallback.
   `animate_in_place=True` always — a clip with baked root motion would
   translate the mesh on top of the engine moving the entity, so the
   character covers double the ground and snaps back on every loop.
   `export_with_geometry=True` because the runtime reads mesh, skin and
   clips from one GLB. Output is `model_animated.glb`; `asset_animation.json`
   records intent → the clip name Tripo ACTUALLY produced (read back out of
   the GLB, never assumed).
7. Assets **validated** (`asset_normalization`): GLB header/JSON check plus
   scale / collision metadata. This stage does NOT modify geometry.
7b. **Asset contract** (`asset_contract`): derives each asset's named
   **sockets** (`muzzle`, `grip`) from its geometry with the deterministic
   `ptv_glb_sockets` tool (principal-axis PCA + mass asymmetry — no model
   calls) and writes `asset_contract.json`. Positions are stored in UNIT
   space (normalised by the longest-axis extent) so they survive the LOD
   chain; the scene compiler multiplies by `target_scale_meters`, exactly as
   it already does for `bounds_radius_m`. Runs BEFORE codegen because the
   codegen agent needs the socket NAMES in context — it writes
   `ctx.sockets->socket_world(self, "muzzle")` and must never hard-code an
   offset it cannot know. Which sockets to derive comes from
   `AssetItem.socket_hints`.
8. CodeTasks from MechanicPlan → Claude Opus → C++ files (`.cpp` + optional
   `.h`/`.hpp` headers) in `engine/generated/{mechanics,ai,dialogue}/`.
   After all tasks are written, a **compile-verify + repair loop** builds
   the `ptv_runtime` target and feeds any compiler errors back to the
   owning task's agent to regenerate (up to `codegen_max_repair_rounds`);
   the stage fails loudly if the generated C++ still does not compile
   (`codegen_verify_compile`, default on).
8b. **World generation**: a single world-architect agent (OpenAI Agents SDK,
   `gpt-5.5` @ reasoning `xhigh`, `WebSearchTool`) writes `IWorldBuilder` C++
   in `engine/generated/world/<project_id>/`, compiled + run offline in
   `ptv_worldgen` under a compile-and-repair loop → `world.scene.json`
   (deterministic fallback world if the budget is exhausted).
8c. **Asset optimization** (`asset_optimization`): derives a device
   `PerformanceProfile` (tier + triangle/draw-call budgets) from
   `target_device` and how much GLB content the project has, allocates a
   per-asset triangle budget weighted by importance, and runs
   `ptv_glb_optimize` (meshoptimizer) to emit an LOD chain
   `<asset_id>_lod0..3.glb`. Raw Tripo output is ~500k triangles per asset;
   Meta's whole-frame budget for Quest 3 is 1.3M-1.8M, so this stage is what
   makes any non-trivial project runnable. There is **no cap on asset count** —
   more content just means a smaller per-asset budget (never below a quality
   floor) plus more aggressive runtime culling/LOD. See `docs/rendering.md`.
   **Rigged assets** take a skin-preserving path: animation clips are copied
   through verbatim (they target nodes, so decimation cannot invalidate
   them — but the writer has to actually emit them, and for a while it did
   not, which silently left every LOD un-animatable),
   bindings ride inside the
   interleaved vertex through the remap, simplification uses
   `meshopt_simplifyWithAttributes` + `meshopt_SimplifyRegularize` (never bone
   data as an attribute), `--sloppy` is refused because it invents vertex
   positions that carry no bindings, and every LOD emits a byte-identical
   skeleton so one joint palette serves all four levels.
9. Deterministic scene compiler merges `world.scene.json` (environment) with
   gameplay entities and emits `scene.json` + build metadata, including each
   mesh's `lod_paths`/`bounds_radius_m`/`sockets`/`skinned`/`animations` and
   the scene-level `performance` block. `mesh.animations` is the clip
   contract: semantic intent → real glTF clip name, so generated C++ writes
   `ctx.skeleton->play_clip(self, "walk", true)` without knowing what the
   retargeter called it, exactly as `socket_world(self, "muzzle")` works. It is also the **socket validator**: it is the only
   stage that sees both a CodeTask's `required_sockets` and the asset's
   derived sockets, so a mechanic requiring a socket its asset cannot supply
   fails the build loudly, with every violation listed at once.
10. Build (branches on `BuildSpec.target_device`):
    - `pcvr`: CMake configure + build (Visual Studio 18 2026, toolset v145) → `.exe`.
    - `quest`: `quest_build` instantiates `engine/android`, packages
      `scene.json` + GLBs + `.spv` into the APK assets, and runs
      `gradlew assembleDebug` (NDK) → standalone APK. See `docs/quest-build.md`.
11. Launch (branches on target_device): `pcvr` runs the `.exe` through OpenXR
    (Quest Link); `quest` does `adb install -r` + `am start` the NativeActivity.

## Verified model / SDK choices (April 2026)

- Claude: `claude-opus-4-7` (hard), `claude-sonnet-4-6` (light). Structured
  outputs via `output_config.format = {type: "json_schema", schema: ...}`.
  Strict tool use via `strict: true`. No beta headers required.
- OpenAI (ChatGPT fallback): default `gpt-5` (hard) and `gpt-5-mini`
  (light) via Chat Completions. Structured outputs via JSON mode
  (`response_format={"type": "json_object"}`) + schema embedded in the
  system prompt; caller re-validates with the same Pydantic model. The
  `LlmClient` wrapper applies the fallback transparently.
- Google GenAI: Python package `google-genai`. Imagen
- Google GenAI: Python package `google-genai`. Reference images use
  `gemini-3.1-flash-image` with `generate_content` and
  `response_modalities=["IMAGE"]` (Imagen 4 endpoints discontinued
  2026-08-17).
- Tripo: **V3 API** (`https://openapi.tripo3d.ai/v3`) called directly over
  `httpx` — NOT the `tripo3d` SDK, whose base URL is still
  `api.tripo3d.ai/v2/openapi` (V2 retires 2026-11-01). Flow:
  `POST /v3/files` → `POST /v3/generation/image-to-model` →
  `GET /v3/tasks/{id}` → download `output.model_url`. `model` is REQUIRED
  (no server default); allowed values are `v3.1-20260211`, `v3.0-20250812`,
  `v2.5-20250123`, `P1-20260311`.
- Vulkan SDK 1.4.341.x on Windows. Requires VS 2022 or 2026 toolchain.
- OpenXR Khronos loader via vcpkg. On Windows + Quest Link, Oculus OpenXR
  runtime is registered automatically.
- CMake generator for Visual Studio Community 2026: `"Visual Studio 18 2026"`
  with toolset `v145`. Requires CMake >= 4.2.

## Coding rules

- C++20. Headers under `engine/runtime/include/ptv/…`. Implementation under
  `engine/runtime/src/…`. Namespace `ptv`.
- Do not include ImGui from release builds of gameplay modules.
- Do not introduce new top-level dependencies without editing `engine/vcpkg.json`
  and the relevant doc.
- Python 3.11+. `ruff` + `black`-compatible formatting.
- All schema changes update pydantic model, exported JSON schema file under
  `packages/schemas/json/`, AND any C++ consumer in `engine/runtime/src/scene/`.

## When in doubt

Prefer reuse over regeneration. Prefer determinism over agent improvisation.
Prefer a smaller working slice over a bigger broken one.
