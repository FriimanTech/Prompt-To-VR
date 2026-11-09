

# No Android Studio: Fastest Way (Windows)

Use this page when you only want to play the sample VR game, **Glock Laser Deck**, on a Quest from a Windows PC. You do not need Android Studio, Python, or Node.js.

On a Mac, use [No Android Studio: Fastest Way (Mac)](platform-tools-no-android-studio-mac.md).

## 1. Prerequisites

**Important**: First if you havent already you must do all the steps in[Quest setup step 5](05-quest-developer-mode-and-adb-drivers.md):  create a developer account, enable Developer Mode, and the Oculus ADB Drivers. Then come back here.

If you later want to build games with this engine or just test the engine out later, leave this page and use [Android Studio (step 6)](06-android-studio.md) instead. That is the path for running and building on top of the VR Engine.

## 2. Download Platform-Tools

1. Open the [Android SDK Platform-Tools download page](https://developer.android.com/tools/releases/platform-tools#downloads).
2. Under **Downloads**, click **Download SDK Platform-Tools for Windows**.
3. Read the terms in the pop-up, scroll to the bottom, and check **I have read and agree with the above terms and conditions** if you agree.
4. Click **Download Android SDK Platform-Tools for Windows**. The file is named `platform-tools-latest-windows.zip` and usually appears in your **Downloads** folder.

## 3. Extract the ZIP file

1. Open **File Explorer**, go to **Downloads**, right-click the ZIP file, and select **Extract All**. Then click **Extract**.
2. Open the extracted folder and find the `platform-tools` folder. Open it and look for `adb.exe`. It may appear as `adb` if Windows hides file extensions.
3. Move the entire `platform-tools` folder somewhere you will keep it, such as **Documents**.
4. Open that folder in its new location. Copy the path from the address bar at the top of File Explorer. The path must be the folder that contains `adb.exe`. An example is:

```text
C:\Users\YourName\Documents\platform-tools
```

## 4. Make the `adb` command available from any folder

1. Open the **Windows search** bar, search for **Edit environment variables for your account**, and open that result.
2. Under **User variables**, select **Path** and click **Edit**.
3. Click **New**, then press **Ctrl+V** to paste the `platform-tools` folder location you copied.
4. Click **OK** in both windows to save.
5. If Command Prompt or PowerShell is already open, close it before continuing.

## 5. Optional checks

### A. Check that ADB works

Command Prompt is a built-in Windows app where you type commands and see the results. You can do this check before connecting the headset.

1. Click the **Start** button (the Windows logo on the taskbar).
2. Type **Command Prompt**, then click the **Command Prompt** app.
3. A window opens with a blinking cursor after a line that may look like `C:\Users\YourName>`. Type this and press **Enter**:

```bat
adb version
```

4. If you see **Android Debug Bridge version** followed by a number, ADB is ready.
5. If Windows says `adb` is not recognized, the **Path** entry is not the folder that directly contains `adb.exe`. Fix that entry, close Command Prompt, open it again, and retry.

### B. Check that ADB sees your Meta Quest

1. With Developer Mode enabled, connect the headset to the PC with a USB-C cable. Put on the headset and select **Allow** if it asks to allow USB debugging. Those prompts are steps 7–11 of [Enable Developer Mode](https://developers.meta.com/vr/documentation/native/android/mobile-device-setup/#enable-developer-mode).
2. Take the headset off and leave it connected to the computer.
3. In the same Command Prompt window, type this and press **Enter**:

```bat
adb devices
```

4. If the headset serial number appears followed by `device`, it is ready for ADB commands.
5. If you see `unauthorized`, put the headset back on and select **Allow** on the USB debugging prompt.
6. If the list is empty, check that Developer Mode is enabled, try another USB-C cable or USB port, and use a cable that carries data, not only power.
7. If the headset still does not appear, confirm the [Oculus ADB Drivers for Windows](https://developers.meta.com/vr/downloads/package/oculus-adb-drivers/) are installed. That install is part of [step 5](05-quest-developer-mode-and-adb-drivers.md).

## 6. Connect the headset and launch the sample game

If the headset is not already connected with a USB-C data cable, connect it now.

1. Open [https://friiman-tech.vercel.app/](https://friiman-tech.vercel.app/).
2. Click **I've completed all setup steps**.
3. Click **Install & Launch VR Game**.

That installs the sample VR game on the headset and starts it.

## 7. How to play the game again

The game does not sit in the normal app library.

1. Put on the Quest.
2. Press the **Quest** button on the right controller to open the menu.
3. Click **Library**.
4. Click **Menu** on the left side of that panel.
5. Click **Unknown Sources**.
6. Click **Glock Laser Deck**.

Back to [Build and run on Quest](../build-and-run.md). The Android Studio path is [step 6](06-android-studio.md).
