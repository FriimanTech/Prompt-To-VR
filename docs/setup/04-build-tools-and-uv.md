# 4. vcpkg, desktop CMake, Vulkan SDK, and uv

These four tools finish the Windows side of the engine. Install them in the order below. **`uv` is installed before any `uv` command.**

The Python from [guide 1](01-python.md) proves Python works. This page creates a separate environment, `vrvenv`, on **Python 3.13**. The orchestrator runs inside that environment.

## 4.1 vcpkg

vcpkg downloads the C++ libraries (Vulkan, OpenXR, Jolt, and others) in manifest mode. The repo does not vendor those libraries.

Official page: [vcpkg](https://github.com/microsoft/vcpkg)

You need Git. If `git` is missing, install it from [https://git-scm.com/download/win](https://git-scm.com/download/win) and open a new PowerShell.

In **PowerShell**:

```powershell
git clone https://github.com/microsoft/vcpkg C:\src\vcpkg
C:\src\vcpkg\bootstrap-vcpkg.bat
```

Set a user environment variable:

| Variable | Value |
| --- | --- |
| `VCPKG_ROOT` | `C:\src\vcpkg` |

Use another folder if you prefer, and use that path as `VCPKG_ROOT`. Add the same value to the repo-root `.env` later (`VCPKG_ROOT=C:\src\vcpkg`). Creating `.env` is covered in [Local setup](../local-setup.md).

Close and reopen PowerShell, then run:

```powershell
echo $env:VCPKG_ROOT
Test-Path "$env:VCPKG_ROOT\vcpkg.exe"
```

Both should succeed. `Test-Path` prints `True`.

The first engine configure builds those libraries and can take 10–30 minutes. Later configures reuse the cache.

## 4.2 Desktop CMake 4.2 or newer

This CMake configures the Windows engine (`Visual Studio 18 2026` needs CMake 4.2 or newer). It is **not** the CMake package inside Android Studio. The Quest guide installs that second CMake later, under the Android SDK, for the APK build. You want both.

Official page: [CMake downloads](https://cmake.org/download/)

1. Download the Windows x64 installer for **CMake 4.2 or any newer 4.x**.
2. During setup, choose **Add CMake to the system PATH for the current user**.
3. Close and reopen PowerShell.

```powershell
cmake --version
```

The first line should show `cmake version 4.2` or newer.

If Visual Studio's own CMake is older than 4.2, do not rely on it. The installer on cmake.org is the one this step is checking.

## 4.3 Vulkan SDK 1.4.341.x

The SDK supplies Vulkan headers and `glslc`, which compiles shaders. Install the **1.4.341** line, not a random newer SDK, so the setup matches this repo.

Official page: [Vulkan SDK for Windows](https://vulkan.lunarg.com/sdk/home#windows)

1. Open the LunarG page and download **1.4.341.x** for Windows x64.
2. Run the installer. Leave the option that sets the `VULKAN_SDK` environment variable enabled.
3. Close and reopen PowerShell.

```powershell
echo $env:VULKAN_SDK
glslc --version
```

`VULKAN_SDK` should point at a `1.4.341.x` directory. `glslc --version` should print a version. If `glslc` is not found, open a new terminal. The installer adds it to PATH.

## 4.4 uv, then the Python 3.13 environment

`uv` installs the Python 3.13 interpreter the engine uses and creates `vrvenv`. Install `uv` before the commands below.

Official page: [uv](https://docs.astral.sh/uv/)

In a new **PowerShell**:

```powershell
winget install --id=astral-sh.uv -e
```

Close and reopen PowerShell so `uv` is on PATH.

```powershell
uv --version
```

You should see a version line. If PowerShell says `uv` is not recognized, the window was opened before the install finished. Open another window.

From the **repository root** (the folder that contains `apps`, `engine`, and `packages`):

```powershell
uv python install 3.13
uv venv .\vrvenv --python 3.13
.\vrvenv\Scripts\Activate.ps1
uv pip install -e .\packages\schemas -e .\packages\provider-clients -e ".\apps\orchestrator[dev]"
python --version
```

`python --version` **after activation** should print **Python 3.13**. That is the interpreter the engine uses. The version from guide 1 can be different; that is expected.

In Cursor or VS Code, choose the interpreter `vrvenv\Scripts\python.exe` (Command Palette → **Python: Select Interpreter**). Pylance and the Python extension from [guide 1](01-python.md) then follow this environment.

## Done when

- `vcpkg.exe` exists under `VCPKG_ROOT`.
- `cmake --version` is 4.2 or newer.
- `VULKAN_SDK` points at 1.4.341.x and `glslc --version` runs.
- `uv --version` runs, and an activated `vrvenv` reports Python 3.13.

## If this fails

- **`uv` is not recognized.** Open a new PowerShell after `winget` finishes.
- **`cmake` is older than 4.2.** You are seeing a different CMake on PATH, often an older copy. Install the 4.2+ desktop installer and reopen the terminal.
- **Activation script is blocked.** In PowerShell, `Set-ExecutionPolicy -Scope CurrentUser RemoteSigned`, then run the activate script again.

Previous: [Visual Studio](03-visual-studio.md). Next: [Meta developer setup and Oculus ADB Drivers](05-quest-developer-mode-and-adb-drivers.md). Back to the [setup index](README.md).
