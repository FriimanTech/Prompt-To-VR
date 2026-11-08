# Build and run on Quest

This page is the standalone Quest path. A Quest 3 is the headset recent testing has used. Quest 2 can be used, but it has not had that same testing.

PC VR through Quest Link is a different path, and it has not been tested recently. Those steps are in [Build and run on PC VR](build-and-run-pcvr.md).

Finish [local setup](local-setup.md) before you generate a new game. The sample APK (VR Game) below does not need that.

## Optional but recommended: try the sample Quest APK

An **APK (VR Game)** is the installable Quest game file. This sample is the fastest way to put a game this engine has already built onto a headset. You do not need Visual Studio, an API key, or a new pipeline run.

You do need Quest setup [step 5](setup/05-quest-developer-mode-and-adb-drivers.md) (developer account, Developer Mode, and the Oculus ADB driver). Then pick one way to get `adb`:

- **[No Android Studio: Fastest Way](setup/platform-tools-no-android-studio.md).** If you do not want to install Android Studio and only want to test the sample VR game this engine already created, use this path.
- **[Android Studio: Recommended if you plan to run or build on top of the VR Engine later](setup/06-android-studio.md).** This is the normal [step 6](setup/06-android-studio.md). It installs Platform-Tools with the rest of the Android SDK.

The sample APK (VR Game) is in the repo at `samples/quest-sample/GlockLaserDeck.apk`. It is **Glock Laser Deck**, used as the sample until a dedicated starter APK is added. You do not need anything from `projects/`. The header button **Quest: Launch Sample Game** runs these same install and start steps for you. The headset still has to be connected, with Developer Mode on.

1. Connect the Quest to the Windows PC with a **data** USB cable.
2. Put on the headset and choose **Always allow from this computer** if it asks.
3. In PowerShell, from the repository root, confirm the headset:

```powershell
adb devices
```

The headset line must end in `device`.

4. **Install the APK (VR Game).** This is the same install command the engine uses (`adb install -r`):

```powershell
adb install -r .\samples\quest-sample\GlockLaserDeck.apk
```

Wait until PowerShell prints `Success`.

### 5. Start the APK (VR Game)

Pick either way.

#### A. In the headset

1. In the Quest, click **Library** to open your game library.
2. Click **Menu**.
3. In the menu panel, click **Unknown Sources**.
4. In the Unknown Sources panel, click **Glock Laser Deck**.

#### B. Via the terminal

With the headset still connected to the computer, run:

```powershell
adb shell am start -n com.prompttovr.p20261030_165846_glock_laser_deck/android.app.NativeActivity
```

## Generate your own Quest game

1. Start the orchestrator from the repository root:

```powershell
.\vrvenv\Scripts\Activate.ps1
uvicorn ptv_orchestrator.main:app --host 127.0.0.1 --port 8787 --reload
```

2. In a second PowerShell window, start the website:

```powershell
cd apps\web
npm run dev
```

3. Open <http://127.0.0.1:5173>. Click **New project**. **Quest (standalone APK)** and **Full Player Avatar** are already selected. Paste a prompt and click **Create & run pipeline**. On the project page, the **Pipeline** tab shows each stage.
4. When the APK (VR Game) is built, click **Install & launch on Quest**. The headset must be connected to your computer. That button installs the new APK (VR Game) with `adb install -r` and starts it on the Quest. You can also download it from the same page with the **Download APK** button. That button works only after the APK exists. Everything for that game is in the `projects` folder.

The headset must show up in `adb devices` before launch. Developer Mode and the data cable are the same requirements as the sample install above.

> **Notice:** The first Quest APK (VR Game) build compiles the engine with the Android NDK and may download Gradle. Plan on a long wait, often **20–40 minutes**, sometimes longer on a slower PC. Later builds of the same project are shorter.

How packaging works is in [Quest build](quest-build.md). Stage-by-stage notes are in [All stages](All_Stages_Docs/README.md).
