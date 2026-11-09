# No Android Studio: Fastest Way (Mac)

Use this page when you only want to play the sample VR game, **Glock Laser Deck**, on a Quest from a Mac. You do not need Android Studio, Python, or Node.js.

If you are on Windows, go here instead [No Android Studio: Fastest Way (Windows)](platform-tools-no-android-studio.md).

## 1. Prerequisites

**Important.** If you have not already, complete [Quest setup (Mac version)](05-quest-developer-mode-mac.md). Set up and verify your Meta developer account, join or create a developer team, and enable Developer Mode on your Quest. Then come back here.

## 2. Download Platform-Tools

1. Open the [Android SDK Platform-Tools download page](https://developer.android.com/tools/releases/platform-tools#downloads).
2. Under **Downloads**, click **Download SDK Platform-Tools for Mac**.
3. Read the terms in the pop-up, scroll to the bottom, and check the box if you agree.
4. Click **Download Android SDK Platform-Tools for Mac**. The file is named `platform-tools-latest-darwin.zip` and usually appears in your **Downloads** folder.

## 3. Extract the ZIP file

1. Open **Finder** and go to **Downloads**.
2. Double-click `platform-tools-latest-darwin.zip`. A folder named `platform-tools` appears. If your browser already extracted the ZIP, open the folder it created.
3. Open the `platform-tools` folder and confirm it contains a file named `adb`.
4. Go back to **Downloads**, select the entire `platform-tools` folder, and press **Command+C**.
5. In Finder’s menu bar, click **Go → Home**, then press **Option+Command+V** to move the folder there.
6. Open the folder in its new location and confirm you still see `adb`. The folder is now at `~/platform-tools`. `~` means your Mac account’s Home folder.

## 4. Make the `adb` command available from any folder

Terminal is the built-in Mac app where you type commands and see the results. These steps use the Mac’s default shell, zsh.

1. Press **Command+Space** to open Spotlight. Type **Terminal**, then press **Return**.
2. Copy and paste this command into Terminal, then press **Return**. It adds the `platform-tools` folder to `PATH`, the list of folders Terminal searches for commands.

```zsh
printf '\n%s\n' 'export PATH="$HOME/platform-tools:$PATH"' >> ~/.zprofile
```

3. Run this command to apply the change in the Terminal window you already have open. Future Terminal windows load the change automatically.

```zsh
source ~/.zprofile
```

## 5. Optional checks

### A. Check that ADB works

In Terminal, type this command and press **Return**. You can do this before connecting your headset.

```zsh
adb version
```

If you see **Android Debug Bridge version** followed by a number, ADB is ready.

If you see `command not found: adb`, confirm that `adb` is directly inside the `platform-tools` folder in **Finder → Go → Home**. Then reopen Terminal and try again.

### B. Check that ADB sees your Meta Quest

1. With Developer Mode enabled, unlock your Mac and connect your headset with a USB-C cable that supports data. If your Mac asks whether to allow the USB accessory, click **Allow**.
2. Put on the headset. If it asks **Allow USB debugging?**, select **Allow**. On your own Mac, you can also select **Always allow from this computer**.
3. Leave the headset connected. In Terminal, type this command and press **Return**:

```zsh
adb devices
```

4. If your headset’s serial number appears followed by `device`, it is ready for ADB commands.
5. If you see `unauthorized`, put the headset back on, approve the USB debugging prompt, and run `adb devices` again.
6. If the list is empty, confirm Developer Mode is enabled and try another USB-C data cable or USB port. If the USB debugging prompt still does not appear, follow Meta’s first-time connection steps. Meta lists **Quick Controls → Settings → Developer → MTP Notification** before approving the separate USB debugging prompt. See [Meta’s Developer Mode instructions](https://developers.meta.com/vr/documentation/native/android/mobile-device-setup/#enable-developer-mode).

## 6. Connect the headset and launch the sample game

If the headset is not already connected, plug it into your unlocked Mac with a USB-C data cable. Approve the USB accessory prompt on the Mac and the USB debugging prompt in the headset if they appear.

1. On your Mac, open [Friiman Tech](https://friiman-tech.vercel.app/) in Google Chrome.
2. Click **I've completed all setup steps**.
3. Click **Install & Launch VR Game**. If Chrome opens a USB device picker, select your Quest and click **Connect**. If the headset asks again about USB debugging, select **Allow**.
4. Leave the headset connected while the installer installs and launches the sample game.

If Chrome reports that the headset is busy after you ran `adb devices`, run this in Terminal, then retry the installation:

```zsh
adb kill-server
```

## 7. How to play the game again

1. Put on the Quest.
2. Press the **Quest** button on the right controller to open the menu.
3. Select **Library**.
4. Select **Unknown Sources**. If it is inside the left-side **Menu** on your headset, open **Menu** first.
5. Select **Glock Laser Deck**.

Back to [Build and run on Quest](../build-and-run.md). The Windows Platform-Tools path is [No Android Studio: Fastest Way (Windows)](platform-tools-no-android-studio.md).
