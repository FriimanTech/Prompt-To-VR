# Overview

Prompt-To-Native-VR is a local-first system that converts a text prompt for a
VR game into a **native C++ executable** (PC VR over Quest Link) or a
**standalone Quest APK**, built from a shared pipeline.

## Why native?

- Deterministic performance over web runtimes.
- Direct Vulkan + OpenXR control (no Unity / Unreal / Godot / WebXR).
- The engine core is **hand-authored and reusable**. Generation only produces:
  - structured game plans (JSON),
  - reference images (Gemini),
  - 3D models (Tripo, textured + PBR),
  - rigged/animated character GLBs (Tripo),
  - bounded C++ gameplay + world modules,
  - deterministic scene JSON.

## Components at a glance

| Component | Path | Purpose |
|---|---|---|
| Web control panel | `apps/web` | Project list, prompt editor, progress, logs, launch |
| Orchestrator | `apps/orchestrator` | FastAPI service that runs the pipeline |
| Schemas | `packages/schemas` | Pydantic v2 + JSON Schemas (single source of truth) |
| Provider clients | `packages/provider-clients` | Anthropic / Google / Tripo wrappers |
| Engine runtime | `engine/runtime` | Reusable native Vulkan + OpenXR engine |
| Worldgen host | `engine/worldgen` | Offline `ptv_worldgen` — runs generated world builders |
| Generated code | `engine/generated` | LLM-produced C++ (mechanics, ai, dialogue, **world**) |
| Starter sample | `samples/starter-room` | Hand-authored scene that runs without any LLM call |
| Stage docs | `docs/All_Stages_Docs/` | Per-stage relearning docs (Summary + deep dive) |

## Pipeline summary (17 runnable stages)

Shared stages **1–15** are identical for `pcvr` and `quest`. Only **stage 16**
branches on `BuildSpec.target_device`.

```
prompt
  →  1. BuildSpec (Claude Opus)
  →  2. AssetManifest (Claude Sonnet)     # objects only; environment is C++
  →  3. NPC plan (OpenAI gpt-5.5)         # animate vs slide; factions; reconciles manifest
  →  4. MechanicPlan (OpenAI gpt-5.5)      # reuse-first; CodeTask DAG
  →  5. SceneSpec (OpenAI gpt-5.5, per scene)
  →  6. Reference images (Gemini)
  →  7. Tripo image-to-3D (textured + PBR)
  →  8. Auto-rigging (Tripo rig-check + rig)
  →  9. Animation retarget (Tripo; semantic intents → clips)
  → 10. Asset normalization (deterministic GLB validation)
  → 11. Asset contract (deterministic sockets via ptv_glb_sockets)
  → 12. C++ code generation (OpenAI Agents + compile-verify)
  → 13. World generation (OpenAI world architect → world.scene.json)
  → 14. Asset optimization (LOD decimation + performance profile)
  → 15. Scene compile (merge world + gameplay → scene.json)
  → 16. pcvr: cmake_build (.exe)  |  quest: quest_build (APK)
  → 17. Engine promotion (registry of reusable generated modules)
```

The VR **environment** is C++ (`IWorldBuilder`), not a Tripo GLB — see
[`world-generation.md`](world-generation.md). NPC behaviour and combat use
engine services — see [`npc-and-ai.md`](npc-and-ai.md).

Every stage writes artifacts to `projects/<project_id>/` and can be resumed.

**Detailed per-stage documentation:** [`All_Stages_Docs/README.md`](All_Stages_Docs/README.md)

## Status

The repo ships:

- Working frontend (Vite production build ok).
- Working orchestrator (`uvicorn ptv_orchestrator.main:app`) with automated tests.
- Schemas with round-trip and JSON-schema export.
- Provider clients with mocked tests; real SDKs are installed at runtime.
- Native engine (Vulkan + OpenXR + ECS + Jolt + skinning + navigation).
- Starter-room sample scene (`samples/starter-room`).
- CTest smoke targets.
- Full docs under `docs/`, stage-by-stage docs under `docs/All_Stages_Docs/`,
  Cursor rules, and `AGENTS.md`.

What must still be done on your machine is listed in the
[`Manual Steps I Still Need To Do`](../README.md#manual-steps-i-still-need-to-do)
section of the top-level README.
