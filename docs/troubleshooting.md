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

## Sliding locomotion still moves in a fixed world direction in an existing project

Locomotion logic lives in `engine/runtime/src/core/Application.cpp`
(search for `LocomotionMode::SlideHeadRelative` /
`SlideControllerRelative`). The catalog mechanics
`builtin.locomotion.slide.head_relative` and
`builtin.locomotion.slide.controller_relative` in
`engine/runtime/src/mechanics/BuiltinMechanics.cpp` are intentional
**no-op stubs** (their `tick` methods are empty) -- they exist only so
each scene's `systems_global` entity can list them without
`MechanicHost` complaining "no mechanic registered".

That means the engine has **one** locomotion implementation, shared by
every game. Whenever it changes:

1. The fix lands in `engine/runtime/src/...` -- the engine source.
2. **Each existing project's `builds/cmake-current/...exe` was built
   from an older copy of the engine, so it still has the old
   behaviour** until you rebuild it.
3. Open the project in the orchestrator UI and click **Resume** /
   re-run the **CMake Build** stage. CMake will detect the changed
   sources and recompile `ptv_runtime.lib` plus the launcher exe.
   Newly generated projects pick up the latest engine automatically.

### Root causes that have shipped at least once

* **`last_head_forward_xz_` defaulted to `(0, 0, -1)`.** The slide path
  used this cache as a "we don't have a head pose yet" fallback, so
  early frames before the runtime delivered a valid VIEW pose slid the
  player along world `-Z`. **Fix:** the cache now defaults to the zero
  vector, and the slide path explicitly skips motion when its length
  is zero (no fixed-direction fallback anywhere). See
  `engine/runtime/include/ptv/core/Application.h`.
* **`xrWaitFrame` was inside `render_frame`, after `sync_input`.** The
  predicted display time used to locate `view_space_` against the
  stage was therefore the *previous* frame's time. On frame 0 it was
  zero, so `input.head_valid` started false and the cache fallback
  above kicked in. **Fix:** `XrRuntime::begin_xr_frame()` is now
  called *before* `sync_input` -- it runs `xrWaitFrame` +
  `xrBeginFrame`, stores the current frame's predicted display time,
  and lets `sync_input` locate spaces against it. `render_frame()`
  reuses the pending state instead of issuing a second `xrWaitFrame`.
* **Right-vector cross product was `cross(up, fwd)`** (yields `-X`
  when `fwd = -Z`), silently swapping left/right strafe.
  **Fix:** uses `cross(fwd, up)` for both head- and controller-
  relative branches.
* **`forward_axis = -ly` inverted "push forward".** OpenXR 1.1
  ("Standard input identifiers") defines thumbstick `+1` on the Y
  axis as "all the way up", i.e. *forward push*. The runtime had the
  sign negated, so pushing the stick forward produced backward
  motion in **both** sliding modes. **Fix:** `forward_axis = +ly` in
  `Application::run` (the controller-relative path inherits the same
  variable).
* **Cross-axis stick bleed leaked into perpendicular motion.**
  Touch Plus thumbsticks accumulate ~0.10 of off-axis reading after
  a few months of use. With only a *radial* dead zone, "push pure
  right" reads as `(+1.0, +0.10)` and the player drifts diagonally.
  **Fix:** after the radial dead zone (now 0.18) the runtime
  applies a *cardinal snap* -- when one axis dominates (`>0.5`) and
  the other is small (`<0.18`), the small axis is forced to zero.
  Intentional diagonals (both axes large) are preserved unchanged.
* **Controller-relative slide used `/grip/pose`'s `-Z`.** Quest
  Touch grips are angled ~30° below the controller body, so
  `/grip`'s `-Z` is along the user's *hand*, not the controller
  body. Pushing "forward along the angled hand axis" includes an
  upward component, which combined with the inverted `forward_axis`
  produced the "controller looks tilted up + forward = backward"
  symptom. **Fix:** `XrSession::create_action_set` now binds a
  separate `aim_pose_action_` to `/aim/pose`, exposed via
  `InputState::aim_left/right`. `Application::run` reads
  `aim_world_rot[0] * (0,0,-1)` for controller-relative slide,
  teleport arc origin direction, and the locomotion menu pointer.
  `/grip/pose` is still used for hand visualization and grab
  detection (where palm-centric is what you want).
* **Procedural hand mesh's `+Z` was placed at the thumb side.** The
  OpenXR `/grip/pose` spec says `-Z` is the forward (sword-blade)
  direction and `+Z` is the wrist/forearm direction. The mesh had
  the thumb stub at `+Z` and the wrist at `-Z`, which is the
  *opposite* convention. The visible symptom: the hand mesh
  appeared to "point diagonally upward" when the user held the
  controller flat. **Fix:** `push_hand_mesh` now puts the wrist
  stub at `+Z` and removes the thumb stub (the symmetric palm + wrist
  reads correctly for both hands without per-hand mirroring). See
  `engine/runtime/src/core/Application.cpp::push_hand_mesh`.

### How to verify the fix is in your current build

Check `projects/<id>/logs/<latest-run>.log` (or stdout if you launched
the exe directly): the slide path now logs once per second whenever
the player is actually sliding, e.g.

```
[info] Slide[head_rel] fwd=(+0.00,-1.00) stick=(+0.00,-0.92) speed=1.38m/s rig=(+0.00,-1.20) head_fresh=yes
```

Physically turning your head 180 deg flips `fwd` within ~1 s. If `fwd`
stays constant while you rotate, the build is from before the fix --
re-run cmake_build.

If the player isn't moving at all when you push the stick, you'll see
this 1 Hz warning instead -- it tells you exactly which tracked pose
is missing:

```
[warn] Slide[head_rel] no basis (head_fresh=no, left_hand_valid=yes, head_valid=no). Player won't move until tracking is restored.
```

`head_valid=no` means OpenXR didn't return a valid VIEW pose against
the stage, which usually points at a runtime / Quest Link issue
(headset asleep, runtime not active, etc.) rather than the engine.

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

## Tripo stage logs `No module named 'aiohttp'` for every asset

The `tripo3d` PyPI package lists `aiohttp` as an *optional* dependency even
though its async HTTP paths (upload / poll / download) import it at runtime.
If your venv was created before this was pinned, install aiohttp manually:

```powershell
.\vrvenv\Scripts\pip.exe install "aiohttp>=3.8.0"
```

`ptv-provider-clients` now declares `aiohttp>=3.8.0` as a direct dependency
so fresh installs will always pick it up.

## Tripo stage fails with `[2010] You don't have enough credit`

Your Tripo account is out of generation credits. Buy more at
<https://platform.tripo3d.ai/billing>, then click **Resume** in the web UI
on the affected project — the pipeline will restart from the Tripo stage,
produce the GLBs, and continue through asset normalization, code generation,
and the CMake build.

## "No GLB file found" during asset normalization

The Tripo stage silently skipped the asset (often: missing API key, or
bad reference image). Check `projects/<id>/logs/tripo_models.log`. Since
April 2026 the Tripo stage hard-fails (status `failed`) if *any* reference
images were produced upstream but *zero* GLBs came back, so this should no
longer let an empty asset set slip through to normalization.

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
