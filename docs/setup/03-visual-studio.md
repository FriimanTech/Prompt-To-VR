# 3. Install Visual Studio Community 2026

Visual Studio compiles the C++ engine on Windows. Quest packaging also uses this compiler: code generation checks the C++ with MSVC before the Android NDK syntax check. Install **Visual Studio Community 2026** with the **Desktop development with C++** workload and the **MSVC v145** toolset.

The CMake generator string this repo uses is `Visual Studio 18 2026`.

Official pages:

- [Visual Studio downloads](https://visualstudio.microsoft.com/downloads/)
- [Install Visual Studio](https://learn.microsoft.com/en-us/visualstudio/install/install-visual-studio)

## Install

1. Open [https://visualstudio.microsoft.com/downloads/](https://visualstudio.microsoft.com/downloads/) and download **Community 2026**.
2. Run the installer. If Visual Studio is already installed, open **Visual Studio Installer** and choose **Modify**.
3. On the **Workloads** tab, check **Desktop development with C++**.
4. On the **Individual components** tab, confirm these are selected (the C++ workload usually selects them):
   - **MSVC v145** (the VS 2026 C++ build tools)
   - **C++ CMake tools for Windows**
   - **Windows 10 SDK** or **Windows 11 SDK**
5. Click **Install** or **Modify** and wait until it finishes. This download is large.
6. You can close Visual Studio when the installer is done. The engine build is started from PowerShell with CMake, not by clicking Build inside a sample project.

## Check

1. Open the **Visual Studio Installer**.
2. Next to Community 2026, confirm the **Desktop development with C++** workload is installed.
3. The later CMake configure in [guide 4](04-build-tools-and-uv.md) is the real compile check. A successful `cmake --preset vs2026-x64` means this workload is visible to the build.

## Done when

Visual Studio Installer shows Community 2026 with **Desktop development with C++** installed, including MSVC v145, C++ CMake tools for Windows, and a Windows SDK.

## If this fails

- **CMake later says it cannot find the Visual Studio 18 2026 generator.** The 2026 product or the C++ workload is missing. Modify the installer and add **Desktop development with C++**. Desktop CMake must also be **4.2 or newer**, which is installed in the next guide. The CMake copy inside Android Studio does not satisfy that requirement.

Previous: [Node.js](02-nodejs.md). Next: [vcpkg, CMake, Vulkan SDK, and uv](04-build-tools-and-uv.md). Back to the [setup index](README.md).
