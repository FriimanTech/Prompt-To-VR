# engine/android - Quest (standalone Android) APK template

This directory is a **template**, not a standalone project. The orchestrator's
`quest_build` stage (`apps/orchestrator/ptv_orchestrator/pipeline/stage_quest_build.py`)
copies it into `projects/<id>/builds/quest/`, substitutes the `@PTV_*@` tokens,
copies the project's `scene.json` + GLBs + compiled `.spv` shaders into
`app/src/main/assets/`, and then runs `gradlew assembleDebug`.

## Token substitutions

- `@PTV_PACKAGE@`      applicationId / namespace, e.g. `com.prompttovr.<sanitized_id>`
- `@PTV_APP_LABEL@`    the game title shown on the headset
- `@PTV_ENGINE_DIR@`   absolute path to `<repo>/engine`
- `@PTV_PROJECT_ID@`   project id (selects `engine/generated/**/<id>`)
- `@PTV_MIN_SDK@`      min SDK (32 for Quest 2/3)
- `@PTV_TARGET_SDK@`   target SDK (34, store-compliant)
- `@PTV_COMPILE_SDK@`  compile SDK (>= target)
- `@PTV_LOADER_VERSION@` Khronos OpenXR Android loader (>= 1.0.34)

## What builds

`src/main/cpp/CMakeLists.txt` compiles the shared engine runtime (minus the
desktop platform dir) + this project's generated mechanics into
`libptv_quest.so`, links the Khronos OpenXR loader (Prefab AAR), Vulkan, and
`android_native_app_glue`. Entry point is `android_main` in
`engine/runtime/src/platform/android/AndroidMain.cpp`.

## Prerequisites (already installed on this machine)

Android Studio SDK/NDK, `ANDROID_HOME`, `ANDROID_NDK_HOME`, `JAVA_HOME`,
`platform-tools` (adb) and `cmdline-tools/latest/bin` on PATH.

## Gradle wrapper

The Gradle 8.9 wrapper IS vendored here (`gradlew`, `gradlew.bat`,
`gradle/wrapper/gradle-wrapper.jar`, `gradle/wrapper/gradle-wrapper.properties`)
and is copied into each project's `builds/quest/` by `stage_quest_build.py`. No
system Gradle install is required: `gradlew` self-bootstraps by downloading the
Gradle 8.9 distribution using `JAVA_HOME` (the Android Studio JBR is fine). The
version is pinned to Gradle 8.9 because the template's AGP 8.7 requires a minimum
of Gradle 8.9. `gradle_runner.find_gradle()` prefers this wrapper and only falls
back to a `gradle` on PATH if the wrapper is missing.

## Build status (arm64-android)

The NDK build of `libptv_quest.so` is validated (configure + compile + link +
APK install + launch on Quest 3). Key pins:

- `spdlog` `v1.15.3` (bundles fmt 11.2.0) -- older spdlog/fmt fails to compile
  with the NDK's modern clang (`FMT_STRING` consteval error).
- JoltPhysics FetchContent target is plain `Jolt` (not `Jolt::Jolt`).
- NDK pinned via `Settings.android_ndk_version` (`@PTV_NDK_VERSION@`).
- Vulkan is linked explicitly (`find_library(vulkan)`) alongside volk.
- Generated mechanics compile into a `ptv_generated` static lib force-linked
  with `--whole-archive` so `PTV_REGISTER_*` initializers survive.

Engine logs go to logcat (spdlog android sink, tag `ptv`): `adb logcat -s ptv`.

Native XR bring-up (NativeActivity surface/focus timing, controller re-acquire
on resume) still needs the headset worn to reach a FOCUSED session.
