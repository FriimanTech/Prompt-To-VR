# 6. Install and configure Android Studio

Android Studio supplies the Android SDK, the NDK, and `adb` for the Quest target. The engine uses them to syntax-check generated C++ with the NDK compiler, package a standalone APK (`quest_build`), and install that APK when a headset is connected (`quest_runtime_verify`).

You do not keep Android Studio open while the pipeline runs. The repo includes a Gradle wrapper (`gradlew`, Gradle 8.9, Android Gradle Plugin 8.7.0). Do not install a separate system-wide Gradle.

This engine builds against **Android SDK Platform 34** (`compileSdk` and `targetSdk` 34, `minSdk` 32), NDK `30.0.15729638`, and **arm64-v8a**. Meta's older native guides that stop at Android 8.0 (API 26) are not enough.

Official pages:

- [Download Android Studio](https://developer.android.com/studio)
- [Install Android Studio](https://developer.android.com/studio/install)
- [Install the NDK and CMake](https://developer.android.com/studio/projects/install-ndk)

## 1. Install Android Studio

1. Download the Windows installer from [https://developer.android.com/studio](https://developer.android.com/studio).
2. Run it. The usual location is `C:\Program Files\Android\Android Studio`.
3. Finish the first-run wizard. If it offers an Android SDK, accept the default location: `C:\Users\<YourWindowsUsername>\AppData\Local\Android\Sdk`.
4. Open Android Studio once so that SDK folder exists.

## 2. Open the SDK Manager

1. From the welcome screen, click the three dots, then **SDK Manager**.
2. Or, with a project open: **File → Settings → Languages & Frameworks → Android SDK**, or **Tools → SDK Manager**.
3. Note **Android SDK Location**. It is usually `C:\Users\<YourWindowsUsername>\AppData\Local\Android\Sdk`.
4. Turn on **Show Package Details** whenever you need an exact NDK or Build-Tools row.

## 3. Install Android SDK Platform 34

On the **SDK Platforms** tab, the only platform this engine requires is **API 34**.

1. Turn on **Show Package Details**.
2. Find **Android 14.0 ("UpsideDownCake")**, API Level 34.
3. Check **Android SDK Platform 34**.
4. Click **Apply** and wait until that row says **Installed**. If it says **Partially installed**, check the row and apply again.
5. Leave **Sources for Android 34** and every emulator system image unchecked. If Sources is already installed, you can leave it. It is not part of this setup.
6. Other API levels (API 26, API 32, API 36 / Baklava, and the rest) are not required. Leave them as they are.

Done for this tab when this folder exists:

```text
C:\Users\<YourWindowsUsername>\AppData\Local\Android\Sdk\platforms\android-34
```

## 4. Install SDK tools

On the **SDK Tools** tab, with **Show Package Details** on, install:

| Package                        | Select                            |
| ------------------------------ | --------------------------------- |
| Android SDK Build-Tools        | **34.0.0 or newer**               |
| NDK (Side by side)             | exactly `30.0.15729638`           |
| CMake                          | **3.22.1 or newer**               |
| Android SDK Platform-Tools     | the latest listed version (`adb`) |
| Android SDK Command-line Tools | the latest listed version         |

Click **Apply** and wait for the downloads.

The CMake row here is **Android SDK CMake**. It lives under the SDK, for the Quest Gradle build. Desktop CMake 4.2 or newer, from [guide 4](04-build-tools-and-uv.md), is a different program and stays on PATH for the Windows engine. You need both. They do not replace each other.

`fastboot` is not used. If Platform-Tools installed `adb`, you are done with that row.

Confirm the NDK folder:

```text
C:\Users\<YourWindowsUsername>\AppData\Local\Android\Sdk\ndk\30.0.15729638
```

## 5. Set JAVA_HOME, the SDK variables, and PATH

1. Press the Windows key, search for **Environment Variables**, and open **Edit the system environment variables**.
2. Click **Environment Variables**.
3. Under **User variables**, create or edit:

| Variable | Value |
| --- | --- |
| `JAVA_HOME` | `C:\Program Files\Android\Android Studio\jbr` |
| `ANDROID_HOME` | `C:\Users\<YourWindowsUsername>\AppData\Local\Android\Sdk` |
| `ANDROID_NDK_HOME` | `C:\Users\<YourWindowsUsername>\AppData\Local\Android\Sdk\ndk\30.0.15729638` |

`JAVA_HOME` points at the `jbr` **folder**, not at `jbr\bin`. Change the Studio path if you installed it somewhere else. Use `jbr`. Older Meta docs mention a `jre` folder. Current Android Studio uses `jbr`.

4. Edit the user **Path** variable and add:

```text
%JAVA_HOME%\bin
C:\Users\<YourWindowsUsername>\AppData\Local\Android\Sdk\platform-tools
```

`%JAVA_HOME%\bin` is the same folder as `C:\Program Files\Android\Android Studio\jbr\bin` when `JAVA_HOME` is the default above.

5. Click **OK** on each dialog.
6. Close and reopen PowerShell, Command Prompt, and Cursor or VS Code so they see the new variables.
7. In a new **PowerShell** window:

```powershell
echo $env:JAVA_HOME
java -version
echo $env:ANDROID_HOME
echo $env:ANDROID_NDK_HOME
adb version
```

`JAVA_HOME` prints the `jbr` folder. `java -version` prints a Java version and does not say Java is missing. Android Studio's bundled runtime may report a newer version than 17, such as 21. That is the runtime Gradle runs on. This project's Java language level stays 17. You do not install a second JDK.

`adb version` prints an Android Debug Bridge version. `ANDROID_NDK_HOME` prints the `30.0.15729638` folder.

`ANDROID_SDK_ROOT` is optional. Leave it unset when `ANDROID_HOME` is set.

## 6. Confirm the headset with adb

Do this only after Platform-Tools is installed and [Developer Mode](05-quest-developer-mode-and-adb-drivers.md) is on. Skip it if you do not have a headset yet.

1. Connect the Quest with a data USB cable.
2. Put on the headset and choose **Always allow from this computer** if it asks.
3. In a new PowerShell:

```powershell
adb devices
```

A ready headset looks like this:

```text
List of devices attached
<serial>    device
```

`unauthorized` means the headset prompt was not accepted. `offline` usually means the cable or the driver. An empty list means the headset is not visible yet. See the driver steps in the previous guide.

### Extra information

In [Meta developer setup and Oculus ADB Drivers](05-quest-developer-mode-and-adb-drivers.md), the Oculus ADB Drivers package only lets Windows recognize the Quest on a data cable. It does not add the `adb` command.

Installing **Android SDK Platform-Tools** in this guide puts `adb` on your Path. After you reopen the terminal, `adb version` and `adb devices` can run.

## How this connects to the engine

| Piece | Used for |
| --- | --- |
| NDK `clang++` under `ANDROID_NDK_HOME` | Quest syntax check during code generation |
| `JAVA_HOME`, `ANDROID_HOME`, the Gradle wrapper, NDK `30.0.15729638`, SDK 34 | `quest_build` (the APK) |
| `adb` | Install, launch, and `quest_runtime_verify` |

More on the APK itself: [Quest build](../quest-build.md). After the tools and [API keys](api-keys.md) are in place, continue with [Local setup](../local-setup.md).

## Done when

- `platforms\android-34` exists.
- `ndk\30.0.15729638` exists.
- Build-Tools 34.0.0 or newer, Android SDK CMake 3.22.1 or newer, and Platform-Tools are installed.
- `JAVA_HOME` is the `jbr` folder, and `java -version` works in a new terminal.
- `adb version` works in that same terminal.
- With a headset connected, `adb devices` shows `device`.

## If this fails

| What you see | What to do |
| --- | --- |
| `JAVA_HOME is set to an invalid directory` | Point `JAVA_HOME` at the `jbr` folder, not at `jbr\bin` and not at `jre`. |
| NDK not found, or the wrong NDK | Install `30.0.15729638` and set `ANDROID_NDK_HOME` to that folder. |
| `adb` is not recognized | Add `platform-tools` to Path and open a new terminal. |
| `adb devices` is empty or says `unauthorized` | Developer Mode, the USB prompt, a data cable, and the Oculus driver from the previous guide. |
| API 34 says partially installed | Check **Android SDK Platform 34** and apply again. |

Previous: [Meta developer setup and Oculus ADB Drivers](05-quest-developer-mode-and-adb-drivers.md). Back to the [setup index](README.md).
