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
native debugging (logcat / GPU profiling).

## Validation ladder (on-device)

1. Build the starter-room scene as an APK (no LLM), `adb install -r`, confirm
   it boots on the Quest 3.
2. Run a generated project end-to-end to an APK on device.
3. Optimize: multiview single-pass stereo, `XR_FB_foveation`, ASTC textures.

## Known first-build iteration points

- The arm64-android dependency build (`app/src/main/cpp/CMakeLists.txt` uses
  FetchContent for glm/EnTT/nlohmann/VMA/volk/tinygltf/spdlog/Jolt); Jolt and
  spdlog versions are the most likely to need pinning.
- Native XR bring-up: NativeActivity surface/focus timing and controller
  re-acquire on resume.
- A Gradle wrapper (`gradlew`) is not vendored in the template; generate one
  once (`gradle wrapper --gradle-version 8.9`) or rely on a system `gradle`.
