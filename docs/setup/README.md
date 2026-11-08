# Quest setup

Install these tools in order on **Windows**. The first guide is [Python](01-python.md). Each page has the download, the install steps, and a short check that you are done.

A Quest headset is highly advised for playing and testing. You can install the build tools and produce an APK (VR Game) without one. Come back to the headset steps in guides 5 and 6 when you have a device.

**Fastest way to play a game this engine already built:** finish [step 5](05-quest-developer-mode-and-adb-drivers.md), then follow [No Android Studio: Fastest Way](platform-tools-no-android-studio.md). You do not need Android Studio, Python, Node.js, or API keys for that install. If you plan to build games with this engine, use [step 6](06-android-studio.md) instead.

**PC VR through Quest Link has not been tested recently and may have major issues.** The recommended path is standalone Quest, especially Quest 3. PC VR requirements are a separate page, not a step in this list: [PC VR through Quest Link: requirements](pc-vr-requirements.md).

## Order

1. [Python](01-python.md) — Python Install Manager, then the editor extension.
2. [Node.js](02-nodejs.md) — Node.js 20 or newer, for the web control panel.
3. [Visual Studio Community 2026](03-visual-studio.md) — Desktop development with C++, MSVC v145.
4. [vcpkg, desktop CMake, Vulkan SDK, and uv](04-build-tools-and-uv.md) — C++ libraries, the Windows build, shaders, and the Python 3.13 project environment.
5. [Meta developer setup and Oculus ADB Drivers](05-quest-developer-mode-and-adb-drivers.md) — developer team, Developer Mode, and the Windows USB driver.
6. [Android Studio](06-android-studio.md) — SDK Platform 34, NDK `30.0.15729638`, `adb`, and `JAVA_HOME`.

API keys are not a step in that list. Set them up in [API keys](api-keys.md) when you are ready to generate a game. You can build the engine without keys.

## After the tools are installed

Run the commands in [Local setup](../local-setup.md) next, in order: copy `.env` and fill in the API keys, create the Python environment, install the website packages, then configure and build the engine. Those commands are the setup that happens after the downloads. Read the wait-time notices on that page. The first CMake configure and the first engine compile each take a long time.

Then:

- [Build and run on Quest](../build-and-run.md) — start the website, generate a Quest game, or install the sample APK (VR Game).
- [Build and run on PC VR](../build-and-run-pcvr.md) — the Windows `.exe` through Quest Link. This path has not been tested recently.
- [Quest APK (VR Game) packaging](../quest-build.md) — what the Quest build stage does inside the pipeline.

## Done when

- [ ] `python --version` prints a version in a new Command Prompt.
- [ ] `node -v` prints v20 or newer.
- [ ] Visual Studio 2026 has the Desktop development with C++ workload.
- [ ] `uv --version` works, and `vrvenv` exists at the repo root.
- [ ] `cmake --version` is 4.2 or newer (desktop CMake).
- [ ] Vulkan SDK 1.4.341.x is installed.
- [ ] If you have a headset: Developer Mode is on, the Oculus ADB driver is installed, and after step 6 `adb devices` shows `device`.
- [ ] `.env` has the keys from [API keys](api-keys.md) for the kind of game you want.

Return to the [README](../../README.md).
