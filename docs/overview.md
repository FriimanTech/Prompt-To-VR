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
  - rigged/animated character GLBs (Tripo or Uthana),
  - named sockets on held items (Gemini vision + geometry),
  - bounded C++ gameplay + world modules,
  - deterministic scene JSON.

## Components at a glance

| Component | Path | Purpose |
|---|---|---|
| Web control panel | `apps/web` | Project list, prompt editor, progress, logs, launch |
| Orchestrator | `apps/orchestrator` | FastAPI service that runs the pipeline |
| Schemas | `packages/schemas` | Pydantic v2 + JSON Schemas (single source of truth) |
| Provider clients | `packages/provider-clients` | Anthropic / OpenAI / Google / Tripo / Uthana wrappers |
| Engine runtime | `engine/runtime` | Reusable native Vulkan + OpenXR engine |
| Worldgen host | `engine/worldgen` | Offline `ptv_worldgen` — runs generated world builders |
| Generated code | `engine/generated` | LLM-produced C++ (mechanics, ai, dialogue, **world**) |
| Starter sample | `samples/starter-room` | Hand-authored scene that runs without any LLM call |
| Stage docs | `docs/All_Stages_Docs/` | Per-stage relearning docs (Summary + deep dive) |

## Pipeline summary

The authoritative order is `_SHARED_STAGES` + `_PCVR_TAIL` / `_QUEST_TAIL` in
`apps/orchestrator/ptv_orchestrator/pipeline/orchestrator.py`. The shared
stages (01–15 below) are identical for `pcvr` and `quest`; only the build tail
(16–17) branches on the device, read from `ProjectState.target_device` (the
create-time toggle, mirrored into `BuildSpec.target_device`). The numbers
match the folders under `docs/All_Stages_Docs/{PC_VR_Stages,Quest_Stages}/`.

```
prompt
  → 01.  BuildSpec (Claude Opus; OpenAI fallback)
  → 01b. Optimization preferences (keyword gate + one light LlmClient call)
  → 02.  AssetManifest (Claude Sonnet)     # objects only; environment is C++
  → 02b. Interaction design (OpenAI gpt-5.6-sol + WebSearchTool)  # holds, sockets, weapons
  → 03.  NPC plan (OpenAI gpt-5.6-sol)     # animate vs slide; factions; reconciles manifest
  → 03b. Game design (OpenAI gpt-5.6-sol)  # health, lives, respawn, objectives, end screens
  → 04.  MechanicPlan (OpenAI gpt-5.6-sol)  # reuse-first; CodeTask DAG
  → 05.  SceneSpec (OpenAI gpt-5.6-sol, per scene)
  → 06.  Reference images (Gemini)
  → 07.  Tripo image-to-3D (textured + PBR)
  → 07b. Character routing (Tripo vs Uthana, per character)
  → 08a. Uthana auto-rig (fingers)
  → 08.  Auto-rigging (Tripo rig-check + rig)
  → 09.  Animation retarget (Tripo; semantic intents → clips)
  → 09b. Uthana motion plan (no generation)
  → 09c. Uthana animation (resolve / download / merge clips)
  → 10.  Asset normalization (deterministic GLB validation)
  → 10b. Socket generation (Gemini vision segmentation + geometry solvers)
  → 11.  Asset contract (sockets, holds, each character's model frame)
  → 12.  World generation (OpenAI world architect → world.scene.json)
  → 13.  C++ code generation (OpenAI Agents + compile-verify)
  → 14.  Asset optimization (LODs, KTX2 textures, profile, locks, memory fit)
  → 15.  Scene compile (merge world + gameplay → scene.json, scene complexity)
  → 16.  pcvr: cmake_build (.exe)  |  quest: quest_build (APK)
  → 16b. quest only: quest_runtime_verify (on-headset NPC / loop / weapons /
         textures / memory checks; report-only, skipped with no headset;
         a memory failure re-runs 14 → 15 → 16 up to
         `quest_memory_max_correction_rounds` times)
  → 17.  Engine promotion (registry of reusable generated modules)
```

World generation runs **before** code generation, so the codegen agent sees
the ids of the objects the world actually declared.

The VR **environment** is C++ (`IWorldBuilder`), not a Tripo GLB — see
[`world-generation.md`](world-generation.md). NPC behaviour and combat use
engine services — see [`npc-and-ai.md`](npc-and-ai.md). The game loop
(health, lives, respawn, objectives, Game Over / victory screens, Try Again,
Leave Game) is the engine's `builtin.game_flow`, run from `game_design.json`
— see [`game-flow.md`](game-flow.md).

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
