# Troubleshooting

Every entry below has been triggered at least once while bringing up this
repo on a fresh Windows 11 + VS 2026 box.

## The UI says a stage is running, but nothing is running

This happened when the orchestrator was killed (Ctrl+C, or a `--reload`
restart) while a stage was in flight: `ProjectState.status` is persisted while
the in-flight run registry is not, so the project claimed to be busy forever
and the panel disabled every button.

The orchestrator now reconciles this at startup, so restarting the server
clears it. The **Abort** button does the same thing on demand. See
[`run-lifecycle.md`](run-lifecycle.md).

## `PermissionError: [WinError 5] Access is denied` or `[WinError 32] ... being used by another process`

On Windows, a file another process has open cannot be renamed over, renamed away or deleted.

The web panel caused this. It refreshed the artifact list on every pipeline event, and the server answered by opening and hashing every file in the project, including the codegen ledger mid-replace and freshly downloaded GLBs mid-move. The codegen stage crashed on `codegen_board.json`, and a paid Tripo rig was recorded as failed because its staging file could not be deleted.

What changed:

- **Writers retry.** Every replace and move goes through `ptv_orchestrator/fsutil.py`, which retries a locked file for about 3 s.
- **JSON writes are atomic.** Each one uses a unique temp name.
- **A codegen board save never kills a worker.** The board stays authoritative in memory and is flushed at the end of the stage.
- **Readers back off.** The artifact listing skips temp and staging files, caches hashes by size and modification time, and skips large files modified in the last 2 s. The panel refreshes the list only when a stage starts or finishes.

If it still happens, something outside the pipeline is holding the file, such as an Explorer preview pane, an open GLB viewer or an antivirus scan. Close it and use **Resume**; finished work is kept.

## A stage fails with just `QueueFull`

That was the live-progress feed, not the stage. Each open browser tab had a 256-event queue, and stages run on worker threads that pushed into it with `put_nowait`. An `asyncio.Queue` is not thread-safe, so the server never woke up to drain it. A burst of output, such as Gradle printing a failed native build, filled it and raised `QueueFull` through `ctx.emit`, which failed `quest_build` and hid the real Gradle error.

`events.py` now hands each event to the subscriber's own loop (`call_soon_threadsafe`). When a reader falls behind it drops the oldest log line, never a stage status, and `publish()` never raises. `quest_build` also streams only progress, diagnostics and the verdict; the full output is still in `logs/gradle.log`.

## NPCs are created but do not appear (or stand still) in the game

Read the headset log: `adb logcat -s ptv` (or the latest
`projects/<id>/logs/runtime/verify_*.log` written by `quest_runtime_verify`).

- `npc ... feet_gap=` far from 0: something moved the character off the
  floor. The classic cause -- the floor probe hitting the NPC's own capsule
  and lifting it by its height every frame, so every NPC left the level on
  frame one -- is fixed in `builtin.character_locomotion`; a generated
  mechanic writing `CTransform` would do the same.
- `Unresolved mechanic bindings` / `No mechanic registered for id`: a binding
  names something the build does not contain. Generated AI used to be
  registered as `IBehaviour` and never hosted; it is now. `scene_compile`
  fails on any remaining unresolved `builtin.*` / `generated.*` binding.
- An ally walking off toward the world origin: generated code following the
  scene's `player_rig` by name before it became the live player. Re-run
  `code_generation`; the contract checks regenerate such tasks.
- No `spawn:` lines after `encounter: wave 1/N started`: check
  `npc_summary ... alive=` and the `spawn:` refusals (budget, blocked spot).
- HUD health never drops although enemies attack: the health mechanic was
  listening to a private hit inbox. Re-run `code_generation`.

## Code generation passed but `quest_build` fails with clang errors

The PC compile check uses MSVC, and the APK is built with the NDK's clang. They disagree on real code. A probe such as `if constexpr (requires { dep::f(weapon); })` with ordinary arguments is quietly "false" on MSVC and a hard error on clang ("too few arguments to function call").

