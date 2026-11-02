# Stage 16: Quest APK Build

> **Pipeline position:** after `scene_compile` → before `quest_runtime_verify`  
> **Target:** Quest only (`target_device == quest`)  
> **Primary code:** [`apps/orchestrator/ptv_orchestrator/pipeline/stage_quest_build.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/stage_quest_build.py)  
> **Primary artifacts:** `projects/<project_id>/builds/quest/app/build/outputs/apk/debug/app-debug.apk`, `projects/<project_id>/logs/gradle.log`

## Main stage summary

### What this stage is

The Quest build stage packages the game that stages 1–15 produced (the same `scene.json`, the same generated C++ and the same LOD GLBs as a PC VR project) into a **standalone Android debug APK**. It instantiates the Gradle + NDK template in `engine/android/`, stages the scene documents, LOD GLBs, KTX2 textures, compiled SPIR-V shaders and engine-owned assets (default rigged hands, UI font) into the APK's `assets/`, and runs `gradlew assembleDebug`.

PC VR projects run [`cmake_build`](../../PC_VR_Stages/16_cmake_build/README.md) instead. The Quest tail is `quest_build` → `quest_runtime_verify` → `engine_promotion`.

### Why it exists

A standalone Quest needs an Android binary (`libptv_quest.so` behind a `NativeActivity`) with its assets inside the APK and the Khronos OpenXR Android loader, not a Windows `.exe`. This is the device-specific packaging step; nothing about the game is decided here.

### Inputs

| Input | Location |
|-------|----------|
| Entry scene | `projects/<id>/scene.json` |
| Other scenes (multi-scene games) | `projects/<id>/scene.*.json` |
| Optimized GLBs | `projects/<id>/assets/**/*.glb` (raw sources superseded by an `_lod0` sibling are skipped) |
| Shared textures | KTX2 (or PNG/JPEG fallback) files the packaged GLBs reference, typically `assets/ptv_textures/` |
| Gradle template | `engine/android/` |
| Shaders | `engine/runtime/shaders/*.{vert,frag,comp}` → `glslc` |
| Engine assets | `Default_Models/default_rigged_hand_model.glb`, `engine/runtime/assets/fonts/` |
| Generated C++ | `engine/generated/**/<project_id>/` plus promoted sources the mechanic plan reuses |
| Project title | `ProjectState.title` (app label) |

### Outputs

| Output | Location |
|--------|----------|
| Instantiated Gradle project | `projects/<id>/builds/quest/` |
| APK assets staging | `projects/<id>/builds/quest/app/src/main/assets/` |
| Native CMake staging tree | `<repo>/.cxx/<8-char sha256 of project id>/` |
| Debug APK | `builds/quest/app/build/outputs/apk/debug/app-debug.apk` |
| Log | `projects/<id>/logs/gradle.log` (truncated each run) |
| State | `ProjectState.latest_build_apk`, `latest_build_dir`; stage artifact = repo-relative APK path |

### Where output goes next

- **`quest_runtime_verify`** (Stage 16b) installs and runs this APK on a connected headset. Its memory correction loop can call this stage again.
- **`engine_promotion`** (Stage 17) runs after verification.
- **Download:** `GET /projects/{project_id}/apk` serves `latest_build_apk` as `<project_id>.apk` whatever its verification or memory status, with the memory status (from `runtime_verification.json`, else `unknown`) in the `X-PTV-Memory-Status` header.
- **Launch:** `POST /projects/{project_id}/launch` on a Quest project runs `adb install -r` + `am start` (`api/launch.py: install_and_launch_apk`).

### Failure behavior

The stage records `failed` and raises `RuntimeError` when:

- `engine/android/` does not exist;
- a packaged GLB references a texture file that is missing or lies outside the project assets (lists up to 10; "re-run asset_optimization");
- any **NPC** mesh (skinned, or tagged `npc`/`enemy`/`ally`/`neutral`/`critter`/`boss`/`template`) referenced by a packaged scene is not in the APK assets. This is checked **before** Gradle runs. Missing non-NPC meshes only produce `warning` events;
- Gradle exits non-zero or no APK is found. If clang printed `: error:` lines, the message quotes the first three; otherwise it points at `logs/gradle.log` and `engine/android/README.md`.

Warnings that do **not** fail the stage: `glslc` not found (shaders not packaged, so the renderer fails on the device), the font directory missing (UI text invisible), hand preparation failing (the runtime falls back to procedural block hands), tools for hand-texture ASTC twins not built (PNG decoded on load), and textures without a KTX2 (decoded on the headset).

---

## Detailed stage deep dive

### Execution trace (`stage_quest_build.run`)

| Step | What happens |
|------|--------------|
| 1 | Emit `running`, `record_stage(running)` |
| 2 | Fail if `engine/android/` is missing |
| 3 | `shutil.copytree(template, builds/quest, dirs_exist_ok=True)`: the template is refreshed over the previous copy, so the Gradle cache survives |
| 4 | Create the short native staging dir `_native_staging_dir()` → `<repo>/.cxx/<sha256(project_id)[:8]>` |
| 5 | `_substitute_tokens()` over the tokenised files |
| 6 | `_package_assets()`: wipe and repopulate `app/src/main/assets/` (details below); each note is emitted as `info` |
| 7 | `missing_packaged_meshes()`: NPC meshes missing → fail; others → `warning` |
| 8 | Truncate `logs/gradle.log`, run `assemble_debug()` (`build/gradle_runner.py`) |
| 9 | `find_debug_apk()`: first `*.apk` in `app/build/outputs/apk/debug/`, else any APK under the build tree |
| 10 | On success, set `latest_build_apk` / `latest_build_dir`, record `succeeded` with message `APK: <name>` |

### Token substitution

Files: `settings.gradle`, `app/build.gradle`, `app/src/main/AndroidManifest.xml`, `app/src/main/res/values/strings.xml`.

| Token | Value |
|-------|-------|
| `@PTV_PACKAGE@` | `quest_package_name(android_package_prefix, project_id)` = `<prefix>.<sanitized id>` |
| `@PTV_APP_LABEL@` | project title, else project id |
| `@PTV_ENGINE_DIR@` | `Settings.engine_dir` (forward slashes) |
| `@PTV_PROJECT_ID@` | project id |
| `@PTV_GENERATED_EXTRA_SOURCES@` | `;`-joined promoted sources from `generated_sources_cache()` |
| `@PTV_CXX_DIR@` | the short `.cxx` staging dir |
| `@PTV_MIN_SDK@` / `@PTV_TARGET_SDK@` / `@PTV_COMPILE_SDK@` | `android_min_sdk` / `android_target_sdk` / `android_compile_sdk` |
| `@PTV_LOADER_VERSION@` | `android_openxr_loader_version` |
| `@PTV_NDK_VERSION@` | `android_ndk_version` |

The `.cxx` relocation exists because ninja still enforces Windows' 260-character `MAX_PATH`; the default `builds/quest/app/.cxx` plus FetchContent stamp paths overflowed it.

### Asset packaging (`_package_assets`)

1. **Delete** `app/src/main/assets/` so the APK mirrors the project's current files (stale raw Tripo GLBs once added 141 MB to a 123 MB APK).
2. Copy `scene.json`, then every `scene.*.json` sibling. Portals resolve through the scene index relative to `scene.json`, so siblings must sit beside it.
3. Copy every GLB under `projects/<id>/assets/` to `assets/assets/<rel>`, skipping a non-LOD GLB whose directory holds a `*_lod0.glb` (`_is_superseded_by_lods`). At runtime `AndroidAssetSource` strips the `projects/<id>/` prefix.
4. `_package_textures()`: for each external texture reference in a packaged GLB (`glb_texture_refs`), ship the KTX2 when there is one, else the PNG/JPEG (counted as "decoded on the headset"). Reports file count and MB.
5. `_compile_shaders()`: `glslc -O` from PATH or `$VULKAN_SDK/Bin/glslc.exe` → `assets/shaders/<name>.spv`.
6. `_package_engine_assets()` → `assets/engine/` (matches `PTV_ENGINE_ASSET_DIR="engine"` in the Android CMakeLists):
   - `.ttf` / `.txt` files from `engine/runtime/assets/fonts/` → `assets/engine/fonts/` (end screens, menus, HUD);
   - `engine/tools/prepare_default_hands.py --input Default_Models/default_rigged_hand_model.glb --out-dir assets/engine` → left/right default hands;
   - `_embedded_texture_twins()`: with the newest built `ptv_glb_optimize` and `ptv_texture_compress` under `engine/build/`, writes ASTC 6x6 (max 1024) twins as `assets/engine/ptv_textures/<hash>.ktx2` for images embedded in the hand GLBs. The folder has no leading underscore because aapt drops `_*` directories.

### Gradle template (`engine/android/`)

- `app/build.gradle`: `abiFilters 'arm64-v8a'` only; CMake args `-DANDROID_STL=c++_shared`, `-DPTV_ENGINE_DIR`, `-DPTV_PROJECT_ID`, `-DPTV_GENERATED_EXTRA_SOURCES`, `-DPTV_APP_LIB_NAME=ptv_quest`, `-std=c++20`; `prefab true` for the OpenXR loader AAR (`org.khronos.openxr:openxr_loader_for_android:<version>`); `buildStagingDirectory '@PTV_CXX_DIR@'`.
- The **debug** build type is debug-signed and `debuggable`, but the native code is built **`RelWithDebInfo`**: `-O0` cannot hold 72–90 Hz, and this also compiles out Jolt asserts (SIGTRAP on device).
- `androidResources { noCompress 'glb', 'ktx2', 'spv' }`: those assets are stored uncompressed so `AAsset_getBuffer` can memory-map them as clean, reclaimable pages instead of private PSS.
- `app/src/main/cpp/CMakeLists.txt` builds `native_app_glue` from the NDK, `find_package(OpenXR REQUIRED CONFIG)`, an optional `ptv_generated` static library and the shared `ptv_quest` library, linked with `-u ANativeActivity_onCreate`.
- `AndroidManifest.xml`: required `android.hardware.vr.headtracking`, `com.oculus.supportedDevices` meta-data, and a launcher activity with `com.oculus.intent.category.VR`.

### Gradle runner (`build/gradle_runner.py`)

`find_gradle()` prefers the project-local `gradlew.bat` / `gradlew` and falls back to `gradle` on PATH. It runs `assembleDebug --console=plain --stacktrace` in its own Windows process group (`CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW`), so a parent Ctrl+C or uvicorn reload does not kill it. `proc_hooks(ctx)` registers the child so Abort can kill the tree. The environment is inherited (`ANDROID_HOME`, `ANDROID_NDK_HOME`, `JAVA_HOME`).

### Logs and events

Every line goes to `logs/gradle.log`. Only notable lines are streamed as `log` events (`is_notable_gradle_line`): `[n/m]` progress, `> Task`, `BUILD`, `FAILURE`/`FAILED`, `* What went wrong`, `Execution failed`, and `: error:` / `: fatal error:` / `: warning:`. Up to six distinct compile errors are collected for the failure message.

Event sequence: `running` → `info` (packaging notes) → `warning` (unpackaged non-NPC meshes) → `log` → `succeeded` | `failed`.

### Settings (`apps/orchestrator/ptv_orchestrator/settings.py`, from `.env` with no prefix)

| Setting | Env var | Default | Use |
|---------|---------|---------|-----|
| `android_package_prefix` | `ANDROID_PACKAGE_PREFIX` | `com.prompttovr` | applicationId prefix |
| `android_min_sdk` | `ANDROID_MIN_SDK` | `32` | `minSdk` |
| `android_target_sdk` | `ANDROID_TARGET_SDK` | `34` | `targetSdk` |
| `android_compile_sdk` | `ANDROID_COMPILE_SDK` | `34` | `compileSdk` |
| `android_openxr_loader_version` | `ANDROID_OPENXR_LOADER_VERSION` | `1.0.34` | Khronos loader AAR (Meta requires ≥ 1.0.34) |
| `android_ndk_version` | `ANDROID_NDK_VERSION` | `30.0.15729638` | pinned `ndkVersion` (must be under `$ANDROID_HOME/ndk`) |
| `android_sdk_root` / `android_ndk_home` | `ANDROID_SDK_ROOT` / `ANDROID_NDK_HOME` | unset | declared overrides; this stage does not read them (Gradle uses the inherited environment) |

### PC VR vs Quest at stage 16

| Aspect | `cmake_build` | `quest_build` |
|--------|---------------|---------------|
| Build tool | CMake + MSVC (VS 18 2026) | Gradle + NDK (clang, arm64) |
| Output | `<Title>.exe` | `app-debug.apk` |
| Assets | read from the filesystem | bundled in the APK, GLB/KTX2/SPV uncompressed |
| Native config | `cmake_build_config` (Release) | RelWithDebInfo |
| Next stage | `engine_promotion` | `quest_runtime_verify` |
| State field | `latest_build_exe` | `latest_build_apk` |

## See also

- [`docs/quest-build.md`](../../../quest-build.md): Quest toolchain setup and APK layout
- [`docs/quest-optimization.md`](../../../quest-optimization.md): memory and performance targets
- [`docs/build-and-run.md`](../../../build-and-run.md)
- Next stage: [`16b_quest_runtime_verify`](../16b_quest_runtime_verify/README.md)
- PC counterpart: [`16_cmake_build`](../../PC_VR_Stages/16_cmake_build/README.md)
- [`engine/android/README.md`](../../../../engine/android/README.md)
