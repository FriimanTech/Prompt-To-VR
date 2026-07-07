# Overview

Prompt-To-Native-VR is a local-first system that converts a text prompt for a
VR game into a **native C++ executable** you can launch on Meta Quest 3 via
Quest Link (Windows PC VR).

## Why native?

- Deterministic performance over web runtimes.
- Direct Vulkan + OpenXR control (no Unity / Unreal / Godot / WebXR).
- The engine core is **hand-authored and reusable**. Generation only produces:
  - structured game plans (JSON),
  - reference images (Imagen / Gemini),
  - 3D models (Tripo, textured + PBR),
  - bounded C++ gameplay modules (Claude),
  - deterministic scene JSON.

## Components at a glance

| Component | Path | Purpose |
|---|---|---|
| Web control panel | `apps/web` | Project list, prompt editor, progress, logs, launch |
| Orchestrator | `apps/orchestrator` | FastAPI service that runs the pipeline |
| Schemas | `packages/schemas` | Pydantic v2 + JSON Schemas (single source of truth) |
| Provider clients | `packages/provider-clients` | Anthropic / Google / Tripo wrappers |
| Engine runtime | `engine/runtime` | Reusable native Vulkan + OpenXR engine |
| Generated code | `engine/generated` | The **only** place LLM-produced C++ lives |
| Starter sample | `samples/starter-room` | Hand-authored scene that runs without any LLM call |

## Pipeline summary

```
prompt
  → BuildSpec (Claude Opus)
  → AssetManifest (Claude Sonnet)   # OBJECTS only; the environment is C++
  → MechanicPlan (Claude Opus)
  → SceneSpec (Claude Opus, per scene)
  → reference images (Imagen / Gemini)
  → Tripo image-to-3D (textured + PBR)   # objects/props/characters
  → asset normalization (deterministic, local)
  → generated C++ gameplay modules (Claude)
  → world generation (single ChatGPT world-architect agent → C++ VR
    environment, compiled + run offline → world.scene.json)
  → scene compile (merge world + gameplay → deterministic JSON)
  → CMake configure + build (Visual Studio 18 2026)
  → launch native .exe via OpenXR
```

The VR environment is authored entirely in C++ by an agent (not a Tripo GLB);
see [`world-generation.md`](world-generation.md).

Every stage writes artifacts to `projects/<project_id>/` and can be resumed.

## Status

The repo ships:

- Working frontend (Vite production build ok).
- Working orchestrator (`uvicorn ptv_orchestrator.main:app`) with 26 passing
  tests.
- Schemas with round-trip and JSON-schema export.
- Provider clients with mocked tests; real SDKs are installed at runtime.
- Native engine skeleton that compiles and boots Vulkan + OpenXR + ECS +
  Jolt + Dear ImGui overlay.
- Starter-room sample scene (`samples/starter-room`).
- CTest smoke targets (`mechanics`, `scene`, `openxr`).
- All docs, `.env.example`, Cursor rules, and `AGENTS.md`.

What must still be done on your machine is listed in the
[`Manual Steps I Still Need To Do`](../README.md#manual-steps-i-still-need-to-do)
section of the top-level README.
