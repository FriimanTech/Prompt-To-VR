---
name: Public setup docs
overview: Update the top-level README and add linked public setup guides for Quest-first onboarding, API keys, and a separate PC VR requirements page. Documentation only. This plan is not implemented yet.
todos:
  - id: readme-onboarding
    content: Rewrite the top of README.md into the five onboarding sections, in order, with both setup links, both API links, explanations A/B/C, the funding table, and all six examples. Keep useful existing README material after that and fix nearby onboarding text that conflicts with Quest-first OpenAI setup.
    status: completed
  - id: setup-index-and-tools
    content: Add docs/setup/README.md plus 01-python.md (from Appendix A), 02-nodejs.md, 03-visual-studio.md, and 04-build-tools-and-uv.md. Install uv in guide 04 before any uv command. Each guide is a numbered install and configuration tutorial with official links and a simple done check.
    status: completed
  - id: quest-and-android
    content: Add 05-quest-developer-mode-and-adb-drivers.md and 06-android-studio.md last, in that order. Cover Meta team/organization, Developer Mode, the Windows driver install, then Android Studio through env vars. Set JAVA_HOME to the jbr folder, add its bin to Path, reopen the terminal, and run java -version. Run adb devices only after Platform-Tools.
    status: completed
  - id: pcvr-and-apikeys
    content: Add pc-vr-requirements.md (shared tools versus Quest-only tools, with the untested Quest Link warning) and api-keys.md (four providers, official account and key steps, .env names, placeholders, the OpenAI fallback, and the three-key exception).
    status: completed
  - id: crosslinks-and-check
    content: Point local-setup.md and provider-setup.md at the new guides where current onboarding conflicts. Then check that the requested sections, tools, explanations, amounts, examples, and guide order are present, and that changed Markdown links work. Summarize changed files and anything still unresolved. Do not claim a build or headset test.
    status: pending
isProject: false
---

# Public GitHub setup documentation

Update [README.md](README.md) and the linked setup docs so a new visitor can understand the engine and install it. Use [VR_Engine_Documentation_Cursor_Prompt.md](c:\Users\micha\.cursor\projects\c-Users-micha-Documents-Prompt-To-VR\attachments\06c7359b-18bb-40a6-b23f-afa7c9988ca8\VR_Engine_Documentation_Cursor_Prompt.md) for the introduction, versions, examples, amounts, and organization. Appendix A below is the Python source.

Documentation only. Do not change engine behavior, provider selection, or dependency versions. Write beginner-friendly steps. Add a detail when it helps someone finish setup (a required component, an environment variable, a copyable command, or a short note about a common problem). Do not add a research log, a requirements matrix, a verification date on every page, or a separate validation system.

If a supplied requirement conflicts with the repository or current official instructions, explain that conflict briefly. Do not invent a fix or silently change the supplied requirement.

## What already exists

- [README.md](README.md) currently leads with PC VR and treats Anthropic as recommended. The public onboarding is Quest-first and recommends OpenAI, with no Claude key.
- Install facts already live in [docs/local-setup.md](docs/local-setup.md), [docs/provider-setup.md](docs/provider-setup.md), [docs/build-and-run.md](docs/build-and-run.md), and [docs/quest-build.md](docs/quest-build.md). There is no `docs/setup/` yet. Reuse those pages for startup and technical reference. Do not rewrite `docs/All_Stages_Docs/`.
- Android source material is [Helper_Folder/VR_Engine_Deployment/android-studio-setup-quest.md](Helper_Folder/VR_Engine_Deployment/android-studio-setup-quest.md). The repo pins API 34, `minSdk` 32, NDK `30.0.15729638`, AGP 8.7.0, the Gradle 8.9 wrapper, OpenXR loader `1.0.34`, `arm64-v8a`, Android SDK CMake `3.22.1+`, and `adb`. `fastboot` is unused. [engine/android/app/build.gradle](engine/android/app/build.gradle) sets Java source/target compatibility to 17. Readers still set `JAVA_HOME` to Android Studio’s `jbr` folder, as specified below.
- [.env.example](.env.example) already has `OPENAI_API_KEY`, `GOOGLE_API_KEY`, `TRIPO_API_KEY`, and `UTHANA_API_KEY`. [docs/local-setup.md](docs/local-setup.md) is stale where it says `UTHANA_API_KEY` is missing from that example.

