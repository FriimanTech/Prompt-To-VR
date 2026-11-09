# Quest setup (Mac)

This page prepares a Quest so a Mac can install a VR game on it. You need a headset for these steps. macOS does not use the Oculus ADB Drivers. Those drivers are Windows only.

**Quest 3** is the headset this engine has been tested on recently. **Quest 2** is also advised, but it has not had that same testing. Meta's current device-setup page still lists the original **Meta Quest** (Quest 1), Quest 2, Quest Pro, Quest 3, and Quest 3S. This engine does not treat Quest 1 as a tested target. Use Quest 3, or Quest 2 if that is the headset you have.

Official pages:

- [Set up your device for development](https://developers.meta.com/horizon/documentation/native/android/mobile-device-setup/)
- [Create or join a developer organization](https://developers.meta.com/vr/manage/organizations/create/)

## 1. Headset and cable

1. Have the Quest charged and paired with the **Meta Horizon** app on your phone ([iOS](https://apps.apple.com/app/meta-horizon/id1366478176), [Android](https://play.google.com/store/apps/details?id=com.oculus.twilight)).
2. Use a **USB-C data cable**. A charge-only cable will not show the headset to the Mac.

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

## Done when

- The dashboard shows your account on a developer team, and verification is complete.
- The phone app shows **Developer Mode** on for your headset.

## If this fails

- **Developer Mode is not in the phone app.** Finish the organization and verification on the dashboard, then force-close the phone app and open it again.

Next: [No Android Studio: Fastest Way (Mac)](platform-tools-no-android-studio-mac.md). The Windows version of this setup is [step 5](05-quest-developer-mode-and-adb-drivers.md).
