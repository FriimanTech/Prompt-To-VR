# Build and run

For the full 17-stage pipeline (including Quest APK builds), see
[`All_Stages_Docs/README.md`](All_Stages_Docs/README.md).

## The hand-authored sample (no API keys required)

```powershell
# 1. Configure + build the engine.
cd engine
cmake --preset vs2026-x64
cmake --build --preset vs2026-x64-release

# 2. Make sure the Quest 3 is connected.
#    - Start the Meta Quest Link desktop app.
#    - Settings > Beta > "Set Oculus as active OpenXR runtime".
#    - Put the headset on and enter the Quest Link (or Air Link) environment.

# 3. Launch the native VR sample.
.\build\vs2026-x64\samples\starter-room\Release\ptv_starter_room.exe
```

On success:

- a desktop *mirror* window opens (`ptv_starter_room [mirror]`),
- the headset shows the OpenXR runtime's stage,
- the on-mirror ImGui panel shows `Mirror: 1280x720` and `ECS entities: N`.

## A generated project

1. Start the orchestrator:

   ```powershell
   .\vrvenv\Scripts\Activate.ps1
   uvicorn ptv_orchestrator.main:app --host 127.0.0.1 --port 8787 --reload
   ```

2. Start the web control panel in another terminal:

   ```powershell
   cd apps\web
   npm run dev
   ```

3. Open <http://127.0.0.1:5173>, click *New project*, paste a prompt, and
   press *Create & run pipeline*. The *Pipeline* tab shows per-stage progress
   in real time.

4. When the pipeline reaches *Ready to launch*, press *Launch latest build*.
   The FastAPI endpoint `/api/projects/<id>/launch` spawns the built `.exe`
   with `PTV_SCENE_JSON` pointing at `projects/<id>/scene.json`.

   The generated executable is named after the project's title (e.g.
   `GREEN LANTERN WILLPOWER VR.exe`) rather than the generic
   `ptv_starter_room.exe`, so each build is easy to identify on disk. The label
   shown in the Quest Link *recents* list comes from the OpenXR application
   name, which the runtime sets from the scene's `title` at launch — so the
   headset shows the project's title regardless of the exe filename.

## Quest standalone (APK)

When `BuildSpec.target_device` is `quest`, stage 16 runs `quest_build` instead
of `cmake_build`. The web panel's **Launch latest build** uses `adb install -r`
and starts the NativeActivity on a connected Quest 3.

Prerequisites: Android SDK/NDK, `adb`, and a USB-connected headset. See
[`quest-build.md`](quest-build.md) for the full toolchain checklist, APK
packaging details, and on-device validation ladder.

## CMake from the IDE

Opening the repo folder in Visual Studio Community 2026 detects the
`CMakePresets.json` automatically. Select **vs2026-x64** from the preset
dropdown and build.

## Quest 3 via Quest Link — checklist

- The Quest Link app shows *Active* in its status bar.
- *Developer Runtime Features* is enabled under *Settings → Beta*.
- Oculus is the active OpenXR runtime (same screen).
- USB 3 cable (or Air Link with a good Wi-Fi 6E+ link).
- No competing OpenXR runtime (SteamVR / Virtual Desktop) is set as active
  while you're testing through Link.

The engine requests the `XR_KHR_vulkan_enable2` extension and the stage
reference space. If either is missing, the log shows a clear error.

## Command-line launch variants

```powershell
# Explicit scene JSON:
.\ptv_starter_room.exe C:\Users\micha\Documents\Prompt-To-VR\samples\starter-room\scene.json

# Through the env var (used by the orchestrator):
$env:PTV_SCENE_JSON = "C:\Users\micha\Documents\Prompt-To-VR\projects\<id>\scene.json"
.\ptv_starter_room.exe

# Pick a different OpenXR runtime (rarely needed with Quest Link):
$env:XR_RUNTIME_JSON = "C:\Program Files\Oculus\Support\oculus-runtime\oculus_openxr_64.json"
```