For Quest projects, code generation now runs a clang syntax check (`quest_syntax_check`) after the MSVC build passes. Its errors go through the same repair loop, so the owning task is fixed before packaging. The code-task prompt also forbids probing a dependency's API. If the NDK is not installed, the check is skipped with a warning. A failed `quest_build` now quotes the first compiler errors in its message.

## Compile verification fails with `LNK2005 ... already defined` for a generated mechanic

The desktop build used to compile every game's generated C++ under `engine/generated/` into one library. Two games that both planned a task with the same name both defined the same classes, for example `ptv::shooting_hub_detail::ShootingHub` in the goblin shooter and the shooting range. The link failed even though each game's code was correct, and the repair loop could never fix it, because the other copy belongs to another project.

`engine/generated/CMakeLists.txt` now compiles only `PTV_GENERATED_PROJECT_ID` (the orchestrator always passes it), plus any promoted mechanics the plan reuses. The Quest build already worked this way. An engine-only build with no project id compiles no generated code; pass `-DPTV_GENERATED_PROJECT_ID=*` for the old everything-at-once behavior.

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

Slide logic lives in
`engine/runtime/src/mechanics/builtin/slide/SlideMechanic.cpp` -- the
mechanics `builtin.locomotion.slide.head_relative` and
`builtin.locomotion.slide.controller_relative` are real implementations, not
stubs. The shared comfort ramp is in `builtin/slide/SlideComfort.h` and the
thumbstick dead zone in `builtin/StickFilter.h`.

The engine still has **one** slide implementation, shared by every game.
Whenever it changes:

1. The fix lands in `engine/runtime/src/...` -- the engine source.
2. **Each existing project's `builds/cmake-current/...exe` was built
   from an older copy of the engine, so it still has the old
   behaviour** until you rebuild it.
3. Open the project in the orchestrator UI and click **Resume** /
   re-run the **CMake Build** stage. CMake will detect the changed
   sources and recompile `ptv_runtime.lib` plus the launcher exe.
   Newly generated projects pick up the latest engine automatically.

### Root causes that have shipped at least once

* **The cached head-forward defaulted to `(0, 0, -1)`.** The slide path
  used this cache as a "we don't have a head pose yet" fallback, so
  early frames before the runtime delivered a valid VIEW pose slid the
  player along world `-Z`. **Fix:** the cache defaults to the zero
  vector, and the slide path explicitly skips motion when its length
  is zero (no fixed-direction fallback anywhere). It now lives on the
  head-relative mechanic in `builtin/slide/SlideMechanic.cpp`.
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
  motion in **both** sliding modes. **Fix:** `forward_axis = +y` in
  `SlideMechanicBase::on_update` (both slide modes share it).
* **Cross-axis stick bleed leaked into perpendicular motion.**
  Touch Plus thumbsticks accumulate ~0.10 of off-axis reading after
  a few months of use. With only a *radial* dead zone, "push pure
  right" reads as `(+1.0, +0.10)` and the player drifts diagonally.
  **Fix:** after the radial dead zone (now 0.18) a *cardinal snap*
  applies -- when one axis dominates (`>0.5`) and the other is small
  (`<0.18`), the small axis is forced to zero. Intentional diagonals
  (both axes large) are preserved unchanged. Both live in
  `engine/runtime/src/mechanics/builtin/StickFilter.h`, shared by every
  stick-driven built-in so teleport's aim threshold and slide's speed
  ramp are expressed against the same filtered magnitude.
