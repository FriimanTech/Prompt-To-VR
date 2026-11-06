# Build and run on PC VR

**PC VR through Quest Link has not been tested recently and may have major issues. The recommended path is standalone Quest, especially Quest 3.** Quest steps are in [Build and run on Quest](build-and-run.md).

This page is only the Windows `.exe` launched through Quest Link. An **APK (VR Game)** is the Quest install file, not this `.exe`. Install the shared desktop tools first ([local setup](local-setup.md)). Android Studio, the NDK, and the Oculus ADB driver are not required for this path. Quest Link requirements are in [PC VR requirements](setup/pc-vr-requirements.md).

## Starter room (no API keys)

The hand-authored starter room checks that Vulkan and OpenXR start. It is not a Quest APK (VR Game).

```powershell
cd engine
cmake --preset vs2026-x64
cmake --build --preset vs2026-x64-release
```

> **Notice:** If you already built the engine during local setup, skip these two commands. The first configure is about **10–30 minutes**. The first compile is about **10–30 minutes** as well.

Then:

1. Start the Meta Quest Link desktop app.
2. Open **Settings → Beta** and choose **Set Oculus as active OpenXR runtime**. Turn on **Developer Runtime Features** on that same screen.
3. Put the headset on and enter Quest Link or Air Link.
4. Launch:

```powershell
.\build\vs2026-x64\samples\starter-room\Release\ptv_starter_room.exe
```

On success, a desktop window titled `ptv_starter_room [mirror]` opens, and the headset shows the OpenXR stage.

You can also press **Launch starter sample** on the website home page after the orchestrator is running. That button starts this same executable.

## A generated PC VR game

1. Start the orchestrator from the repository root:

```powershell
.\vrvenv\Scripts\Activate.ps1
uvicorn ptv_orchestrator.main:app --host 127.0.0.1 --port 8787 --reload
```

2. In a second window:

```powershell
cd apps\web
npm run dev
```

3. Open <http://127.0.0.1:5173>, click **New project**, paste a prompt, choose **PC VR (Quest Link)**, and click **Create & run pipeline**.

4. When the `.exe` is built, press **Launch build**. The site starts that project's `.exe` and passes that project's `scene.json`. The executable is named from the project title, not `ptv_starter_room.exe`. Quest Link's recents list uses the scene title. Everything for that game is in the `projects` folder.

## Quest Link checklist

- The Quest Link app shows **Active**.
- **Developer Runtime Features** is on under **Settings → Beta**.
- Oculus is the active OpenXR runtime on that same screen.
- The headset is on a USB 3 cable, or Air Link on a strong Wi-Fi link.
- SteamVR or another OpenXR runtime is not the active runtime while you test through Link.

## Launch from PowerShell

From the folder that contains the executable:

```powershell
# Starter room scene, passed as an argument:
.\ptv_starter_room.exe C:\path\to\Prompt-To-VR\samples\starter-room\scene.json

# A generated project's scene:
$env:PTV_SCENE_JSON = "C:\path\to\Prompt-To-VR\projects\<id>\scene.json"
.\ptv_starter_room.exe

# Point at the Oculus OpenXR runtime if Link did not register it:
$env:XR_RUNTIME_JSON = "C:\Program Files\Oculus\Support\oculus-runtime\oculus_openxr_64.json"
```

Opening the `engine` folder in Visual Studio Community 2026 picks up `CMakePresets.json`. Choose the **vs2026-x64** preset and build.
