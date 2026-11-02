# Prompt-To-Native-VR

Local-first system that turns a detailed text prompt for a VR game into a
**native C++ Vulkan + OpenXR executable** (PC VR over Quest Link) or a
**standalone Quest 3 APK** (local sideload via `adb`).

The repo is a monorepo:

- `apps/web` — React + Vite control panel (local website).
- `apps/orchestrator` — FastAPI backend that runs the generation pipeline.
- `engine/runtime` — hand-authored reusable VR engine (Vulkan + OpenXR + EnTT
  + Jolt).
- `engine/generated/{mechanics,ai,dialogue,world}` — the only place LLM-produced
  C++ is allowed (`world/` = offline VR environment builders).
- `engine/worldgen` — offline `ptv_worldgen` host for generated world builders.
- `engine/android` — Gradle template the Quest build stage instantiates per project.
- `engine/tools` — scene compiler, GLB validator / optimizer (meshoptimizer LODs),
  KTX2 texture compressor, GLB view renderer and socket derivation, bounds check.
- `engine/promoted` — generated modules promoted into the reuse catalog.
- `packages/schemas` — strict Pydantic + JSON schemas used across the pipeline.
- `packages/provider-clients` — Anthropic, OpenAI, Google GenAI, Tripo V3 and
  Uthana clients (plus the shared `LlmClient` with Claude → OpenAI fallback).
- `samples/starter-room` — hand-authored demo scene that boots without calling
  any LLM.
- `projects/` — per-project local artifact workspace (created by the orchestrator,
  git-ignored).
- `docs/` — end-user documentation; **`docs/All_Stages_Docs/`** has per-stage
  relearning guides (summary + deep dive for every stage, PC VR and Quest).

## Quick links

- [`docs/All_Stages_Docs/README.md`](docs/All_Stages_Docs/README.md) — **stage-by-stage pipeline docs**
- [`docs/overview.md`](docs/overview.md)
- [`docs/architecture.md`](docs/architecture.md)
- [`docs/local-setup.md`](docs/local-setup.md)
- [`docs/build-and-run.md`](docs/build-and-run.md)
- [`docs/quest-build.md`](docs/quest-build.md)
- [`docs/run-lifecycle.md`](docs/run-lifecycle.md) — aborting a run, recovering a stuck project
- [`docs/provider-setup.md`](docs/provider-setup.md)
- [`docs/prompt-schema.md`](docs/prompt-schema.md)
- [`docs/world-generation.md`](docs/world-generation.md)
- [`docs/npc-and-ai.md`](docs/npc-and-ai.md)
- [`docs/game-flow.md`](docs/game-flow.md) — health, lives, objectives, end screens
- [`docs/weapons.md`](docs/weapons.md) and [`docs/sockets.md`](docs/sockets.md)
- [`docs/uthana.md`](docs/uthana.md) — character routing, rigging and motions
- [`docs/rendering.md`](docs/rendering.md) and [`docs/quest-optimization.md`](docs/quest-optimization.md)
- [`docs/runtime-subsystems.md`](docs/runtime-subsystems.md)
- [`docs/project-layout.md`](docs/project-layout.md)
- [`docs/troubleshooting.md`](docs/troubleshooting.md)

## High-level flow

The authoritative order is `_SHARED_STAGES`, `_PCVR_TAIL` and `_QUEST_TAIL` in
[`apps/orchestrator/ptv_orchestrator/pipeline/orchestrator.py`](apps/orchestrator/ptv_orchestrator/pipeline/orchestrator.py).
Stages 1–15 are identical for `pcvr` and `quest`; only the build tail branches
on `ProjectState.target_device` (the PC VR / Quest toggle in New Project,
mirrored into `BuildSpec.target_device`).

```
prompt
  →  1.  build_spec               Claude Opus (OpenAI fallback)
  →  1b. optimization_preferences user's Quest optimization overrides → locks
  →  2.  asset_manifest           Claude Sonnet; objects only (environment is C++)
  →  2b. interaction_design       how each item is held / fired; its sockets
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
                                  run on the headset: NPCs, game loop, weapons,
                                  textures, memory. A failed check fails the
                                  stage (the APK stays downloadable); no headset
                                  = skipped, not failed; a memory failure re-runs
                                  14 → 15 → 16 first
  → 17.  engine_promotion         index generated modules for reuse
```

