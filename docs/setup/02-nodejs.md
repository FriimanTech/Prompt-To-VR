# 2. Install Node.js

Node.js and npm run the local website in `apps/web` (Vite). You need **Node.js 20 or newer**.

Official page: [https://nodejs.org/](https://nodejs.org/)

## Install

1. Open [https://nodejs.org/](https://nodejs.org/).
2. Download the Windows installer for the current **20.x LTS** or any newer 20+ release. The 22 LTS line is fine. Do not install a 19 or older release.
3. Run the installer. Keep the option that adds Node.js to **PATH**.
4. Finish the installer.
5. Close any open terminals and Cursor or VS Code, then open a new **PowerShell** window.

## Check

In PowerShell:

```powershell
node -v
npm -v
```

`node -v` should print `v20` or higher, for example `v22.14.0`. `npm -v` should print a version number.

You will run `npm install` from `apps\web` when you start the website. Those commands are in [Local setup](../local-setup.md), after the rest of the tools are in place.

## Done when

A new PowerShell prints a Node version of 20 or newer.

## If this fails

- **`node` is not recognized.** The installer did not update the terminal you have open. Close it and open a new PowerShell window. If it still fails, run the installer again and leave "Add to PATH" enabled.

Previous: [Python](01-python.md). Next: [Visual Studio](03-visual-studio.md). Back to the [setup index](README.md).