## Facts the instructions must follow

- Packages require Python `>=3.11`. The project environment in the existing workflow is `uv python install 3.13` and `uv venv .\vrvenv --python 3.13`. Appendix A’s Python 3.14 is only an example of `python --version` output.
- The initial Python install and the uv 3.13 `vrvenv` are different. The engine runs in `vrvenv`. Install `uv` before any `uv` command. Cross-link between the Python guide and the build-tools guide so that order stays obvious.
- Regular LLM stages (BuildSpec, AssetManifest, optimization preferences) use `LlmClient`: Claude is tried first, and a missing Claude key falls back to OpenAI ([packages/provider-clients/ptv_providers/llm_client.py](packages/provider-clients/ptv_providers/llm_client.py)). Most AI stages already use the OpenAI Agents SDK and `OPENAI_API_KEY` only: interaction design, NPC plan, game design, mechanic plan, scene spec, character routing, Uthana motion plan, world generation, and code generation. The recommended setup leaves `ANTHROPIC_API_KEY` unset. Do not add a new provider switch.
- The three-key case is exactly **Hands Only, no full-body avatar, and no in-game NPCs**: OpenAI, Gemini, and Tripo3D. Do not widen that case from internal routing code, and do not say Hands Only alone removes the need for Uthana.
- A Quest run still needs the Windows desktop toolchain, because code generation’s compile check builds with MSVC and desktop CMake before the NDK syntax check. Android Studio is additional. A PC-VR-only workflow does not need Android Studio, the NDK, Platform-Tools, or the Oculus ADB Drivers. It does need Quest Link and the Oculus OpenXR runtime.
- Desktop CMake **4.2+** and Android SDK CMake **3.22.1+** are different installs. The repo ships `gradlew`. Do not tell readers to install a global Gradle. `ANDROID_SDK_ROOT` is optional. Required Android variables are `JAVA_HOME`, `ANDROID_HOME`, and `ANDROID_NDK_HOME`, with `adb` on `PATH`.

## README

Put these five sections first. After them, keep the useful repo map, pipeline, and first-run commands, and update nearby text that still tells people to start with Claude or to treat PC VR as the main path.

1. **Introduction.** Keep the three supplied paragraphs: FriimanTech; Prompt to VR Engine; Meta Quest 3 with some PC VR support; no Unity, Unreal, or Godot; C++, Vulkan, and OpenXR; agents that plan gameplay, generate 3D assets, automate rigging and animation, build environments, and write gameplay code; compiler errors fed back to the coding agents; reusable systems for interactions, movement, NPC behavior, and game rules; a local pipeline that uses external AI services; native installable Quest APKs rather than browser WebXR; a foundation to explore and extend; Quest Link tested early; recent work focused on standalone Quest 3.
2. **Prerequisites.** Windows is required. A Quest 3 or Quest 2 is highly advised. Recent testing focused on standalone Quest 3; do not present Quest 2 as equally tested. The engine can create an installable APK without a headset. A headset is needed to play the game and to test it on the device. Cursor is highly advised; other IDEs are acceptable. Cursor’s agent can help with some dependency setup and is especially useful for changing the engine. A substantial amount of setup is still manual, especially Visual Studio Community 2026 and Android Studio. Separate what is required to build from what is needed to test on a headset.
3. **Quest downloads.** A short table, in this order, with a plain-language role for each tool:

   | Tool | Version or requirement |
   | --- | --- |
   | Python | **3.11+**; **3.13 via `uv` recommended** |
   | Node.js | **20+** |
   | Visual Studio Community 2026 | **Desktop development with C++ / MSVC v145** |
   | vcpkg | As required by the repository |
   | CMake | **4.2 or newer**, desktop tooling |
   | Vulkan SDK | **1.4.341.x** |
   | uv | Python environment manager |
   | Oculus ADB Drivers | Windows drivers for a Quest used in development |
   | Android Studio | **SDK Platform 34**, **NDK `30.0.15729638`**, **Platform-Tools / `adb`**, **JDK 17**, plus the other required Android pieces named in the Android guide |

   Put the same prominent link to [docs/setup/README.md](docs/setup/README.md) before and after the table. Keep the install steps in the linked guides.

   Add a clearly labeled link or GitHub `<details>` section, **PC VR through Quest Link: requirements**, pointing at [docs/setup/pc-vr-requirements.md](docs/setup/pc-vr-requirements.md). Beside it, state: **PC VR through Quest Link has not been tested recently and may have major issues. The recommended path is standalone Quest, especially Quest 3, because recent development and testing have focused there.**