World generation runs **before** code generation so the C++ gameplay agents can
reference the ids of rooms, doors and spawn zones the world actually declared.

The engine core is hand-authored and reusable. Generation only produces assets,
scene data, bounded gameplay modules, and offline world builders. Built-in VR
mechanics (teleport, slide, snap / smooth turn, grab, throw, hand pose, avatar
body, equip, character locomotion, NPC follow / melee chase, encounter director,
game flow + HUD, and the weapon set: weapon, bow, melee weapon, shield, item
source, NPC ranged attack) are reused whenever possible and are data-driven via
`SceneSpec.mechanic_bindings`.

## Minimal first-run (once prerequisites are installed and keys filled in)

```powershell
# 1. Install Python deps (from repo root; uv manages CPython 3.13)
uv python install 3.13
uv venv .\vrvenv --python 3.13
.\vrvenv\Scripts\Activate.ps1
uv pip install -e .\packages\schemas -e .\packages\provider-clients -e ".\apps\orchestrator[dev]"

# 2. Install web deps
cd apps\web
npm install

# 3. Start the orchestrator (repo root, vrvenv already active)
cd ..\..
uvicorn ptv_orchestrator.main:app --host 127.0.0.1 --port 8787 --reload

# 4. In another shell, start the web control panel
cd apps\web
npm run dev
# open http://127.0.0.1:5173

# 5. Configure + build the engine skeleton / starter room (PowerShell)
cd engine
cmake --preset vs2026-x64
cmake --build --preset vs2026-x64-release
```

When the starter-room sample is built, the web panel's "Launch sample" button
(or running `ptv_starter_room.exe` directly while Meta Quest Link is active)
boots a native XR session on PC VR.

See [`docs/local-setup.md`](docs/local-setup.md) for the full checklist,
[`docs/build-and-run.md`](docs/build-and-run.md) for PC VR launch,
[`docs/quest-build.md`](docs/quest-build.md) for the Quest APK path, and
[`docs/provider-setup.md`](docs/provider-setup.md) for API keys.

---

# Manual Steps I Still Need To Do

This list is the authoritative summary of everything that can only be done on
your machine.

## 1. Software to install

| Item | Why | Notes |
|---|---|---|
| **Visual Studio Community 2026** with the *Desktop development with C++* workload, *C++ CMake tools for Windows*, and the *Windows 10 SDK / 11 SDK* components | Required for the native runtime build | Toolset `v145`, CMake generator string `"Visual Studio 18 2026"` |
| **CMake ≥ 4.2** | VS 2026 generator support | `winget install Kitware.CMake` or use the one shipped with VS 2026 |
| **Vulkan SDK 1.4.341.x** (Windows x64) from LunarG | Vulkan headers, validation layers, `glslc` shader compiler | Sets `VULKAN_SDK` env var automatically |
| **vcpkg** (checkout anywhere, e.g. `C:\src\vcpkg`) | C++ dependency manifest mode | Set `VCPKG_ROOT=C:\src\vcpkg` in `.env` and run `bootstrap-vcpkg.bat` once |
| **uv** (`winget install --id=astral-sh.uv -e`) | Orchestrator + schemas + provider clients | uv installs a managed CPython 3.13; no global Python required |
| **Node.js 20+** and **npm** | Vite frontend | Ship with Windows installer |
| **Meta Quest Link** desktop app | Routes Quest 3 as a PC VR headset and registers the Oculus OpenXR runtime | Enable "Developer Runtime Features" under Beta |
| **Oculus Quest 3** connected by Link cable or Air Link, with Meta Quest Link active | PC VR headset | Set the Oculus OpenXR runtime as the active one in the Meta Quest Link app |
| **Android Studio SDK/NDK + adb** (Quest target only) | Builds and sideloads standalone APKs | See [`docs/quest-build.md`](docs/quest-build.md) |

