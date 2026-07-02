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
   multiplayer, speech-to-text, marketplace publishing, Steam / Quest standalone
   packaging** in this version.
4. Schema-first development. Once a schema exists in `packages/schemas`,
   downstream stages MUST consume the structured artifact, not the raw prompt.
5. Reuse built-in VR mechanics. The engine ships reusable, data-driven modules
   (teleport, smooth locomotion, grab, throw, trigger zones, etc.). Generated
   code adds mechanics; it does not reimplement basic VR behavior.
6. Generated C++ (`.cpp`, `.h`, `.hpp`) is confined to
   `engine/generated/{mechanics,ai,dialogue}/`. It MUST NOT modify the
   renderer, XR session loop, platform layer, memory core, or base ECS.
   Generated headers are allowed for same-task, same-directory use only
   (bare-filename `#include "foo.h"` includes). Registration macros
   (`PTV_REGISTER_MECHANIC`, etc.) must only appear in `.cpp` files.
   Include violations cause the CodeTask to fail loudly (not silently
   strip).
7. Use Claude for the hardest gameplay planning and hardest C++ generation
   (`claude-opus-4-7`). Use `claude-sonnet-4-6` for lighter work. If Claude
   is unavailable (missing key or API error) the pipeline transparently
   falls back to OpenAI via the shared `LlmClient`
   (`OPENAI_HARD_MODEL=gpt-5`, `OPENAI_LIGHT_MODEL=gpt-5-mini`).
8. Tripo image-to-3D calls MUST enable textured, PBR output with image-aligned
   textures (`texture=True`, `pbr=True`, `texture_alignment="original_image"`).
9. Verify provider SDK signatures and model names with live docs before
   changing provider client code.
10. When code structure changes, update `/docs/` in the same change.

## Repository map

- `apps/web` – React + Vite + TypeScript control panel.
- `apps/orchestrator` – FastAPI backend that runs the pipeline.
- `engine/runtime` – **hand-authored** reusable C++ VR engine. Stable.
- `engine/generated/{mechanics,ai,dialogue}` – the **only** directories where
  LLM-produced C++ lives.
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
2. BuildSpec → **AssetManifest** (Claude Sonnet, structured output).
3. AssetManifest + BuildSpec → **MechanicPlan** (Claude Opus). Each mechanic
   must be classified `builtin`, `builtin+config`, or `generate`.
4. All above → **SceneSpec** (Claude Opus, structured output).
5. Reference images for each `AssetItem` → Gemini / Imagen.
6. Reference images → Tripo image-to-3D (textured, PBR).
7. Assets normalized (scale / materials / collision).
8. CodeTasks from MechanicPlan → Claude Opus → C++ files (`.cpp` + optional
   `.h`/`.hpp` headers) in `engine/generated/{mechanics,ai,dialogue}/`.
9. Deterministic scene compiler emits `scene.json` and build metadata.
10. CMake configure + build (Visual Studio 18 2026 generator, toolset v145).
11. Orchestrator launches the produced `.exe` through OpenXR.

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
- Tripo: Python package `tripo3d`. `image_to_model(... texture=True,
  pbr=True, texture_alignment="original_image", texture_quality="detailed")`.
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
