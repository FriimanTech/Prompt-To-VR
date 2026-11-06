# Local setup

Install the tools first from the [Quest setup guide](setup/README.md). That walkthrough is Quest-first. The recommended API setup is OpenAI, Gemini, Tripo3D, and Uthana, with no Claude key. See [API keys](setup/api-keys.md).

This page is the commands to run after those installs. The project is Windows-only and uses Visual Studio Community 2026 for the C++ toolchain.

PC VR through Quest Link has not been tested recently and may have major issues. Requirements for that path are in [PC VR requirements](setup/pc-vr-requirements.md). The recommended target is standalone Quest, especially Quest 3.

## Repo setup

```powershell
cd C:\path\to\Prompt-To-VR
copy .env.example .env
```

Open `.env` and set `OPENAI_API_KEY`, `GOOGLE_API_KEY`, `TRIPO_API_KEY`, `UTHANA_API_KEY` (skip Uthana only for Hands Only with no full-body avatar and no NPCs), and `VCPKG_ROOT`. Leave `ANTHROPIC_API_KEY` empty. Step-by-step key setup is in [`setup/api-keys.md`](setup/api-keys.md). `UTHANA_API_KEY` is already listed in `.env.example`. The other lines in `.env` are the engine's defaults. Leave them unless you want to customize.

## Python side

From the repo root:

```powershell
uv python install 3.13
uv venv .\vrvenv --python 3.13
.\vrvenv\Scripts\Activate.ps1
uv pip install -e .\packages\schemas -e .\packages\provider-clients -e ".\apps\orchestrator[dev]"
```

`vrvenv` is pinned to uv’s managed 3.13, not whatever Python is on PATH.

> **Notice:** The first `uv python install` and `uv pip install` download Python 3.13 and the project packages. Expect about **2–10 minutes**. Later runs are much shorter.

## Web side

```powershell
cd apps\web
npm install
```

> **Notice:** The first `npm install` usually takes about **1–5 minutes**.

## C++ side

```powershell
cd engine
# Configure (triggers vcpkg to fetch/build all dependencies on first run).
cmake --preset vs2026-x64
# Build the engine library, starter-room sample, tools, and tests.
cmake --build --preset vs2026-x64-release
# Run the C++ smoke tests.
ctest --preset vs2026-x64-release
```

> **Notice:** The first `cmake --preset vs2026-x64` is the long configure step. vcpkg downloads and compiles Vulkan, OpenXR, Jolt, and the other C++ libraries. Expect **10–30 minutes**. Later configures are cached and finish much faster.

> **Notice:** `cmake --build --preset vs2026-x64-release` is a separate wait. It compiles the engine, the starter-room sample, the tools, and the tests. The first compile often takes about **10–30 minutes** as well, and a slower PC can take longer. Later builds are shorter when only a few files changed.

> **Notice:** `ctest --preset vs2026-x64-release` usually finishes in a few minutes.

The libraries built on that first configure are Vulkan / OpenXR / GLFW / Jolt / EnTT / VMA / ImGui / TinyGLTF / spdlog, plus `meshoptimizer` (LOD decimation in `ptv_glb_optimize`) and `ktx` (KTX2 ASTC compression in `ptv_texture_compress`). Other presets: `ninja-x64` / `ninja-x64-release` and `vs2026-x64-debug`.

## Quick sanity check

```powershell
# In one terminal
.\vrvenv\Scripts\Activate.ps1
uvicorn ptv_orchestrator.main:app --host 127.0.0.1 --port 8787 --reload

# In another terminal
cd apps\web
npm run dev
# open http://127.0.0.1:5173

# Build the engine (no API keys needed for this compile)
cd engine
cmake --preset vs2026-x64
cmake --build --preset vs2026-x64-release
```

> **Notice:** Skip these two commands if you already ran them in the C++ section above. If you did not, they are the same **10–30 minute** configure and the same **10–30 minute** first compile.

That build produces the Windows engine, which the Quest code-generation check also uses. Launching `ptv_starter_room.exe` through Quest Link is the PC VR path and has not been tested recently. See [`setup/pc-vr-requirements.md`](setup/pc-vr-requirements.md) and [`build-and-run-pcvr.md`](build-and-run-pcvr.md). The recommended way to play a generated game is the Quest APK (VR Game) in [`build-and-run.md`](build-and-run.md).

See [`All_Stages_Docs/README.md`](All_Stages_Docs/README.md) for the pipeline stages and [`troubleshooting.md`](troubleshooting.md) for common failures.
