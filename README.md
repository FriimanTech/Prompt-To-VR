# Prompt to VR Engine

## Introduction

**Prompt to VR Engine**, created by **FriimanTech**, is an AI-powered VR game creation engine that turns text descriptions into playable VR games for the **Meta Quest 3**, with some **PC VR support**. No Unity, Unreal Engine, or Godot is required.

The VR engine is built in **C++ using Vulkan and OpenXR**. It coordinates the game creation process through AI agents that plan gameplay, generate 3D assets, automate character rigging and animation, build environments, and write additional gameplay code. Compiler errors are fed back to the coding agents for correction, while reusable engine systems handle interactions, movement, NPC behavior, and game rules.

The pipeline runs locally on your computer and connects to external AI services. It produces **native Quest APKs (VR Games) that can be downloaded and installed on your Quest 3 headset**, rather than browser-based WebXR experiences. This gives developers a foundation they can explore, extend, and build upon. **PC VR through Quest Link was tested during early development; recent testing and updates have primarily focused on standalone Quest 3 support.**

## Choose a path

There are three ways to use what is in this repository. Pick the one that matches what you want to do.

1. **Play a game this VR Engine already created.** This is the fastest path. You need a Quest headset and do not need Android Studio, Python, or Node.js. Follow [No Android Studio: Fastest Way](docs/setup/platform-tools-no-android-studio.md).
2. **Run the VR Engine and create games with it.** This is the normal path. It installs the full Windows and Quest toolchain, then lets you generate and play new games. Follow the [Quest setup guide](docs/setup/README.md).
3. **PC VR through Quest Link.** **This path is not recommended.** PC VR through Quest Link has not been tested recently and may have major issues. Recent development and testing have focused on standalone Quest, especially Quest 3. If you still want this path, follow [PC VR through Quest Link: requirements](docs/setup/pc-vr-requirements.md).

## Prerequisites

**A Windows machine is required** to run this engine's local pipeline (**Howver, Path 1 can be ran on Windows or Mac**).

**A Meta Quest 3 or Quest 2 is highly advised.** Recent development and testing have focused on standalone Quest 3. Quest 2 is a reasonable headset to try, but it has not received the same recent testing.

You can run the engine and create a complete VR game as an installable APK **without a Quest headset**. A headset is required to play that game and to run the on-headset checks. Building an APK and verifying it on a device are separate steps.

**Cursor is highly advised.** Other IDEs are acceptable. Cursor's AI agent can help with some dependency setup and is especially useful if you want to modify the VR engine. Using Cursor still involves a substantial amount of manual setup, particularly when installing and configuring Visual Studio Community 2026 and Android Studio. **(IDE are needed for paths 2 and 3)**

| You need this to build VR Games | You need this to play and test on a headset |
| --- | --- |
| Windows, the tools in the download list below, and the API keys for the kind of game you are making | A Quest headset, Developer Mode Turned on, a data USB cable, and `adb` seeing the headset |

## Required downloads (Quest)

> **First-time setup:** Follow the [complete Quest setup guide](docs/setup/README.md) for download links and step-by-step installation and configuration instructions for every tool listed here. Start with [Python](docs/setup/01-python.md).

