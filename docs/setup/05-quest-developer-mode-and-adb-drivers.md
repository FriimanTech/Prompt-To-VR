# 5. Meta developer setup and Oculus ADB Drivers

This page prepares a Quest so Windows can install a build (VR Games) on it. You need a headset for these steps. If you do not have one yet, skip this page and return when the headset arrives. Building an APK does not require the headset. Playing it and running the on-headset checks does.

**Quest 3** is the headset this engine has been tested on recently. **Quest 2** is also advised, but it has not had that same testing. Meta's current native device-setup page still lists the original **Meta Quest** (Quest 1), Quest 2, Quest Pro, Quest 3, and Quest 3S as Android devices that page covers. This engine does not treat Quest 1 as a tested target. Use Quest 3, or Quest 2 if that is the headset you have.

Official pages:

- [Set up your device for development](https://developers.meta.com/horizon/documentation/native/android/mobile-device-setup/)
- [Create or join a developer organization](https://developers.meta.com/vr/manage/organizations/create/)
- [Oculus ADB Drivers](https://developers.meta.com/horizon/downloads/package/oculus-adb-drivers/)

## 1. Headset and cable

1. Have the Quest charged and paired with the **Meta Horizon** app on your phone ([iOS](https://apps.apple.com/app/meta-horizon/id1366478176), [Android](https://play.google.com/store/apps/details?id=com.oculus.twilight)).
2. Use a **USB-C data cable**. A charge-only cable will not show the headset to the PC.

## 2. Developer team on the dashboard

Developer Mode stays locked until your Meta account is on a verified developer team. Do this in a browser, on the **Meta Horizon Developer Dashboard**.

1. Open [Create or join an organization](https://developers.meta.com/vr/manage/organizations/create/) and sign in with your Meta account. You must be 18 or older.
2. **Create** an organization if you do not already belong to one, or **join** an existing team when an admin invites you. The dashboard page uses "organization" and "team" for this membership. Follow the labels on that page.
3. Open the dashboard **Verification** area and complete account verification. Meta describes this as a one-time identity check. The exact documents it asks for are on that screen.
4. Confirm the dashboard shows you as a member of a developer team before you continue. If Developer Mode is missing in the phone app, this step is not finished yet.

## 3. Turn on Developer Mode

This toggle is in the **phone app**, not in the headset settings menu you use for brightness.

1. On your phone, open the **Meta Horizon** app and sign in with the same Meta account.
2. Tap the **headset** icon in the toolbar.
3. Tap your paired headset.
4. Tap **Headset Settings**.
5. Tap **Developer Mode**.
6. Turn **Developer Mode** on.

If the toggle is missing, go back to the dashboard and finish team membership and verification.

## 4. Install the Oculus ADB Drivers

These are Windows drivers. Android Studio does not install them for you.

1. Open the [Oculus ADB Drivers](https://developers.meta.com/horizon/downloads/package/oculus-adb-drivers/) page and download the package.
2. Unzip the download.
3. Right-click `android_winusb.inf` and choose **Install**.
4. Approve the Windows prompt and wait until it reports that the driver installed.

If Windows says the best driver is already installed, that is fine. This package lets Windows recognize the Quest on a data cable. It does not install the `adb` command. That command is added when you install Platform-Tools in [Android Studio](06-android-studio.md).

## Done when

- The dashboard shows your account on a developer team, and verification is complete.
- The phone app shows **Developer Mode** on for your headset.
- `android_winusb.inf` has been installed.

## If this fails

- **Developer Mode is not in the phone app.** Finish the organization and verification on the dashboard, then force-close the phone app and open it again.
- **Windows will not install `android_winusb.inf`.** Unzip the download again, right-click that file, and choose **Install**. Approve the prompt.

Previous: [vcpkg, CMake, Vulkan SDK, and uv](04-build-tools-and-uv.md). Next: [Android Studio](06-android-studio.md). Back to the [setup index](README.md).
