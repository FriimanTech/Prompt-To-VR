# 1. Install Python

Python runs the local pipeline. This page installs a working Python with the official **Python Install Manager**. The engine's project environment is created later, in [vcpkg, CMake, Vulkan SDK, and uv](04-build-tools-and-uv.md), as **Python 3.13** inside a folder named `vrvenv`. Packages require **Python 3.11 or newer**. A newer version from this page, such as 3.14, is fine for this check. It does not replace 3.13 for the project.

Official pages:

- [Python downloads](https://www.python.org/downloads/)
- [Python Install Manager releases](https://www.python.org/downloads/latest/pymanager/)
- [Using Python on Windows](https://docs.python.org/3/using/windows.html)

## Install the Python Install Manager

1. Open a browser and go to [https://www.python.org/downloads/](https://www.python.org/downloads/).
2. Choose the Windows download, then **Download Python Install Manager** (the installer may also be listed on the [Install Manager release page](https://www.python.org/downloads/latest/pymanager/)). The Microsoft Store app is the approach Python recommends for most people. The downloaded installer is the one these steps follow.
3. When the download finishes, double-click the installer.
4. If you see **Launch when ready**, leave it checked.
5. Click **Install** and wait until the Install Manager finishes.
6. A terminal window opens and runs a configuration helper. The questions depend on what is already installed on your PC. **Read each question.** Do not type a fixed sequence of letters.

Answer these if they appear:

| Prompt | Answer | Why |
| --- | --- | --- |
| **Add commands directory to your PATH now? [y/N]** | `y` | Puts the Python commands on your PATH so a new Command Prompt can find `python`. |
| **Install CPython now? [Y/n]** | `Y` | Installs a Python runtime. The version is whatever Python currently calls latest. The source walkthrough used 3.14 only as an example. |
| **View online help? [y/N]** | `N` | Closes the helper. You do not need the help page to continue. |

Other questions can appear, and some of the rows above can be skipped when Windows is already set up. If you are asked about **app execution aliases**, enable the entries labeled **Python (default)** and **Python install manager** when `python` is not found later. If you are asked to enable **long paths**, allowing them helps on this repo because some build paths are long; say yes if you can approve the change. If you are asked about a legacy **Python launcher**, uninstalling that old launcher avoids a clash with the new `py` command.

7. Close the terminal when the helper says configuration checks are done.

## Check Python in Command Prompt

1. Press the Windows key, type `CMD`, and press Enter. This opens **Command Prompt**, not PowerShell.
2. Run:

```bat
python --version
```

You should see a line such as `Python 3.14.x`. Your number can differ. Any version line means this install works. An error that Python cannot be found means the PATH entry or the app execution alias is missing. Open a **new** Command Prompt after changing PATH. If it still fails, open **Manage app execution aliases** in Windows Settings and enable **Python (default)**.

## Editor extension

**Cursor** is the IDE this project recommends. **VS Code** is fine too.

### VS Code

1. Open VS Code.
2. Open the **Extensions** view on the left.
3. Search for `Python`.
4. Install **Python** by Microsoft.
5. VS Code may also install **Pylance**, **Python Debugger**, and **Python Environments**. Leave those installed. They help the editor understand, run, and debug Python.

### Cursor

1. Open Cursor.
2. Open **Extensions** (the same square icon, or View → Extensions).
3. Search for `Python` and install **Python** by Microsoft if it is not already installed.
4. If Cursor offers **Pylance**, **Python Debugger**, or **Python Environments**, install those too.

You will point the editor at `vrvenv` after [guide 4](04-build-tools-and-uv.md) creates it. This page does not run `uv`.

## Done when

In a new Command Prompt, `python --version` prints a version and does not say Python cannot be found.

## If this fails

- **Python was not found.** Install CPython from the helper (`py install default` in a new terminal also works), enable the Python app execution aliases, then open a new Command Prompt.
- **The helper questions do not match this page.** Answer the question that is on your screen. The three answers above are for those exact prompts only.

Next: [Node.js](02-nodejs.md). Back to the [setup index](README.md).
