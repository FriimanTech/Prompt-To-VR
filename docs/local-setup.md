# Local setup

This project is Windows-first and assumes Visual Studio Community 2026 as the
C++ toolchain.

## One-time installs

1. **Visual Studio Community 2026** with:
   - *Desktop development with C++* workload
   - *C++ CMake tools for Windows* component
   - *Windows 10 SDK* or *Windows 11 SDK*
2. **CMake ≥ 4.2** — the one shipped with VS 2026 is fine
   (`Visual Studio 18 2026` generator requires CMake 4.2+).
3. **Vulkan SDK 1.4.341.x** from [LunarG](https://vulkan.lunarg.com/sdk/home#windows).
   The installer sets `VULKAN_SDK` automatically.
4. **vcpkg** (manifest mode):

   ```powershell
   git clone https://github.com/microsoft/vcpkg C:\src\vcpkg
   C:\src\vcpkg\bootstrap-vcpkg.bat
   $env:VCPKG_ROOT = "C:\src\vcpkg"   # add to system env vars permanently
   ```

5. **uv** — install with `winget install --id=astral-sh.uv -e`, then open a
   new PowerShell so `uv` is on PATH. uv downloads a managed CPython 3.13
   for this repo; you do not need a global Python install.
6. **Node.js 20+** and npm.
7. **Meta Quest Link** desktop app. Connect the Quest 3 by Link cable (or
   enable Air Link) and — once connected — open *Settings → Beta* and click
   *Set Oculus as active OpenXR runtime*.

## Repo setup

```powershell
cd C:\Users\micha\Documents\Prompt-To-VR
copy .env.example .env
# then open .env in an editor and fill in ANTHROPIC_API_KEY, GOOGLE_API_KEY,
# TRIPO_API_KEY, and VCPKG_ROOT.
```

## Python side

From the repo root:

```powershell
uv python install 3.13
uv venv .\vrvenv --python 3.13
.\vrvenv\Scripts\Activate.ps1
uv pip install -e .\packages\schemas -e .\packages\provider-clients -e ".\apps\orchestrator[dev]"
```

`vrvenv` is pinned to uv’s managed 3.13, not whatever Python is on PATH.

## Web side

```powershell
cd apps\web
npm install
```

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

The first `cmake --preset vs2026-x64` downloads and builds Vulkan / OpenXR /
GLFW / Jolt / EnTT / VMA / ImGui / TinyGLTF / spdlog through vcpkg. Expect
10-30 minutes the first time; subsequent configures are cached.

## Quick sanity check

```powershell
# In one terminal
.\vrvenv\Scripts\Activate.ps1
uvicorn ptv_orchestrator.main:app --host 127.0.0.1 --port 8787 --reload

# In another terminal
cd apps\web
npm run dev
# open http://127.0.0.1:5173

# Build + launch the starter sample (no API keys needed)
cd engine
cmake --preset vs2026-x64
cmake --build --preset vs2026-x64-release
# Put on the Quest 3, start Meta Quest Link, then:
.\build\vs2026-x64\samples\starter-room\Release\ptv_starter_room.exe
```

See [`build-and-run.md`](build-and-run.md) for the full launch procedure and
[`troubleshooting.md`](troubleshooting.md) for common failures.
