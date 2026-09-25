# Stage 16: Quest APK Build

> **Pipeline position:** after `scene_compile` → before `engine_promotion`  
> **Target:** Quest only (`target_device == quest`)  
> **Primary code:** [`apps/orchestrator/ptv_orchestrator/pipeline/stage_quest_build.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/stage_quest_build.py)  
> **Primary artifacts:** `projects/<project_id>/builds/quest/app-debug.apk`, `logs/gradle.log`

## Main stage summary

### What this stage is

The **Quest build stage** packages the same generated game (identical `scene.json` and C++ mechanics as PC VR) into a **standalone Android APK**. It instantiates the Gradle template, copies LOD GLBs and shaders into APK assets, and runs NDK build via `gradlew assembleDebug`.

PC VR projects use **`cmake_build`** instead; stages 1–15 are unchanged.

### Why it exists

Quest standalone play requires an Android binary with assets inside the APK, OpenXR Android loader init, and `NativeActivity` entry — not a Windows `.exe`. This stage is the device-specific packaging tail of the pipeline.

### Inputs (what enters)

| Input | Location |
|-------|----------|
| Scene | `projects/<id>/scene.json` |
| Optimized GLBs | `assets/<id>/*_lod*.glb` (raw Tripo sources skipped) |
| Engine template | `engine/android/` |
| Shaders | compiled to `.spv` via `glslc` |
| Generated C++ | same as PC path — linked into `libptv_quest.so` |

### Outputs (what it produces)

| Output | Location |
|--------|----------|
| Gradle project | `projects/<id>/builds/quest/` |
| Debug APK | `app/build/outputs/apk/debug/app-debug.apk` |
| Log | `logs/gradle.log` |
| State | `latest_build_apk` |

### Where output goes next

- **`quest_install`** (manual/API) — `adb install -r` + `am start` (mentioned here; not a separate doc folder)
- **`engine_promotion`** — same as PC VR after successful build

### Key concepts and terms

- **IAssetSource** — Engine seam: filesystem on desktop, `AAssetManager` on Android. Scene and mesh loaders use it so the same scene JSON works on both targets.
- **LOD-only packaging** — APK ships decimated LOD chain (~20× smaller than raw Tripo GLBs).
- **Gradle template tokens** — `@PTV_PACKAGE@`, `@PTV_ENGINE_DIR@`, etc. substituted per project.
- **libptv_quest.so** — Native library linking engine runtime + generated mechanics + OpenXR Android loader.

### End-to-end flow (plain language)

1. Copy `engine/android/` → `builds/quest/`, substitute tokens.
2. Wipe and repopulate `app/src/main/assets/` (no stale GLBs).
3. Copy `scene.json` + LOD GLBs + compiled shaders + engine hand fixtures.
4. Run `gradlew assembleDebug`.
5. Record APK path on success.

---

## Detailed stage deep dive

### PC VR vs Quest at stage 16

Stages 1–15 produce identical `scene.json`, generated C++, and LOD GLBs. Only stage 16 branches:

| Target | Stage | Output |
|--------|-------|--------|
| `pcvr` | `cmake_build` | Windows `.exe` via CMake/MSVC |
| `quest` | `quest_build` | Android APK via Gradle/NDK |

Generated mechanics use only public APIs (`IMechanic`, `World`, etc.) — no per-target codegen.

### Exact execution trace (`stage_quest_build.run`)

| Step | Function | Detail |
|------|----------|--------|
| 1 | emit + `record_stage(running)` | |
| 2 | Validate `scene.json` exists | Missing → fail immediately |
| 3 | Copy `engine/android/` → `builds/quest/` | `dirs_exist_ok=True` — template refreshed |
| 4 | `_substitute_tokens()` | Replaces `@PTV_*@` in Gradle, manifest, strings |
| 5 | `_package_assets()` | **Wipes** `app/src/main/assets/` then repopulates |
| 6 | Copy `scene.json` into assets | |
| 7 | Copy LOD GLBs only | Skips raw Tripo sources superseded by `_lod0..3` |
| 8 | `_compile_shaders()` | `glslc` GLSL → `.spv` under `assets/shaders/` |
| 9 | `_package_engine_assets()` | Runs `prepare_default_hands.py` on `Default_Models/default_rigged_hand_model.glb` → `default_hand_{left,right}.glb` in `assets/engine/` |
| 10 | `assemble_debug()` | Gradle + NDK builds `libptv_quest.so` + APK |
| 11 | `find_debug_apk()` | Locates `app-debug.apk` |
| 12 | Set `state.latest_build_apk`, succeed | Failure → RuntimeError + gradle.log |

### Token substitution (`_TOKENISED` files)

- `settings.gradle`
- `app/build.gradle`
- `app/src/main/AndroidManifest.xml`
- `app/src/main/res/values/strings.xml`

Tokens include package name (`quest_package_name`), app label from project title, engine dir path, SDK/NDK versions.

### Asset packaging rules (`_package_assets`)

- Assets directory **deleted every run** — prevents stale GLBs from prior pipeline attempts
- Only ships **LOD chain** files from optimization stage (~20× smaller than raw Tripo)
- `scene.json` at assets root — runtime `SceneLoader` reads via `AndroidAssetSource` / `AAssetManager`

### Shader compilation

`_compile_shaders()` searches `glslc` on PATH or `$VULKAN_SDK/Bin/glslc.exe`. If missing: warning only, shaders not packaged — APK builds but renderer fails on device (explicit signal).

### Native binary architecture

`libptv_quest.so` links:

- Shared engine runtime (Vulkan, OpenXR Android loader, ECS, Jolt, mechanics)
- This project's `engine/generated/**/<project_id>/` objects
- Platform seam: `engine/runtime/src/platform/android/` (`android_main`, asset source)

Same generated C++ as PC; different platform entry and IAssetSource implementation.

### Engine-owned vs project-owned assets

| Asset | Source |
|-------|--------|
| Game GLBs + scene.json | Project `assets/` + `scene.json` |
| Hand rig GLBs | Generated at build time from committed script |
| SPIR-V shaders | Compiled from `engine/runtime/shaders/` |

### Gradle log and failure

Output streamed to `projects/<id>/logs/gradle.log` and SSE `log` events. Non-zero gradle exit → stage failed, message includes excerpt.

### Post-build: `quest_install` (not a doc folder)

Orchestrator launch path: `adb install -r app-debug.apk` + `am start` NativeActivity. Debug APK is auto-signed. Meta Store submission out of scope.

### SSE events

`running` → `info` (packaging notes, glslc/hand warnings) → `log` (gradle lines) → `succeeded` | `failed`

### Settings

`android_*` paths, `android_openxr_loader_version`, `android_package_prefix` — see [`docs/quest-build.md`](../../../quest-build.md).

### Comparison to `cmake_build`

| Aspect | cmake_build | quest_build |
|--------|-------------|---------------|
| Build tool | CMake + MSVC | Gradle + NDK |
| Output | `<Title>.exe` | `app-debug.apk` |
| Assets | Filesystem paths | Bundled in APK |
| Shaders | Desktop loader | `.spv` in APK assets |
| State field | `latest_build_exe` | `latest_build_apk` |

---

## Extend this folder

- `artifacts.md` — APK asset layout
- `failure-modes.md` — Gradle/NDK troubleshooting

## See also

- [`docs/quest-build.md`](../../../quest-build.md) — authoritative Quest path reference
- PC stage 16 [`16_cmake_build`](../16_cmake_build/README.md)
- [`docs/architecture.md`](../../../architecture.md) — platform split
