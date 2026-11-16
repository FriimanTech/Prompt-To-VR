# No Android Studio: Fastest Way (Mac)

Use this page when you only want to play the sample VR game, **The Shooting Range Game**, on a Quest from a Mac. You do not need Android Studio, Python, Node.js, or Android Platform-Tools.

This page prepares your Quest so the website can install the sample game. You need a headset for these steps. If you do not have one yet, skip this page and return when the headset arrives.

On Windows, use [No Android Studio: Fastest Way (Windows)](platform-tools-no-android-studio.md).

You do not type any install commands. After the Meta setup below, the website runs the install when the headset is connected to the Mac. macOS does not use the Oculus ADB Drivers. Those drivers are Windows only, so this page does not install them.

Steps 1–3 follow these official Meta pages:

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

## 4. Connect the headset and launch the sample game

If the headset is not already connected, plug it into your unlocked Mac with a USB-C data cable.

1. If the Mac asks whether to allow the USB accessory, click **Allow**.
2. Put on the headset. If it asks **Allow USB debugging?**, select **Allow**. On your own Mac, you can also select **Always allow from this computer**.
3. On the Mac, open [https://friiman-tech.vercel.app/products/prompt-to-vr-engine](https://friiman-tech.vercel.app/products/prompt-to-vr-engine) in Google Chrome.
4. Click **Play Sample VR Game**.
5. Click **I've completed all setup steps**.
6. Read the instructions labeled **Important**.
7. Click **Install & Launch VR Game**. If Chrome opens a USB device picker, select your Quest and click **Connect**. If the headset asks again about USB debugging, select **Allow**.

That installs the sample VR game on the headset and starts it. Leave the headset connected until it finishes.

## 5. How to play the game again

The game does not sit in the normal app library.

1. Put on the Quest.
2. Press the **Quest** button on the right controller to open the menu.
3. Select **Library**.
4. Select **Menu** on the left side of that panel.
5. Select **Unknown Sources**.
6. Select **The Shooting Range Game**.

## Done when

- The dashboard shows your account on a developer team, and verification is complete.
- The phone app shows **Developer Mode** on for your headset.
- **The Shooting Range Game** starts from the website, or you can open it from **Unknown Sources**.

## If this fails

- **Developer Mode is not in the phone app.** Finish the organization and verification on the dashboard, then force-close the phone app and open it again.
- **The website cannot see the headset.** Unlock the Mac and click **Allow** on the USB accessory prompt. Use a USB-C cable that carries data, not only power. Put the headset on and select **Allow** on the USB debugging prompt. If Chrome shows a device picker, select the Quest and click **Connect**.

If you later want to build games with this engine, leave this page. That setup is for Windows, in the [Quest setup guide](README.md).
