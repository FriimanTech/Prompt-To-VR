# Stage 16: CMake Build (PC VR)

> **Pipeline position:** after `scene_compile` → before `engine_promotion`  
> **Target:** PC VR only (`target_device == pcvr`)  
> **Primary code:** [`apps/orchestrator/ptv_orchestrator/pipeline/stage_cmake.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/stage_cmake.py), [`build/cmake_runner.py`](../../../../apps/orchestrator/ptv_orchestrator/build/cmake_runner.py)  
> **Primary artifacts:** `projects/<project_id>/builds/cmake-current/`, `projects/<project_id>/logs/cmake.log`, `<Title>.exe`

## Main stage summary

### What this stage is

The CMake build stage configures and compiles the native Windows VR executable from the engine source tree (`engine/`). The result contains the hand-authored runtime and this project's generated C++. The executable is the shared `ptv_starter_room` host, renamed to the game's title, and it loads the project's `scene.json` at launch. Quest projects run [`quest_build`](../../Quest_Stages/16_quest_build/README.md) instead.

### Why it exists

Earlier stages produce JSON, GLBs and C++ sources. The player needs a runnable `.exe` launched through OpenXR (Quest Link). The build directory is stable across runs, so vcpkg packages and object files are reused instead of being rebuilt cold on every resume.

### Inputs

| Input | Location |
|-------|----------|
| Engine CMake root | `Settings.engine_dir` (`engine/`) with vcpkg manifest `engine/vcpkg.json` |
| Generated C++ | `engine/generated/**/<project_id>/`, selected by `PTV_GENERATED_PROJECT_ID` |
| Promoted sources the plan reuses | from `promoted_sources_for_plan()` → `PTV_GENERATED_EXTRA_SOURCES` |
| Scene | `projects/<id>/scene.json` (also the source of the exe name via its `title`) |

CMake cache variables passed by the stage:

| Variable | Value |
|----------|-------|
| `PTV_GENERATED_PROJECT_ID` | project id (only this game's generated code is compiled, so two games' same-named tasks cannot collide) |
| `PTV_GENERATED_EXTRA_SOURCES` | `;`-joined absolute paths of reused promoted `.cpp` files from other projects |
| `PTV_SCENE_JSON` | forward-slash path to `scene.json` |
| `PTV_PROJECT_DIR` | forward-slash project workspace |
| `PTV_APP_NAME` | executable base name (see below) |
| `CMAKE_TOOLCHAIN_FILE` | `<vcpkg_root>/scripts/buildsystems/vcpkg.cmake`, only when `vcpkg_root` is set |

### Outputs

| Output | Location |
|--------|----------|
| Build tree | `projects/<id>/builds/cmake-current/` (stable; shared with codegen's compile-verify) |
| Log | `projects/<id>/logs/cmake.log` (truncated each run; the stage artifact) |
| Executable | `builds/cmake-current/samples/starter-room/<Release|Debug>/<Title>.exe` |
| State | `latest_build_exe`, `latest_build_dir`, `current_stage = ready_to_launch`, `status = succeeded` |
| Stage records | `cmake_configure` and `cmake_build`, recorded separately |

### Where output goes next

- **`engine_promotion`** (Stage 17) runs next.
- **Launch:** `POST /projects/{project_id}/launch` runs `latest_build_exe` with the project's scene through OpenXR (Quest Link).

### Failure behavior

- **Configure** counts as succeeded only if the log contained `Build files have been written to`. Otherwise `cmake_configure` is `failed`, with the first recognized error line, an "interrupted" message, or "see logs/cmake.log".
- **Build** counts as succeeded when configure succeeded and the exit code is 0. Otherwise `cmake_build` is `failed`, with the first error line or an "interrupted, Resume to retry" message.
- If cmake is killed externally (`STATUS_CONTROL_C_EXIT`, `0xC000013A` / `-1073741510`) **before configure finished**, the stage truncates the log and retries automatically **once**.
- The stage **does not raise** on a build failure. It records the failed stages and returns a `StageResult`, and the orchestrator's stage status reflects the failure. On success with no exe found, state is not moved to `ready_to_launch`.

---

## Detailed stage deep dive

### Execution trace (`stage_cmake.run`)

1. Emit `cmake_configure running`, `record_stage(cmake_configure, running)`.
2. `build_dir = projects/<id>/builds/cmake-current`; `app_name = _project_app_name(ctx)`.
3. Truncate `logs/cmake.log`.
4. `_run_once()` → `configure_and_build(source_dir=engine_dir, build_dir, generator, platform, config, vcpkg_root, extra_cache, on_line=_sink, **proc_hooks(ctx))`.
5. Retry once on Ctrl+C exit before configure finished.
6. Record `cmake_configure` and `cmake_build` (artifact: `logs/cmake.log`), then emit `cmake_build succeeded|failed` with `exit=<code> :: <first error>`.
7. On success, `_find_exe()`. If found, set state and emit `ready_to_launch ready <exe name>`.

### `configure_and_build()` (`build/cmake_runner.py`)

1. Configure: `cmake -S engine -B build_dir -G "<cmake_generator>" -A <cmake_generator_platform> -D<cache vars…> [-DCMAKE_TOOLCHAIN_FILE=…]`.
2. Build: `cmake --build build_dir --config <cmake_build_config> --parallel`.
3. cmake runs in its own process group so a uvicorn reload or Ctrl+C in the parent does not kill it. `proc_hooks` registers the child so Abort kills the whole tree (cmake → msbuild → compilers).

The stage does **not** use `engine/CMakePresets.json`. Those presets (`vs2026-x64` with toolset `v145`, `ninja-x64`) are for manual builds; see [`docs/build-and-run.md`](../../../build-and-run.md). The stage passes no `-T` toolset, so Visual Studio uses the generator's default toolset (v145 for VS 18 2026).

### Executable name (`_project_app_name`)

The name comes from `scene.json`'s `title`, falling back to the project id. Characters Windows forbids (`<>:"/\|?*`, control chars) become spaces, whitespace is collapsed, and trailing dots and spaces are trimmed. `samples/starter-room/CMakeLists.txt` sets `OUTPUT_NAME` to `PTV_APP_NAME` when it is defined.

### Finding the exe (`_find_exe`)

The stage looks for these, in order:

1. `samples/starter-room/<config>/<Title>.exe`, then the other config
2. `ptv_starter_room.exe` in both configs
3. `apps/ptv_game/<config|other>/ptv_game.exe`
4. any `*.exe` under the build dir except `CompilerIdCXX.exe`, `ptv_smoke.exe` and `ptv_glb_validate.exe`

### Log routing

Every line goes to `cmake.log`. Lines are emitted as `log` events on `cmake_configure` until `Build files have been written to` is seen, then on `cmake_build`. Error excerpts are collected from lines containing `: error `, `error c` or `fatal error`, and lines starting `error:` or `cmake error`.

### Settings (`settings.py`, `.env`, no prefix)

| Setting | Env var | Default |
|---------|---------|---------|
| `cmake_generator` | `CMAKE_GENERATOR` | `Visual Studio 18 2026` |
| `cmake_generator_platform` | `CMAKE_GENERATOR_PLATFORM` | `x64` |
| `cmake_build_config` | `CMAKE_BUILD_CONFIG` | `Release` |
| `vcpkg_root` | `VCPKG_ROOT` | unset (no toolchain file is passed) |

`CMAKE_GENERATOR` and `CMAKE_GENERATOR_PLATFORM` are also environment variables cmake reads itself. The stage passes `-G`/`-A` explicitly, so the values agree either way.

### PC VR vs Quest at stage 16

| Aspect | `cmake_build` | `quest_build` |
|--------|---------------|---------------|
| Build tool | CMake + MSVC | Gradle + NDK |
| Output | `<Title>.exe` | `app-debug.apk` |
| Assets | read from the project directory | bundled in the APK |
| Next stage | `engine_promotion` | `quest_runtime_verify` |
| State field | `latest_build_exe` | `latest_build_apk` |

## See also

- [`docs/build-and-run.md`](../../../build-and-run.md): toolchain and manual builds
- [`docs/engine-growth.md`](../../../engine-growth.md): promoted sources in the build
- Quest counterpart: [`16_quest_build`](../../Quest_Stages/16_quest_build/README.md)
- Next: [`17_engine_promotion`](../17_engine_promotion/README.md)
