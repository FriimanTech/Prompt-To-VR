# Prompt-To-Native-VR

Local-first system that turns a detailed text prompt for a VR game into a
**native C++ Vulkan + OpenXR executable** you can launch on Meta Quest 3 via
Quest Link (PC VR).

The repo is a monorepo:

- `apps/web` — React + Vite control panel (local website).
- `apps/orchestrator` — FastAPI backend that runs the generation pipeline.
- `engine/runtime` — hand-authored reusable VR engine (Vulkan + OpenXR + EnTT
  + Jolt).
- `engine/generated/{mechanics,ai,dialogue}` — the only place LLM-produced C++
  is allowed.
- `engine/tools` — scene compiler, GLB validator, generated-code bounds check.
- `packages/schemas` — strict Pydantic + JSON schemas used across the pipeline.
- `packages/provider-clients` — Anthropic, Google GenAI, Tripo clients.
- `samples/starter-room` — hand-authored demo scene that boots without calling
  any LLM.
- `projects/` — per-project local artifact workspace (created by the orchestrator).
- `docs/` — end-user documentation and troubleshooting.

## Quick links

- [`docs/overview.md`](docs/overview.md)
- [`docs/architecture.md`](docs/architecture.md)
- [`docs/local-setup.md`](docs/local-setup.md)
- [`docs/build-and-run.md`](docs/build-and-run.md)
- [`docs/provider-setup.md`](docs/provider-setup.md)
- [`docs/prompt-schema.md`](docs/prompt-schema.md)
- [`docs/runtime-subsystems.md`](docs/runtime-subsystems.md)
- [`docs/project-layout.md`](docs/project-layout.md)
- [`docs/troubleshooting.md`](docs/troubleshooting.md)

## High-level flow

```
prompt
  → BuildSpec
  → AssetManifest
  → MechanicPlan
  → SceneSpec
  → reference images (Imagen / Gemini)
  → Tripo image-to-3D (textured, PBR)
  → asset normalization
  → generated C++ gameplay modules (Claude)
  → deterministic scene compiler
  → CMake configure + build (Visual Studio 18 2026)
  → launch native .exe via OpenXR
```

The engine core is hand-authored and reusable. Generation only produces assets,
scene data, and bounded gameplay modules. Built-in VR mechanics (teleport,
grab, throw, trigger zones, etc.) are reused whenever possible and are
data-driven via `SceneSpec.mechanic_bindings`.

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
(or running `build\Release\ptv_starter_room.exe` directly while Meta Quest Link
is active) boots a native XR session.

See [`docs/local-setup.md`](docs/local-setup.md) for the full checklist,
[`docs/build-and-run.md`](docs/build-and-run.md) for Quest Link details, and
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

Optional: install **Git** if you don't already have it.

## 2. API accounts / keys you must create

| Provider | Used for | Required? | Where to get a key |
|---|---|---|---|
| Anthropic | BuildSpec / MechanicPlan / SceneSpec / C++ generation | Yes | <https://console.anthropic.com> → *API Keys* |
| OpenAI (ChatGPT) | Automatic fallback for every Claude call when Claude is unavailable | Optional (recommended) | <https://platform.openai.com/api-keys> |
| Google AI Studio | Imagen + Gemini image generation (reference images) | Yes | <https://aistudio.google.com/app/apikey> |
| Tripo | Image-to-3D (textured / PBR) | Yes | <https://developers.tripo3d.ai> |

## 3. Environment variables to fill

Copy `.env.example` to `.env` at the repo root and fill in at least:

```
ANTHROPIC_API_KEY=sk-ant-...
OPENAI_API_KEY=sk-...            # optional ChatGPT fallback for Claude
GOOGLE_API_KEY=...
TRIPO_API_KEY=...
VCPKG_ROOT=C:\src\vcpkg
```

The orchestrator reads `.env` via `python-dotenv`. The frontend picks up the
orchestrator port from its own `apps/web/.env`.

## 4. Things that are scaffolded but blocked on your action

- **Running the full generation pipeline** requires all three API keys. Without
  them you can still build and launch the hand-authored `starter-room` sample
  end-to-end, which validates Vulkan + OpenXR + mechanics without spending
  credits.
- **First-time vcpkg install** will download and build Vulkan/OpenXR/Jolt/etc.
  Expect 10–30 minutes on the first `cmake --preset`. Subsequent builds are
  cached.
- **Meta Quest Link's OpenXR runtime selection** is a one-time click: in the
  Meta Quest Link desktop app, open *Settings → Beta → Set Oculus as active
  OpenXR runtime*.
- **Shader compilation** relies on `glslc` from the Vulkan SDK being on
  `PATH`. The Vulkan SDK installer adds it automatically.

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
and watch the pipeline run. You can also press **Launch starter sample** on the
home page to verify the engine without invoking providers.