* **Controller-relative slide used `/grip/pose`'s `-Z`.** Quest
  Touch grips are angled ~30° below the controller body, so
  `/grip`'s `-Z` is along the user's *hand*, not the controller
  body. Pushing "forward along the angled hand axis" includes an
  upward component, which combined with the inverted `forward_axis`
  produced the "controller looks tilted up + forward = backward"
  symptom. **Fix:** `XrSession::create_action_set` now binds a
  separate `aim_pose_action_` to `/aim/pose`, surfaced on each hand of
  the `InputSnapshot` as `aim_position` / `aim_rotation`. The
  controller-relative slide, the teleport arc direction and the
  locomotion menu pointer all read `aim_rotation * (0,0,-1)`.
  `/grip/pose` (`hand_position` / `hand_rotation`) is still used for
  hand visualization and grab detection, where palm-centric is what
  you want.
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
[info] Slide[head_rel] fwd=(+0.00,-1.00) stick=(+0.00,+0.92) speed=1.38m/s rig=(+0.00,-1.20)
```

Physically turning your head 180 deg flips `fwd` within ~1 s. If `fwd`
stays constant while you rotate, the build is from before the fix --
re-run cmake_build.

If the player isn't moving at all when you push the stick, you'll see
this 1 Hz warning instead -- it tells you exactly which tracked pose
is missing:

```
[warn] Slide[head_rel] no basis (head_valid=no, left_aim_valid=yes). Player won't move until tracking is restored.
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

## Tripo stage fails with `Unsupported Tripo model version`

`TRIPO_MODEL_VERSION` in `.env` is set to something V3 does not accept. The
whitelist is `v3.1-20260211`, `v3.0-20250812`, `v2.5-20250123` and
`P1-20260311`; leave the variable **blank** to get the default
(`v3.1-20260211`). A common mistake is pasting `v2.5-20260210`, which is the
auto-*rig* model version, not a generation one.

Unlike V2, V3 has no server-side default for this field, so the client
resolves and validates it at startup rather than letting the API 400
mid-run.

## Tripo stage fails with `HTTP 401` on the balance preflight

Either the key is wrong, or the client is pointed at the wrong region. A
Tripo key only validates in the region it was issued in. International keys
use the default `https://openapi.tripo3d.ai/v3`; China-mainland keys need
`TRIPO_BASE_URL=https://openapi.tripo3d.com/v3` in `.env`.

## Tripo stage fails with `[2010] You don't have enough credit`

Your Tripo account is out of generation credits. Buy more at
<https://developers.tripo3d.ai>, then click **Resume** in the web UI
on the affected project — the pipeline will restart from the Tripo stage,
produce the GLBs, and continue through asset normalization, code generation,
and the CMake build.

## "No GLB file found" during asset normalization

The Tripo stage silently skipped the asset (often: missing API key, or
bad reference image). Check `projects/<id>/logs/tripo_models.log`. Since
April 2026 the Tripo stage hard-fails (status `failed`) if *any* reference
images were produced upstream but *zero* GLBs came back, so this should no
longer let an empty asset set slip through to normalization.

## `asset_optimization` fails: "could not build ptv_glb_optimize"

The stage shells out to the `ptv_glb_optimize` tool (meshoptimizer-based
decimation) and builds it on demand if it is not already in `engine/build/`.
A cold build needs vcpkg to install `meshoptimizer`. Build it once by hand:

```powershell
cmake --preset vs2026-x64
cmake --build build/vs2026-x64 --config Release --target ptv_glb_optimize
```

## The game runs but distant objects look blocky / pop in

Expected, and tunable. Assets are decimated into LOD chains against the device
triangle budget, and the adaptive governor coarsens LODs further when frame
time slips. Check `adb logcat -s ptv` for a `governor:` line — if it is
escalating, the scene is over budget for the headset. Levers, in order:

- lower the asset count or mark key assets `importance: hero` in the manifest,
- raise `Settings`-level budgets in
  `packages/schemas/ptv_schemas/performance.py` (the single policy location),
- set an explicit `max_triangles` on a specific `AssetItem`.

Objects popping in during the first second or two after load is the streaming
loader working as designed — it is what keeps the app from tripping Android's
ANR watchdog.

## APK is much larger than expected

`quest_build` ships only `*_lod*.glb` files once `asset_optimization` has run,
and wipes its staging directory each run. If the APK still looks bloated,
confirm the LOD chain exists (`projects/<id>/assets/<asset_id>/*_lod0.glb`) and
inspect what actually got packaged:

```powershell
Get-ChildItem projects/<id>/builds/quest/app/src/main/assets/assets -Recurse -Filter *.glb
```

Note that overwriting an APK in place can leave dead bytes, so the file can
read larger than its contents; delete `app/build/outputs` and rebuild for a
true size.

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
