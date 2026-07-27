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

The `gradlew` / `gradle/wrapper` files are NOT vendored here; generate them once
with a locally installed Gradle (`gradle wrapper --gradle-version 8.9`) or let
the orchestrator invoke a system `gradle`. See `stage_quest_build.py`.

## First-build iteration

The arm64-android dependency build (Jolt/spdlog versions) and native XR
bring-up (surface/focus timing, controller re-acquire on resume) are expected
to need on-device iteration. Validate with the starter-room scene first.
