# PC VR through Quest Link: requirements

**PC VR through Quest Link has not been tested recently and may have major issues. The recommended path is standalone Quest, especially Quest 3, because recent development and testing have focused there.**

Use this page only if you are building the Windows `.exe` and running it through Quest Link. The ordered install walkthrough for the recommended path is the [Quest setup guide](README.md).

## Shared with the Quest setup

These are the same tools as guides 1–4. Install them from those pages. A PC VR build does not skip them, because the `.exe` is the Windows engine.

| Tool | Requirement | Guide |
| --- | --- | --- |
| Python | 3.11 or newer installed, then **Python 3.13 via `uv`** in `vrvenv` | [Python](01-python.md), [uv in guide 4](04-build-tools-and-uv.md) |
| Node.js | **20+** | [Node.js](02-nodejs.md) |
| Visual Studio Community 2026 | **Desktop development with C++ / MSVC v145** | [Visual Studio](03-visual-studio.md) |
| vcpkg | `VCPKG_ROOT` set, `vcpkg.exe` bootstrapped | [Guide 4](04-build-tools-and-uv.md) |
| Desktop CMake | **4.2 or newer** | [Guide 4](04-build-tools-and-uv.md) |
| Vulkan SDK | **1.4.341.x** | [Guide 4](04-build-tools-and-uv.md) |
| uv | On PATH, then `vrvenv` created | [Guide 4](04-build-tools-and-uv.md) |

Generating a game still needs the API keys in [API keys](api-keys.md). The starter-room sample does not.

## Only for PC VR through Quest Link

These are not in the Quest download table. Standalone Quest packaging does not use them.

| Tool | What to do |
| --- | --- |
| Meta Quest Link desktop app | Install it from Meta and sign in. Connect the headset with a Link cable, or turn on Air Link. |
| OpenXR runtime | In the Quest Link app, open **Settings → Beta** and choose **Set Oculus as active OpenXR runtime**. |
| Headset in Link mode | Put on the Quest and confirm Link is connected before you launch the `.exe`. |

Official Link requirements: [Meta Quest Link requirements](https://www.meta.com/help/quest/articles/headsets-and-accessories/oculus-link/requirements-quest-link/).

## Not required for a PC VR-only workflow

Do not install these if you are only building the Windows executable:

- Android Studio
- Android SDK Platform 34, the NDK, Android SDK CMake, and Platform-Tools
- Oculus ADB Drivers
- `JAVA_HOME`, `ANDROID_HOME`, and `ANDROID_NDK_HOME`

Those exist so the Quest path can package and sideload an APK (VR Game). Quest Link does not use `adb` for the `.exe`.

## Build and launch

From PowerShell, after guide 4:

```powershell
cd engine
cmake --preset vs2026-x64
cmake --build --preset vs2026-x64-release
```

With Quest Link connected, the starter-room executable is:

```text
engine\build\vs2026-x64\samples\starter-room\Release\ptv_starter_room.exe
```

The website's **Launch starter sample** button starts that PC VR executable. Choosing **Quest (standalone APK)** in New project builds an APK (VR Game) instead, which is the path this repo has been updating. The full PC VR launch steps are in [Build and run on PC VR](../build-and-run-pcvr.md).

Back to the [setup index](README.md) or the [README](../../README.md).
