# Troubleshooting

Every entry below has been triggered at least once while bringing up this
repo on a fresh Windows 11 + VS 2026 box.

## CMake says "Visual Studio 18 2026 could not find any instance"

- Confirm VS Community 2026 is installed (not just 2022).
- Run `vswhere -latest -products * -format json` and make sure `installationVersion`
  starts with `18.`.
- If you have both 2022 and 2026, pass
  `-DCMAKE_GENERATOR_INSTANCE="C:/Program Files/Microsoft Visual Studio/18/Community"`.

## vcpkg takes forever on first `cmake --preset`

Normal. The first run builds Vulkan, OpenXR, Jolt, and all other native
dependencies from source. Expect 10-30 minutes. `cmake --preset vs2026-x64`
again is cached.

## `find_package(openxr_loader)` fails

Make sure `VCPKG_ROOT` is set and `--preset vs2026-x64` was used (the preset
points CMake at `%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake`). If you
skipped the preset, pass `-DCMAKE_TOOLCHAIN_FILE` manually.

## `glslc` not found

The Vulkan SDK installer adds `%VULKAN_SDK%\Bin` to `PATH`. Restart the
terminal after installing the SDK. Alternatively, set `PATH` per-session:

```powershell
$env:PATH = "$env:VULKAN_SDK\Bin;" + $env:PATH
```

## The exe starts, opens a desktop mirror, but the headset shows nothing

- Quest Link status must be "Active" in the Meta Quest Link desktop app.
- Oculus must be the *active* OpenXR runtime
  (*Settings → Beta* in the Meta Quest Link app).
- Some SteamVR / Virtual Desktop installs re-assert themselves as the
  default runtime. Re-click "Set Oculus as active OpenXR runtime".
- If the engine log prints `No OpenXR system found`, the runtime isn't
  running yet. Put the headset on first, then launch.

## `ANTHROPIC_API_KEY is not set`

Fill `.env` at the repo root. The orchestrator loads it via
`pydantic-settings`. Restart `uvicorn` after editing `.env`.

## Tripo calls time out

The Tripo wrapper polls every 3 s with a 600 s timeout by default. You can
raise the ceiling by editing `TripoConfig.timeout_s`. Very large models
with `texture_quality="detailed"` can take 60-120 s per asset.

## "No GLB file found" during asset normalization

The Tripo stage silently skipped the asset (often: missing API key, or
bad reference image). Check `projects/<id>/logs/tripo_models.log`.

## Frontend: "Orchestrator unreachable"

The dev server proxies `/api/*` to `http://127.0.0.1:8787` by default. If
you changed `ORCHESTRATOR_PORT` in `.env`, also set `VITE_API_BASE_URL` in
`apps/web/.env`.

## CMake can't find `VulkanMemoryAllocator::VulkanMemoryAllocator`

vcpkg has shipped this port under different target names. The CMakeLists
uses `GPUOpen::VulkanMemoryAllocator` which is the current export. If a
much older vcpkg is in use, `vcpkg update && vcpkg upgrade` fixes it.

## Jolt-specific link errors on MSVC

Jolt's vcpkg port ships headers that rely on compile-time flags. The engine
sets the ones it needs via CMake; if you add extra Jolt features (profiler,
debug renderer), enable the matching vcpkg *features*:

```jsonc
// engine/vcpkg.json
{ "name": "joltphysics", "features": ["debugrenderer", "profiler"] }
```

## Generated C++ won't compile

Open `projects/<id>/logs/cmake.log`. Likely causes:

- A generated module tried to include a non-whitelisted header. The
  orchestrator should have stripped it; if it didn't, run
  `py engine\tools\check_generated_bounds.py` to see violations.
- The mechanic id registered in `PTV_REGISTER_MECHANIC(...)` doesn't match
  the `mechanic_id` in the scene. Check the generated file and the
  `scene.json`.
