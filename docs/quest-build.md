# Quest (standalone) build path

Prompt-To-VR can build a generated game for two targets, chosen at create time
with the PC VR vs Quest toggle in the New Project UI. That choice is stored on
`ProjectState.target_device`, which is the **single source of truth**: the
orchestrator routes the build tail off it, `asset_optimization` derives the
`PerformanceProfile` from it, the launch route reads it, and every planning
prompt names it. `BuildSpec.target_device` is a mirrored copy — the build_spec
stage overwrites whatever the model returned with the project's value, and a
stale copy from an older project is re-stamped at run start.

- `pcvr` (default): a Windows `.exe` (Vulkan + Win32 OpenXR) run on the PC
and viewed on the headset over Quest Link. Built by `stage_cmake`.
- `quest`: a standalone Android APK (Vulkan + Android OpenXR loader,
NativeActivity) installed and run locally on the Quest 3. Built by
`stage_quest_build` and installed with `adb`.

Stages 1–15 (BuildSpec → … → scene compile, including C++ codegen, world
generation, and asset optimization) are **identical** for both targets. Only
the build + launch stages branch on the device. Generated mechanics use only
`IMechanic` / `World` / `Math`, so no per-target codegen changes are needed.

```
prompt + device toggle
  -> ProjectState.target_device  (source of truth; mirrored into BuildSpec)
  -> shared stages 1-15 (scene.json + generated C++ + LOD GLBs + world.scene.json)
  -> pcvr:  cmake_build (.exe)      -> launch_exe (Quest Link)
     quest: quest_build (APK) -> quest_runtime_verify (report-only;
            memory failure re-runs asset_optimization -> scene_compile
            -> quest_build, at most quest_memory_max_correction_rounds)
            -> adb install -r + am start NativeActivity
  -> engine_promotion (both targets)
```



## Engine architecture

One engine tree, one platform-abstraction seam:

- Shared: `Renderer`/Vulkan, `XrRuntime` frame loop, ECS (EnTT), Jolt,
mechanics, `SceneLoader`, `Math`.
- `engine/runtime/src/platform/desktop/` - GLFW mirror window (PC VR).
- `engine/runtime/src/platform/android/` - `android_main` (native_app_glue),
`AndroidAssetSource` (AAssetManager), Android OpenXR loader init.
- `ptv::IAssetSource` (`ptv/platform/AssetSource.h`) abstracts asset reads:
filesystem on desktop, APK assets on Quest. `SceneLoader`, `GltfMeshLoad`,
and the `Renderer` shader loader all go through it.
- `XrSession` / `VulkanContext` select `XR_USE_PLATFORM_WIN32` vs
`XR_USE_PLATFORM_ANDROID` from the per-target CMakeLists; the Android path
adds `xrInitializeLoaderKHR` + `XR_KHR_android_create_instance`.



## Quest APK template

`engine/android/` is a Gradle template the `quest_build` stage instantiates
into `projects/<id>/builds/quest/`, substituting `@PTV_*@` tokens and copying
`scene.json`, **every `scene.*.json` sibling**, GLBs and compiled `.spv` shaders
into `app/src/main/assets/`. The siblings matter for a multi-scene game: a portal
resolves through the scene index, whose paths are relative to `scene.json`, so
packaging only the entry point leaves an APK that runs and looks right until the
player walks into the first doorway. The
native `.so` (`libptv_quest.so`) is built by
`app/src/main/cpp/CMakeLists.txt` (NDK), linking the shared engine runtime +
this project's generated mechanics + the Khronos OpenXR Android loader.

### Catching Quest-only compile errors early

Code generation's compile check builds with MSVC, but this APK is built with the NDK's clang, and the two disagree on some code. For Quest projects, code generation therefore also runs `quest_syntax_check`: the NDK `clang++ --target=aarch64-linux-android32 -fsyntax-only` over every generated `.cpp`, with this CMakeLists' defines and warning flags. Errors go to the repair loop like any compile error.