4. **API providers.** Four short rows: OpenAI for planning, reasoning, and code generation, including Agents SDK stages; Gemini for reference images used to create 3D assets; Tripo3D for the 3D objects and characters; Uthana for rigging and animation where needed, including the full-body avatar. Put the same link to [docs/setup/api-keys.md](docs/setup/api-keys.md) before and after the list. Do not put account or billing tutorials in the README. Keep these explanations:
   - **A.** Some stages use regular LLM calls. Most AI stages use OpenAI Agents through the OpenAI Agents SDK. Use OpenAI for all of those stages instead of configuring both Claude and OpenAI. The full setup is four keys: OpenAI, Gemini, Tripo3D, and Uthana.
   - **B.** Recent updates were tested with OpenAI, not Claude. For the recommended setup, use OpenAI and do not configure a Claude key.
   - **C.** Hands Only, with no full-body avatar and no in-game NPCs, needs only OpenAI, Gemini, and Tripo3D. That scenario does not need a Uthana key.
5. **Funding.** These are recommended available USD budgets, not the price of a key and not a guaranteed charge for one game. Actual use depends on the prompt, assets, animations, model choices, retries, and the work performed. Keep every amount, including the comfortable OpenAI amounts:

| Game size | OpenAI | Gemini | Tripo3D | Uthana |
   | --- | --- | --- | --- | --- |
| Small prompt games | $50; comfortable: $75 | $5 | $10 | $5 |
| Medium prompt games | $100; comfortable: $150 | $10 | $25 | $10 |
| Large prompt games | $200; comfortable: $300 | $25 | $50–75 | $20–25 |

   Include the medium row even though there is no medium example. Uthana funding does not apply to the three-key scenario. Keep these examples, with spelling and punctuation fixes only:

   - Small: a goblin shooter with a laser blaster and 5 waves of goblins. A simple shooting range with one pistol and floating targets.
   - Large: a Navy SEAL-style raid across two rooms, with an assault rifle, a sidearm, a full-body avatar, 3 enemy types, ammo, and reload. An open combat campaign with a desert outpost, underground bunker, and rooftop finale; full avatar; assault rifle, shotgun, grenades, and shield; soldiers, drones, and heavy brutes; scripted waves, objectives, and a pause/end-game flow. A multi-room museum heist with stealth and shooting, laser puzzles, guards with varied weapons, and a getaway courtyard; full avatar, several unique characters, and many grabbable props. A survival arena with three biomes linked by portals, a crafting bench, ranged and melee loadouts, and rotating boss waves.

   Do not treat the examples as a promise that generation or performance will succeed. Explain provider billing in the API guide only as far as a reader needs in order to add funds. Do not change these amounts.

## Setup guides

[docs/setup/README.md](docs/setup/README.md) is the start of the walkthrough: prerequisites, the ordered links, and a short completion checklist. The first tool guide is Python. Both README download links point here.

Use the Android guide’s numbered style. Each tool guide explains what the tool does for this engine, links to the official download, and gives full install and configuration steps. Include required components, paths, environment variables, the shell name on commands, and a simple confirmation where that helps. Add links back to the index and to the previous and next guide, plus a short troubleshooting note when a common problem is worth naming. Do not require a large troubleshooting section for every tool.

Only vcpkg, desktop CMake, Vulkan SDK, and uv may share a file. Keep the Meta developer / Oculus ADB Drivers guide and the Android Studio guide last, in that order. Keep PC VR requirements and API keys outside this ordered list.