| Tool                         | Version or requirement                                                                   | What it does here                                                                                                                                                                                                                                                      |
| ---------------------------- | ---------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Python                       | **3.11+**; **3.13 via** `uv` **recommended**                                             | Runs the local pipeline. Packages require Python 3.11 or newer. The project environment is Python 3.13, created later with `uv`.                                                                                                                                       |
| Node.js                      | **20+**                                                                                  | Runs the local web control panel.                                                                                                                                                                                                                                      |
| Visual Studio Community 2026 | **Desktop development with C++ / MSVC v145**                                             | Compiles the C++ engine on Windows.                                                                                                                                                                                                                                    |
| vcpkg                        | As required by the repository                                                            | Fetches the C++ libraries the engine builds with.                                                                                                                                                                                                                      |
| CMake                        | **4.2 or newer** (desktop)                                                               | Configures the Windows engine build. This is separate from the Android SDK CMake used for Quest.                                                                                                                                                                       |
| Vulkan SDK                   | **1.4.341.x**                                                                            | Provides Vulkan headers and the shader compiler.                                                                                                                                                                                                                       |
| uv                           | Python environment manager                                                               | Installs Python 3.13 and creates the `vrvenv` environment the engine runs in.                                                                                                                                                                                          |
| Oculus ADB Drivers           | Windows drivers for Quest development                                                    | Lets Windows talk to a Quest over USB.                                                                                                                                                                                                                                 |
| Android Studio               | **SDK Platform 34**, **NDK** `30.0.15729638`, **Platform-Tools /** `adb`, and **JDK 17** | Packages the standalone Quest APK. Also install Build-Tools 34.0.0 or newer, Android SDK CMake 3.22.1 or newer, and the Android SDK command-line tools. The [Android Studio guide](docs/setup/06-android-studio.md) sets `JAVA_HOME` to Android Studio's `jbr` folder. |

> **First-time setup:** Follow the [complete Quest setup guide](docs/setup/README.md) for download links and step-by-step installation and configuration instructions for every tool listed here. Start with [Python](docs/setup/01-python.md).

**PC VR through Quest Link: requirements**

**PC VR through Quest Link has not been tested recently and may have major issues. The recommended path is standalone Quest, especially Quest 3, because recent development and testing have focused there.**

The full list is in [PC VR through Quest Link: requirements](docs/setup/pc-vr-requirements.md). It is not the same list as the Quest downloads above. Android Studio, the NDK, Platform-Tools, and the Oculus ADB Drivers are for standalone Quest packaging, not for a PC VR-only workflow.

## Required API providers

> **API keys:** Follow the [API key setup guide](docs/setup/api-keys.md) for account, billing, and key steps for every provider below. Put real keys only in the local `.env` file, which is git-ignored.

| API key | What it does here |
| --- | --- |
| **OpenAI API key** | Powers the AI planning, reasoning, and code-generation stages, including stages that use the OpenAI Agents SDK. |
| **Gemini API key** | Generates the reference images used to create the game's 3D assets. |
| **Tripo3D API key** | Creates the 3D objects and characters used throughout the VR game. |
| **Uthana API key** | Provides character rigging and animation for workflows that need it, including the full-body VR avatar. |

### A. Use OpenAI for the LLM and agent stages

This engine has several stages that do AI-generated work. Some of those stages use regular LLM API calls. Most of them use OpenAI Agents through the OpenAI Agents SDK.

The code still lists OpenAI as the fallback for the regular LLM calls. If `ANTHROPIC_API_KEY` is empty, those calls use OpenAI. The agent stages already require OpenAI.

**Use OpenAI for all AI-generated LLM and agent stages instead of configuring both Claude and OpenAI.** You then need one LLM provider key, not two.

The full recommended setup is **four API keys: OpenAI, Gemini, Tripo3D, and Uthana.** Gemini, Tripo3D, and Uthana still provide image, 3D, and animation services.

### B. OpenAI is the best-tested configuration

The latest updates to this engine have been tested with OpenAI, not with Claude.

**For the best-tested and most reliable results, use OpenAI and do not configure a Claude API key.**

### C. Hands Only, with no avatar and no NPCs, needs three keys

If you want a VR game in **Hands Only** mode, without a full-body VR avatar **and without any in-game NPCs**, you only need **three API keys: OpenAI, Gemini, and Tripo3D.**

You do not need a Uthana API key for that configuration, because no character rigging or animation is required. Hands Only by itself does not remove Uthana. A Hands Only game that still has NPCs can still need Uthana.

> **API keys:** Follow the [API key setup guide](docs/setup/api-keys.md) for account, billing, and key steps for every provider above.

## Recommended API funding

These amounts are **recommended starting funds in USD**, not the price of an API key and not a guaranteed charge for generating one game. Actual use depends on the prompt, assets, animations, model choices, retries, and the work the pipeline does.

For the Hands Only, no-avatar, no-NPC configuration above, ignore the Uthana column. That setup does not need a Uthana key.

| Game size | OpenAI | Gemini | Tripo3D | Uthana |
| --- | --- | --- | --- | --- |
| **Small prompt games** | **$50; comfortable: $75** | **$5** | **$10** | **$5** |
| **Medium prompt games** | **$100; comfortable: $150** | **$10** | **$25** | **$10** |
| **Large prompt games** | **$200; comfortable: $300** | **$25** | **$50–75** | **$20–25** |

The standard and comfortable OpenAI amounts are different. Use the comfortable amount if you want more room for retries. There is no medium example; the medium funding row still applies to a medium-sized prompt.

**Small prompt game examples:**

- “Create a Goblin Shooter game where I have a laser blaster and can face off against waves of goblins for 5 waves.”
- “A simple shooting range with one pistol and floating targets.”

**Large prompt game examples:**

- “A Navy SEAL-style raid across two rooms: breach a door, clear hostiles with an assault rifle and a sidearm, then extract. Full-body avatar, 3 enemy types, ammo, and reload.”
- “An open combat campaign: desert outpost, underground bunker, and rooftop finale. Full avatar. Assault rifle, shotgun, grenades, shield. Multiple enemy factions—soldiers, drones, and heavy brutes—scripted waves, objectives, and a pause/end-game flow.”
- “A multi-room museum heist: stealth and shooting, laser puzzles, guards with varied weapons, a getaway courtyard. Full avatar, several unique characters, many grabbable props.”
- “A survival arena with three biomes linked by portals, a crafting bench, ranged and melee loadouts, and rotating boss waves.”

These examples show the size of prompt each funding row is meant for. They are not a promise that generation will succeed or that the game will hold a particular frame rate.

## Repository map

The repo is a monorepo:

- `apps/web` — React + Vite control panel (local website).
- `apps/orchestrator` — FastAPI backend that runs the generation pipeline.
- `engine/runtime` — hand-authored reusable VR engine (Vulkan + OpenXR + EnTT + Jolt).
- `engine/generated/{mechanics,ai,dialogue,world}` — the only place LLM-produced C++ is allowed (`world/` = offline VR environment builders).
- `engine/worldgen` — offline `ptv_worldgen` host for generated world builders.
- `engine/android` — Gradle template the Quest build stage instantiates per project.
- `engine/tools` — scene compiler, GLB validator / optimizer (meshoptimizer LODs), KTX2 texture compressor, GLB view renderer and socket derivation, bounds check.
- `engine/promoted` — generated modules promoted into the reuse catalog.
- `packages/schemas` — strict Pydantic + JSON schemas used across the pipeline.
- `packages/provider-clients` — OpenAI, Google GenAI, Tripo V3, and Uthana clients, plus the shared `LlmClient`.
- `samples/starter-room` — hand-authored demo scene that boots without calling any LLM.
- `projects/` — per-project local artifact workspace (created by the orchestrator, git-ignored).
- `docs/` — documentation. Start with [`docs/setup/README.md`](docs/setup/README.md). [`docs/All_Stages_Docs/`](docs/All_Stages_Docs/README.md) has per-stage notes.

## Quick links

- [`docs/setup/README.md`](docs/setup/README.md) — **start here for installation**
- [`docs/setup/api-keys.md`](docs/setup/api-keys.md) — API keys
- [`docs/local-setup.md`](docs/local-setup.md) — commands after the tools are installed
- [`docs/build-and-run.md`](docs/build-and-run.md) — play on Quest, including the sample APK
- [`docs/build-and-run-pcvr.md`](docs/build-and-run-pcvr.md) — PC VR `.exe` through Quest Link
- [`docs/quest-build.md`](docs/quest-build.md)
- [`docs/All_Stages_Docs/README.md`](docs/All_Stages_Docs/README.md) — stage-by-stage pipeline docs
- [`docs/overview.md`](docs/overview.md)
- [`docs/architecture.md`](docs/architecture.md)
- [`docs/provider-setup.md`](docs/provider-setup.md) — model and endpoint reference
- [`docs/troubleshooting.md`](docs/troubleshooting.md)

## High-level flow

The authoritative order is `_SHARED_STAGES`, `_PCVR_TAIL` and `_QUEST_TAIL` in
[`apps/orchestrator/ptv_orchestrator/pipeline/orchestrator.py`](apps/orchestrator/ptv_orchestrator/pipeline/orchestrator.py).
Stages 1–15 are identical for `pcvr` and `quest`; only the build tail branches
on `ProjectState.target_device` (the PC VR / Quest toggle in New Project,
mirrored into `BuildSpec.target_device`).

```
prompt
  →  1.  build_spec               regular LLM call (OpenAI when no Claude key)
  →  1b. optimization_preferences user's Quest optimization overrides → locks
  →  2.  asset_manifest           regular LLM call; objects only (environment is C++)
  →  2b. interaction_design       OpenAI Agents SDK
  →  3.  npc_plan                 roles, factions, animate vs slide, encounters
  →  3b. game_design              health, lives, objectives, endings, menus
  →  4.  mechanic_plan            built-in vs generated; controller bindings
  →  5.  scene_spec               one per scene, validate-and-repair
  →  6.  reference_images         Gemini image generation
  →  7.  tripo_models             Tripo V3 image-to-3D (textured + PBR)
  →  7b. character_routing        Tripo or Uthana per character
  →  8a. uthana_rigging           Uthana auto-rig (with fingers)
  →  8.  asset_rigging            Tripo rig-check + rig
  →  9.  asset_animation          Tripo animation retarget
  →  9b. uthana_motion_plan       pick each character's Uthana motions
  →  9c. uthana_animation         resolve, download, merge Uthana clips
  → 10.  asset_normalization      validate GLBs, scale / collision metadata
  → 10b. socket_generation        Gemini vision + geometry finds sockets
  → 11.  asset_contract           sockets, holds, character model frames
  → 12.  world_generation         C++ IWorldBuilder → world.scene.json
  → 13.  code_generation          C++ gameplay modules + compile-verify loop
  → 14.  asset_optimization       LODs, KTX2 textures, memory fit
  → 15.  scene_compile            world + gameplay + weapons → scene.json
  → 16.  pcvr:  cmake_build       Windows .exe
         quest: quest_build       standalone APK (Gradle + NDK)
  → 16b. quest only: quest_runtime_verify
                                  run on the headset when one is connected
  → 17.  engine_promotion         index generated modules for reuse
```

The recommended setup leaves the Claude key empty, so steps 1, 1b, and 2 use OpenAI. Steps from interaction design through code generation already use the OpenAI Agents SDK. World generation runs **before** code generation so gameplay code can use the room and spawn ids the world declared.

The engine core is hand-authored and reusable. Generation produces assets, scene data, bounded gameplay modules, and offline world builders. Built-in VR mechanics (teleport, slide, snap / smooth turn, grab, throw, hand pose, avatar body, equip, character locomotion, NPC follow / melee chase, encounter director, game flow + HUD, and the weapon set) are reused whenever possible.

## Run the engine after setup

Install the tools from the [Quest setup guide](docs/setup/README.md) and fill in [`.env`](docs/setup/api-keys.md) first. Then, from PowerShell at the repo root:

```powershell
# Backend (vrvenv from the build-tools guide)
.\vrvenv\Scripts\Activate.ps1
uvicorn ptv_orchestrator.main:app --host 127.0.0.1 --port 8787 --reload

# New terminal: web control panel
cd apps\web
npm run dev
# open http://127.0.0.1:5173
```

In the web panel, click **New project**. **Quest (standalone APK)** and **Full Player Avatar** are already selected. Paste a prompt and click **Create & run pipeline**. That is the recommended target. Switch to **PC VR (Quest Link)** or **Hands Only** only when you want those. **PC VR: Launch Starter Sample** starts the hand-authored PC VR starter room. It does not build that program, and it does not call the API providers. Build it first with the commands in [local setup](docs/local-setup.md). **Quest: Launch Sample Game** installs `samples/quest-sample/GlockLaserDeck.apk` on a connected Quest. Finish setup steps 5 and 6 first.

Quest APK packaging, install, and on-headset checks are in [`docs/build-and-run.md`](docs/build-and-run.md) and [`docs/quest-build.md`](docs/quest-build.md). A PC VR `.exe` launch through Quest Link is documented in [`docs/build-and-run-pcvr.md`](docs/build-and-run-pcvr.md). That path has not been tested recently and may have major issues.

More command detail is in [`docs/local-setup.md`](docs/local-setup.md).