Optional: install **Git** if you don't already have it.

## 2. API accounts / keys you must create

| Provider | Used for | Required? | Where to get a key |
|---|---|---|---|
| Anthropic | BuildSpec, AssetManifest, optimization preferences (Claude structured JSON) | Recommended (falls back to OpenAI when blank) | <https://console.anthropic.com> → *API Keys* |
| OpenAI | Every Agents SDK stage (interaction design, NPC plan, game design, mechanic plan, scene spec, character routing, Uthana motion plan, world + code generation); Claude fallback | Yes | <https://platform.openai.com/api-keys> |
| Google AI Studio | Gemini reference images and socket vision (socket generation falls back to geometry-only without it) | Yes | <https://aistudio.google.com/app/apikey> |
| Tripo | Image-to-3D, rig-check, rigging, animation retarget (V3 API) | Yes | <https://developers.tripo3d.ai> |
| Uthana | Player-avatar / humanoid auto-rig with fingers, locomotion and idle motions | Optional (without it every character routes to Tripo, so the player avatar gets no finger joints) | <https://uthana.com> |

## 3. Environment variables to fill

Copy `.env.example` to `.env` at the repo root and fill in the keys. `.env`
(and every `.env.*` variant except `.env.example`) is git-ignored; never commit
real keys.

```
ANTHROPIC_API_KEY=
OPENAI_API_KEY=
GOOGLE_API_KEY=
TRIPO_API_KEY=
UTHANA_API_KEY=
VCPKG_ROOT=C:\src\vcpkg
```

The orchestrator reads `.env` via `pydantic-settings`. The frontend picks up the
orchestrator URL from its own `apps/web/.env` (`VITE_API_BASE_URL`, default
`http://127.0.0.1:8787`).

## 4. Things that are scaffolded but blocked on your action

- **Running the full generation pipeline** requires Anthropic, OpenAI, Google,
  and Tripo keys. Without them you can still build and launch the hand-authored
  `starter-room` sample end-to-end, which validates Vulkan + OpenXR + mechanics
  without spending credits.
- **First-time vcpkg install** will download and build Vulkan/OpenXR/Jolt/etc.
  Expect 10–30 minutes on the first `cmake --preset`. Subsequent builds are
  cached.
- **Meta Quest Link's OpenXR runtime selection** is a one-time click: in the
  Meta Quest Link desktop app, open *Settings → Beta → Set Oculus as active
  OpenXR runtime*.
- **Shader compilation** relies on `glslc` from the Vulkan SDK being on
  `PATH`. The Vulkan SDK installer adds it automatically.
- **Quest standalone builds** need `ANDROID_HOME`, `ANDROID_NDK_HOME`, `JAVA_HOME`,
  and `adb` on PATH — see [`docs/quest-build.md`](docs/quest-build.md).

## 5. Exact commands to run the first example

From a PowerShell at the repo root, once installs are done and `.env` is
filled:

```powershell
# One-time: bootstrap vcpkg
& "$env:VCPKG_ROOT\bootstrap-vcpkg.bat"

# Backend (from repo root)
uv python install 3.13
uv venv .\vrvenv --python 3.13
.\vrvenv\Scripts\Activate.ps1
uv pip install -e .\packages\schemas -e .\packages\provider-clients -e ".\apps\orchestrator[dev]"
uvicorn ptv_orchestrator.main:app --host 127.0.0.1 --port 8787 --reload

# (new terminal) Frontend
cd apps\web
npm install
npm run dev
# open http://127.0.0.1:5173

# (new terminal) Build the engine + starter room sample
cd engine
cmake --preset vs2026-x64
cmake --build --preset vs2026-x64-release

# Put on the Quest 3, start Meta Quest Link, then launch:
.\build\vs2026-x64\samples\starter-room\Release\ptv_starter_room.exe
```

Then in the web panel click **New Project**, paste a detailed VR game prompt,
choose **PC VR** or **Quest**, and watch the pipeline run. You can also press
**Launch starter sample** on the home page to verify the engine without
invoking providers.
