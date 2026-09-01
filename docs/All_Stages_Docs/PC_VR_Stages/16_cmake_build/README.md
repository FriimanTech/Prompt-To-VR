# Stage 16: CMake Build (PC VR)

> **Pipeline position:** after `scene_compile` → before `engine_promotion`  
> **Target:** PC VR only (`target_device == pcvr`)  
> **Primary code:** [`apps/orchestrator/ptv_orchestrator/pipeline/stage_cmake.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/stage_cmake.py)  
> **Primary artifacts:** `projects/<project_id>/builds/cmake-current/`, `logs/cmake.log`, `<Title>.exe`

## Main stage summary

### What this stage is

The **CMake build stage** configures and compiles the native Windows VR executable. It links the hand-authored engine runtime, this project's generated C++ mechanics, and loads `scene.json` at launch. Quest projects skip this stage and use **`quest_build`** instead.

### Why it exists

Planning and asset stages produce JSON and source files; the player needs a runnable `.exe`. CMake + MSVC turn the engine tree into a binary you launch through OpenXR (Quest Link on PC VR).

This stage reuses the **same build directory** as codegen's compile-verify step (`builds/cmake-current`) so vcpkg packages and object files are incremental, not cold every time.

### Inputs (what enters)

| Input | Location |
|-------|----------|
| Scene | `projects/<id>/scene.json` |
| Generated C++ | `engine/generated/**/<project_id>/` |
| Engine source | `engine/` (CMake root) |

CMake cache variables set by orchestrator:

- `PTV_SCENE_JSON` — path to scene file
- `PTV_GENERATED_PROJECT_ID` — project id namespace
- `PTV_PROJECT_DIR` — project workspace
- `PTV_APP_NAME` — exe base name from scene title

### Outputs (what it produces)

| Output | Location |
|--------|----------|
| Build tree | `projects/<id>/builds/cmake-current/` |
| Log | `projects/<id>/logs/cmake.log` (truncated each run) |
| Executable | `<Title>.exe` or `ptv_starter_room.exe` under build dir |
| State | `latest_build_exe`, `ready_to_launch` marker |

### Where output goes next

- **Launch** — orchestrator runs exe via OpenXR (Quest Link)
- **`engine_promotion`** — runs after successful build

### Key concepts and terms

- **CMake configure vs build** — Two phases recorded separately (`cmake_configure`, `cmake_build`). Configure generates project files and runs vcpkg; build invokes MSVC to compile/link.
- **Stable build dir** — `cmake-current` preserves vcpkg install between resumes (avoids re-downloading ~22 packages).
- **ptv_starter_room** — Shared runtime host target; loads whichever `scene.json` you pass. Per-project exe name comes from scene title.
- **STATUS_CONTROL_C_EXIT** — Windows exit code when cmake was killed externally (uvicorn reload, Ctrl+C). Stage auto-retries configure once.

### End-to-end flow (plain language)

1. Truncate `cmake.log` for clean output.
2. Run `configure_and_build()` with VS 2026 generator and vcpkg manifest.
3. Stream log lines to UI (configure vs build stages).
4. On success, locate exe, set `ready_to_launch`.
5. On failure, record which phase failed with first error line excerpt.

---

## Detailed stage deep dive

### Sub-stages inside one module

`stage_cmake.run()` records both **`cmake_configure`** and **`cmake_build`** separately in project_state so UI shows whether vcpkg/configure failed vs C++ compile/link failed.

### Stable build directory

`projects/<id>/builds/cmake-current/` — reused across resumes. Preserves vcpkg install + CMakeCache (avoids re-downloading ~22 packages).

`logs/cmake.log` **truncated** at start of each run — only current attempt visible.

### CMake cache variables set

| Variable | Value |
|----------|-------|
| `PTV_GENERATED_PROJECT_ID` | project id |
| `PTV_SCENE_JSON` | forward-slash path to `scene.json` |
| `PTV_PROJECT_DIR` | project workspace |
| `PTV_APP_NAME` | sanitized title from scene.json |

### `configure_and_build()` flow

1. CMake configure with VS 2026 generator + vcpkg manifest
2. Log sink watches for `"Build files have been written to"` → switches SSE routing from configure to build phase
3. Collect error lines (`: error `, `fatal error`, etc.) for user message
4. On `STATUS_CONTROL_C_EXIT` before configure done: **retry once** with fresh log (uvicorn reload / Ctrl+C kill)

### Success path

1. `_find_exe(build_dir, config, app_name)` — prefers `<Title>.exe` under `samples/starter-room/<Release|Debug>/`
2. Sets `state.latest_build_exe`, `latest_build_dir`
3. Sets `current_stage = ready_to_launch`, emits `ready_to_launch`

Exe is shared `ptv_starter_room` host loading project's scene.json at launch.

### Quest counterpart

Quest projects use `stage_quest_build` instead — identical scene.json and generated C++, different packaging.

See [`docs/build-and-run.md`](../../../build-and-run.md).

### Orchestrator project lock

Prevents concurrent pipeline runs from two cmake processes racing vcpkg install lock on same build dir.

---

## Extend this folder

- `failure-modes.md` — cmake.log reading guide
- `settings.md` — generator and vcpkg paths

## See also

- [`docs/build-and-run.md`](../../../build-and-run.md) — manual build instructions
- [`docs/quest-build.md`](../../../quest-build.md) — Quest counterpart
- Quest stage 16 [`16_quest_build`](../../Quest_Stages/16_quest_build/README.md)