The NDK is found through `android_ndk_home`, `$ANDROID_NDK_HOME`, or `<SDK>/ndk/<android_ndk_version>`; without one, the check is skipped with a warning. Turn it off with `codegen_quest_syntax_check=false`.

`quest_build` writes the full Gradle output to `logs/gradle.log`. The live UI shows only progress, diagnostics and the verdict, and a failure message quotes the first compiler errors.

Required Meta manifest entries (see `AndroidManifest.xml`): the
`android.hardware.vr.headtracking` uses-feature, the
`com.oculus.intent.category.VR` intent category, `com.oculus.supportedDevices`,
and the `android.app.lib_name` meta-data.

## Toolchain (already installed on this machine)

Android Studio SDK/NDK + `ANDROID_HOME`, `ANDROID_NDK_HOME`, `JAVA_HOME`,
`platform-tools` (adb) and `cmdline-tools/latest/bin` on PATH. SDK levels
default to min 32 / target 34 / compile 34; the OpenXR loader is
`org.khronos.openxr:openxr_loader_for_android:1.0.34`. Overridable via
`Settings` (`android_*` knobs).

Everything is drivable from Cursor: the `quest_build` stage shells out to
`gradlew assembleDebug`; launch shells out to `adb install -r` + `am start`.
Android Studio the IDE is only needed for one-time SDK/NDK install and deep
native debugging (logcat / GPU profiling); it does NOT need to be running for a
build.

A Gradle 8.9 wrapper is vendored in `engine/android/` (`gradlew`, `gradlew.bat`,
`gradle/wrapper/*`) and copied into `builds/quest/` per project, so no system
Gradle install is required -- `gradlew` self-bootstraps the Gradle distribution
using `JAVA_HOME` (Android Studio's bundled JBR works). Gradle 8.9 is the
minimum required by the template's AGP 8.7.

## On-headset verification

`quest_runtime_verify` runs after `quest_build` in the Quest tail ONLY
(`_QUEST_TAIL` in `orchestrator.py`; PC VR never runs it, the on-demand
`POST /projects/{id}/verify` route returns 400 for a PC VR project, and the
stage skips itself if it ever runs on one). It is **report-only**: it never
repairs anything or reruns earlier stages.

1. `adb devices`. No adb, no device, an `unauthorized` / `offline` device,
   or several devices with none named in `quest_verify_device_serial`: the
   stage is SKIPPED -- recorded as a successful stage with a "skipped" event
   and reason, `runtime_verification.json` gets `status: "skipped"`, and the
   pipeline continues. A headset that disconnects mid-run is also a skip.
2. `adb shell am broadcast -a com.oculus.vrpowermanager.prox_close` keeps the
   XR session running with the headset on a desk (Meta's scriptable-testing
   docs), `setprop debug.oculus.guardian_pause 1` pauses Guardian (an
   unworn headset that "looks worn" has no boundary, and Horizon OS's Guardian
   setup flow then makes every launch time out in window placement; the
   previous value is restored afterwards), `setprop debug.ptv.verify 1` turns
   on the engine's verification mode (scripted player walk, player cannot
   die), then `install -r`, `logcat -c` and a plain `am start` confirmed with
   `pidof` (up to 6 attempts with a wake/back key between them; `-W` waits for
   a launch-complete a VR NativeActivity never reports). Optionally,
   `quest_verify_store_pin` lets the stage use Meta's Scriptable Testing
   service to disable Guardian, dialogs and auto-sleep for the run.
3. Stream `adb logcat -v time -s ptv:*` to `logs/runtime/verify_<ts>.log`
   for the encounter start delay plus `quest_verify_capture_seconds` (40 s;
   20 s for a game with no NPCs), plus 35 s for every wave before the one in
   which the last NPC type first appears (capped at 240 s). Verification mode
   defeats an enemy shortly after it hits the player, so later waves arrive
   without anyone holding the controllers. Streamed, not read once at the end: the
   headset's log ring buffer holds well under a minute of NPC telemetry, so
   a final `logcat -d` has already lost the start-up and manifest lines.