| File | Content |
| --- | --- |
| [docs/setup/01-python.md](docs/setup/01-python.md) | Appendix A, rewritten as steps. |
| [docs/setup/02-nodejs.md](docs/setup/02-nodejs.md) | Node.js 20+ for `apps/web`. |
| [docs/setup/03-visual-studio.md](docs/setup/03-visual-studio.md) | Visual Studio Community 2026, Desktop development with C++, MSVC v145, C++ CMake tools, Windows SDK. Generator `Visual Studio 18 2026`. |
| [docs/setup/04-build-tools-and-uv.md](docs/setup/04-build-tools-and-uv.md) | vcpkg (`VCPKG_ROOT`, `bootstrap-vcpkg.bat`), desktop CMake 4.2+, Vulkan SDK 1.4.341.x, then uv. After `uv` works, create the 3.13 `vrvenv` and install the Python packages with the existing repo commands. |
| [docs/setup/05-quest-developer-mode-and-adb-drivers.md](docs/setup/05-quest-developer-mode-and-adb-drivers.md) | Device setup, developer team/organization, Developer Mode, and the Windows driver install. |
| [docs/setup/06-android-studio.md](docs/setup/06-android-studio.md) | Android Studio install and the configuration after it, including the first `adb devices` check. |
| [docs/setup/pc-vr-requirements.md](docs/setup/pc-vr-requirements.md) | Full PC VR list from the actual PC VR workflow. |
| [docs/setup/api-keys.md](docs/setup/api-keys.md) | OpenAI, Gemini, Tripo3D, and Uthana account and key setup. |

After the tools are installed, the setup index links to the existing startup instructions in [docs/local-setup.md](docs/local-setup.md) and [docs/build-and-run.md](docs/build-and-run.md). Check those commands against the repo while writing. Readers without a headset can finish the build tools and return to the device steps later.

### Python and uv

Adapt Appendix A into written steps. Do not publish the narration as-is. Preserve the official Python download page, **Python Install Manager**, **Launch when ready** where the installer shows it, Command Prompt, `python --version`, and the Microsoft **Python** extension in VS Code, including **Pylance**, **Python Debugger**, and **Python Environments** when the editor offers them. Give the equivalent Cursor steps where Cursor supports them.

The transcript answers `N`, `y`, and `Y` before the known question. The known one is **View online help?** → **N**. For the earlier answers, use the current official Install Manager instructions to name the choice. If a prompt’s wording cannot be confirmed, say so. Do not invent the question, and do not tell readers to type those letters blindly.

Python 3.14 in the transcript is an example. It does not replace the recommended 3.13 project environment. Explain that the Install Manager provides a working Python, and that guide 04 later creates the uv-managed 3.13 environment the engine uses.

### Meta developer setup and drivers

Use the official references from the original request, or the current page if one has moved:

- [Meta device setup](https://developers.meta.com/vr/documentation/native/android/mobile-device-setup/)
- [Create or join a developer organization](https://developers.meta.com/vr/manage/organizations/create/)
- [Oculus ADB Drivers](https://developers.meta.com/vr/downloads/package/oculus-adb-drivers/)

Walk through having a headset available for on-device testing, signing in and joining or creating a developer team/organization, enabling Developer Mode, and installing the Windows drivers. Say whether each action happens on the dashboard, in the mobile app, or on the headset. Include a data-capable USB cable and the USB-debugging prompt. Put `adb devices` in the Android Studio guide, after Platform-Tools exists.

Quest 3 is the recently tested target. Quest 2 is advised, not equally tested. For the original Quest 1 mention, add a brief limitation from the current official setup page if that page states one. Do not run a separate headset-compatibility study.

### Android Studio and JAVA_HOME

Turn the Helper Folder guide into a public page: repository-relative links, example paths only, no editor-only links. Cover installing Android Studio, opening the SDK Manager, installing **Android SDK Platform 34**, the side-by-side NDK **`30.0.15729638`**, Build-Tools 34.0.0 or newer, Platform-Tools / `adb`, command-line tools, and Android SDK CMake **3.22.1+**. Sources, emulator images, API 26, and Android 16 / Baklava are not required. Confirm the SDK and NDK folders, then connect the Quest and accept USB debugging so `adb devices` shows `device`.

Java setup stays short:

1. Set `JAVA_HOME` to `C:\Program Files\Android\Android Studio\jbr`, adjusting the path if Studio is installed elsewhere. Point it at the `jbr` folder, not at `jbr\bin`.
2. Add `%JAVA_HOME%\bin` to the user `Path`.
3. Close and reopen the terminal and IDE.
4. Run `java -version` and confirm Java starts.

Also set `ANDROID_HOME` and `ANDROID_NDK_HOME`, and add `platform-tools` to `Path`. Mention in one sentence, only so a newer `java -version` line does not look like a failed install, that Android Studio’s bundled runtime may be newer than 17 while this project’s Java language level remains 17. Do not ask readers to compare compatibility tables or choose another JDK. Use the repo’s Gradle wrapper. Say briefly how these tools feed `quest_build` and on-headset verification, with links to [docs/quest-build.md](docs/quest-build.md).

### PC VR requirements

List what the PC VR workflow actually needs, and mark which of those tools are shared with the Quest walkthrough. Include Quest Link and selecting the Oculus OpenXR runtime. Do not copy Android-only tools into this list. Repeat the warning that PC VR through Quest Link has not been tested recently and may have major issues, and recommend standalone Quest, especially Quest 3.

### API keys

For OpenAI, Gemini, Tripo3D, and Uthana, give the current official console link and the steps to sign in, turn on API access and billing or credits where that provider requires it, create a key, and place it in the repo-root `.env` under the real variable name. Mention extra settings the engine actually reads when a reader needs them to succeed, such as the Gemini image model and a Tripo region base URL. Use placeholders. Say that real keys belong in the local ignored `.env` and must not be committed.

Show the recommended setup as OpenAI with `ANTHROPIC_API_KEY` left empty, so the existing fallback handles the regular LLM stages. If that variable is already set in Windows and would keep selecting Claude, the practical fix is to clear it for the shell that starts the orchestrator and start the orchestrator again. Do not expand that into a settings-library or environment-precedence lesson.

Repeat explanations A, B, and C, including the exact three-key exception. A local check such as confirming the variable is present in `.env` is enough. Do not require a live paid API call for each provider. If a signed-in dashboard control cannot be confirmed, say that instead of inventing the button.

## Existing pages to touch

- [docs/local-setup.md](docs/local-setup.md): point installation at `docs/setup/README.md`, keep command blocks that still match, and correct the stale Uthana / `.env.example` sentence.
- [docs/provider-setup.md](docs/provider-setup.md): keep the technical provider reference, point beginners to `docs/setup/api-keys.md`, and state that the recommended public setup uses OpenAI without a Claude key.
- Leave stage deep-dives and engine code unchanged.

## Finish

When the docs are written, check that the five README sections, every requested tool and version, explanations A/B/C, every funding amount, all six examples, and the guide order are present. Check that changed Markdown is readable and that relative links resolve. Then summarize the files created or updated, how the README reaches the setup index, what was checked by reading, and any real unresolved conflict. Do not claim a successful build or headset test unless one was actually run.

## Appendix A — Python source to adapt

Hey everyone, in this video we are going to install Python and set up the Python extensions inside VS Code. Go ahead and open your browser and search for Python download. You should see the official Python website. Go ahead and click Download Python. Once you are on the Python download page, Click on Download Python install Manager. Once the installer finishes downloading, double click to open it. When the installer appears make sure Launch when ready is checked, then click Install let the Python Install Manager finish installing. Once it finishes you will see this terminal popup. Follow along with the options you see on my screen. The questions you receive may be slightly different depending on how Windows is already configured on your computer.

- For the first question shown on my screen, type capital N and press Enter.
- For the second question, type lowercase y and press Enter.
- For the third question, type capital Y and press Enter.
- When it asks View online help?, type capital N and press Enter. Close the Terminal Window.

Now go to the Windows search bar, type CMD, and press Enter to open Command Prompt. Inside Command Prompt type in python --version and hit enter where you should see the version of Python installed on your computer, such as Python 3.14, although your version may be different from mine depending on what the latest version of Python is when you are watching this video. As long as you see a Python version and do not receive an error saying that Python cannot be found, your Python installation is working correctly.

Now, let's open VS Code. Inside VS Code, Click on the Extensions tab on the left hand side. Then in the search bar, type Python. Click on the extension called Python by Microsoft. Then Click Install. When you install this Python extension, VS Code may also install related tools like Pylance, Python Debugger, and Python Environments. That is completely fine. These tools help VS Code understand your Python code and make it easier to write, run, debug, and work inside Python projects.

Once the Python extension is installed, we are ready to begin building our project. In the next video, we will set up our project environment inside VS Code. See you there!
