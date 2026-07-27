# Quest (standalone) build path

Prompt-To-VR can build a generated game for two targets, chosen at create time
via `BuildSpec.target_device` / the PC VR vs Quest toggle in the New Project UI:

- **`pcvr`** (default): a Windows `.exe` (Vulkan + Win32 OpenXR) run on the PC
  and viewed on the headset over Quest Link. Built by `stage_cmake`.
- **`quest`**: a standalone Android APK (Vulkan + Android OpenXR loader,
  NativeActivity) installed and run locally on the Quest 3. Built by
  `stage_quest_build` and installed with `adb`.

Stages 1-9 (BuildSpec -> ... -> scene compile, including C++ codegen and world
generation) are **identical** for both targets. Only the build + launch stages
branch on the device. Generated mechanics use only `IMechanic` / `World` /
`Math`, so no per-target codegen changes are needed.

```
prompt + device toggle
  -> BuildSpec.target_device
  -> shared stages 1-9 (scene.json + generated C++ + GLBs)
  -> pcvr:  cmake_build (.exe)      -> launch_exe (Quest Link)
     quest: quest_build (APK)       -> adb install -r + am start NativeActivity
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
`scene.json` + GLBs + compiled `.spv` shaders into `app/src/main/assets/`. The
native `.so` (`libptv_quest.so`) is built by
`app/src/main/cpp/CMakeLists.txt` (NDK), linking the shared engine runtime +
this project's generated mechanics + the Khronos OpenXR Android loader.

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
  one malformed argument and silently does nothing -> `UnsatisfiedLinkError:
  undefined symbol: ANativeActivity_onCreate` at launch).
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

## Known on-device iteration points

- Native XR bring-up: NativeActivity surface/focus timing and controller
  re-acquire on resume. The VR frame loop only advances once the OpenXR session
  reaches READY/FOCUSED, i.e. when the headset is actually worn (a doffed
  headset pauses/stops the app).