4. Force-stop, `setprop debug.ptv.verify 0` (and `verify_flow` /
   `verify_weapons` 0) and `vrpowermanager.automation_disable` -- always,
   even after a failure.

Checks come from THIS game's `npc_plan.json` and `scene.json`, and each runs
only when the game calls for it:

- **Every Quest game (base):** launched, scene loaded, frame loop ran, no
  crash, every mechanic binding resolves, every GLB loaded. A Hands Only game
  (`player.hands.source = "default"`) must also show its default hands: the
  engine's 1 Hz `hands:` line must report both hands drawn, finite,
  0.08-0.45 m across their joints and within 1.2 m of the head. With no
  controller tracked (the headset on a desk) verify mode holds the visible
  hands in front of the eyes, so the check runs unattended. These hands once
  loaded 20 m long -- a room the player stood inside -- while every other
  check passed. A game with no
  NPCs runs only these; its NPC section reads "not applicable: this game has
  no NPCs".
- **Placed at load (`persistent_count > 0`):** present, skeleton and clips
  resolved, DRAWN feet within 5 cm of the floor (median; the lowest animated
  foot joint, not the declared offset -- a hovering NPC is checked against
  its hover height instead and a free flyer not at all), faces where it is
  going (median heading error under 20 degrees while moving), inside the
  walkable area.
- **Stationary (`movement_style: stationary`):** stays within 0.25 m of where
  it was placed, plays `idle` if it has one; with combat behaviours it must
  land hits on the player from its spot. Follow / chase movement checks never
  apply to it.
- **Followers (`follow_leader`):** within the trailing band (plus slack)
  while the scripted player walks, walks, and plays a walk/run clip while
  moving.
- **Encounters:** waves start; enough enemies spawn within the start delay
  plus 15 s.
- **Enemies hostile to the player with chase/melee:** on the floor, facing
  their travel / target, close the distance, play a gait clip while moving,
  land hits on the player.
