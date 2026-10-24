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
     quest: quest_build (APK)       -> adb install -r + am start NativeActivity
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

## Validation ladder (on-device)

1. Build the starter-room scene as an APK (no LLM), `adb install -r`, confirm
  it boots on the Quest 3.
2. Run a generated project end-to-end to an APK on device.
3. Optimize: multiview single-pass stereo, `XR_FB_foveation`, ASTC textures.



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