- **The game loop (projects with `game_design.json`):** a SECOND launch with
  `debug.ptv.verify_flow 1`, where the player can die and
  `builtin.game_flow` clicks its own end-screen buttons (see
  [game-flow.md](game-flow.md#5-on-headset-verification-quest-only-report-only)).
  The stage waits up to `flow_capture_seconds` (75 s plus 17 s per life plus
  a tenth of any survival timer, capped at 300 s) for the game to leave on its
  own, logs to `logs/runtime/verify_<ts>_flow.log`, then launches it once
  more. Checks: the loop starts; health 0 costs a life; the player respawns;
  Game Over follows the last life; Try Again restarts and resets lives;
  winning shows the victory screen; Play Again restarts; Leave Game ends the
  process; the app launches again. A game that cannot kill the player is
  checked for never dying instead.
- **Weapons (scenes with player weapons):** a THIRD launch with
  `debug.ptv.verify_weapons 1`. The player stands still and the engine's
  `WeaponVerifier` supplies a scripted hand: it holds each weapon through
  `IHands::request_hold`, aims at the nearest hostile (else a clone of an
  NPC template 8 m ahead), fires five single pulls and then three seconds
  held (two seconds for a beam; a scripted draw and release for a bow), and
  drops it. Logged to `logs/runtime/verify_<ts>_weapons.log`; the stage
  stops waiting at `weapons: done` or after 25 s plus 9 s per weapon (capped
  at 240 s). Checks per weapon: held, fires, every shot leaves within 2 cm of
  the muzzle socket, hits the target, the target's health drops through
  `ctx.combat`, a beam ticks continuously, the projectile pool stays inside
  `max_live_projectiles`, and p95 frame time holds the frame budget (13.9 ms
  at 72 Hz). See [weapons.md](weapons.md).
- **Textures (scenes with textured meshes):** the base pass's `tex:` line
  must show every texture resident, none missing, and ASTC KTX2 rather than
  PNG/JPEG decoded on the headset. Then a FOURTH launch with
  `debug.ptv.verify_textures 1`: the engine's `TextureGallery` hides the scene
  (and every text / effect overlay) and shows each unique textured LOD0 (the
  default hands included) 1.1 m in front of the player, three-quarter view,
  one every 4 s -- a rigged asset framed by its skinned bind pose, measured on
  the CPU, because a rig's raw vertices can be in another scale (the default
  hands' span 2 mm before skinning) -- logging
  `texgallery: show i/n <path> textured= missing=`. The stage takes a Quest
  screenshot of the empty background and of each asset (`metacam`
  `TAKE_SCREENSHOT`, pulled from `/sdcard/Oculus/Screenshots/` into
  `logs/runtime/textures_<ts>/`) and compares each with an offline
  `ptv_glb_views` render of the same LOD: the asset must cover enough of the
  view, show no magenta (missing-texture) pixels, keep at least 30% of the
  reference's colourfulness and gain no more than 30% near-white surface.
- **Render health (every Quest game):** FFR's density map reached the render
  pass on every frame (without it nothing draws); a merged static batch that
  dissolved because a "static" member moved, or a GPU allocation that needed
  out-of-memory recovery, is reported as a warning.
- **User locks (projects with Custom Quest Optimizations):** every `perf:`
  line must honour them -- "FFR off" means `ffr 0` throughout, `fixed`
  exactly that level, `max` never above it, resolution `off` / `high` a render
  scale of 1.0 throughout. A frame-rate miss while the governor was blocked by
  the user's own locks is a **warning**, not a failure.
- **Memory (every Quest game):** `dumpsys meminfo` every 5 s and one
  `gpumeminfo` sample during the base pass, the engine's `mem:` line, and a
  `logcat -b all` scan after every launch for `lowmemorykiller` / `lmkd` /
  "Low Memory Kill" naming the package. Then a soak launch
  (`quest_memory_soak_seconds`, 600 s by default: the scripted walk, PSS every
  15 s) and a pressure launch (`debug.ptv.simulate_memory_pressure` dropped to
  5% of the thresholds for 20 s, then restored). Checks: peak PSS at or under
  the 5.0 GiB ceiling, steady state at or under the 4.5 GiB target (Meta's
  limit is 5.75 GiB), no low-memory kill, PSS growth across the game-loop
  pass's restarts at most 50 MB per cycle, soak growth after a 60 s warm-up at
  most 5 MB/min, and under pressure MemoryGuard reaches red and the game stays
  alive. The `memory` section of `runtime_verification.json` carries
  `memory_status` (see [quest-optimization.md](quest-optimization.md)); a
  memory failure writes `memory_feedback.json` and the orchestrator re-runs
  asset_optimization → scene_compile → quest_build → verify with the measured
  calibration, at most `quest_memory_max_correction_rounds` (2) times.

The APK also carries the engine's UI font: `quest_build` copies
`engine/runtime/assets/fonts/` (Atkinson Hyperlegible Bold + its OFL
license) into `assets/engine/fonts/`, beside the default hands. Without it
the game runs, but no label, title or HUD number is drawn.

Output: `runtime_verification.json` (`RuntimeVerification` in
`packages/schemas`) with `passed` / `failed` / `skipped`, every check with
pass / fail / not applicable / warning and its evidence, the measured
`performance` medians, the `memory` section, plus
`logs/runtime/verify_<timestamp>.log` (and `_flow`, `_weapons`, `_textures`,
`_soak`, `_pressure`). A failed check fails the stage with the exact list,
memory failures first. A skipped run (no headset) still records the memory
estimate as `estimated_safe_not_device_tested` or `at_risk` -- never as
verified. The APK stays downloadable in every case
(`GET /projects/{id}/apk`, the Download APK button on the project page).

## Packaging notes

- `app/build.gradle` stores `glb`, `ktx2` and `spv` uncompressed
  (`noCompress`), so the engine maps them straight out of the APK with
  `AAsset_getBuffer` (`IAssetSource::map`): clean, reclaimable pages instead
  of an inflated heap copy counted in the app's PSS.
- Textures live in `assets/ptv_textures/` (aapt drops asset folders whose
  name starts with `_`). Quest ships the KTX2; a texture without one ships its
  PNG/JPEG; a referenced file that is missing fails the build.
- The default hands' embedded textures get KTX2 twins under
  `engine/ptv_textures/<hash>.ktx2`.

## Validation ladder (on-device)

1. Build the starter-room scene as an APK (no LLM), `adb install -r`, confirm
  it boots on the Quest 3.
2. Run a generated project end-to-end to an APK on device.
3. Optimize: multiview single-pass stereo, FFR through the fragment density
   map, ASTC textures, two frames in flight (done; see
   [rendering.md](rendering.md) and
   [quest-optimization.md](quest-optimization.md)).



## Verified build (arm64-android)

The NDK build of `libptv_quest.so` has been validated end-to-end (configure +
compile + link + APK package + `adb install` + launch on a Quest 3, booting
through scene load, OpenXR + Vulkan + Jolt init, and into the render loop).
Notes captured while getting there:

- `spdlog` is pinned to `v1.15.3` (bundles fmt 11.2.0). Older spdlog (1.14.x ->
fmt 10.x) fails to compile with the NDK's modern clang: bundled fmt's
`FMT_STRING` triggers "call to consteval function ... is not a constant
expression" (fixed in fmt 11.2.0).
- JoltPhysics via FetchContent exposes the plain `Jolt` target (NOT the
`Jolt::Jolt` alias, which only exists in the vcpkg package used on desktop).
- `ANativeActivity_onCreate` (the NativeActivity entry point, provided by
`native_app_glue` and looked up by the framework via `dlsym`) MUST be
exported by `libptv_quest.so`. `native_app_glue` is linked with
`--whole-archive` and `-u ANativeActivity_onCreate` is passed via a `SHELL:`
link option (a plain quoted `"-u ANativeActivity_onCreate"` is delivered as
one malformed argument and silently does nothing -> `UnsatisfiedLinkError: undefined symbol: ANativeActivity_onCreate` at launch).
- The NDK is pinned via `Settings.android_ndk_version` -> `@PTV_NDK_VERSION@`
in `app/build.gradle`.
- Engine logs go to logcat on Android (spdlog android sink, tag `ptv`): watch
with `adb logcat -s ptv`. Native crashes are in the crash buffer:
`adb logcat -b crash -d`.
- Debug APKs build Jolt with assertions enabled, so an entity carrying a
non-normalized/zero quaternion or non-finite position aborts with SIGTRAP in
`JPH::Body::SetPositionAndRotationInternal`. `PhysicsWorld::rebuild_from_world`
sanitizes transforms (normalize rotation, guard position) before creating
bodies (on desktop Release the same bad data is silent UB).



## Asset size + performance

Raw Tripo GLBs are ~500k triangles / ~14 MB each, which is far past Meta's
whole-frame Quest 3 budget (1.3M-1.8M triangles) and produced multi-second
first-frame stalls (an ANR). The `asset_optimization` stage decimates every
asset into an LOD chain before packaging, and `quest_build` ships ONLY the
LODs (the raw source stays on disk for future use).

Measured on the iron-man reference project: 4.86M -> 239k triangles (20x) and
a 111 MB -> 20 MB APK. `_package_assets` wipes its staging directory each run,
so assets deleted or superseded upstream cannot linger in the APK.

## Known on-device iteration points

- Native XR bring-up: NativeActivity surface/focus timing and controller
re-acquire on resume. The VR frame loop only advances once the OpenXR session
reaches READY/FOCUSED, i.e. when the headset is actually worn (a doffed
headset pauses/stops the app).

