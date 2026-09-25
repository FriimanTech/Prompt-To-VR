# Uthana Avatar Rig Quest Test

Self-contained playtest: auto-rig `player_range_avatar` with **Uthana** (fingers included), then package a Quest 3 APK that uses that GLB as your first-person body.

This folder does **not** run LLM / agent pipeline stages. It only uses Uthana’s API plus the deterministic `quest_build` stage.

## What you get

- First-person body = Uthana-rigged avatar (head hidden in self-view, arm IK, no leg animation)
- Head-relative slide locomotion
- Grab / throw: range pistol + a few cubes
- Finger curls from trigger / grip / thumbrest (or face buttons)

## Engine note (required for finger test)

`builtin.hand_pose` previously curled only the engine hand fixtures. When a `player_avatar` has finger bones, `avatar_body` hides those fixtures — so Uthana fingers never moved.

A **minimal** change in [`engine/runtime/src/mechanics/builtin/hand_pose/HandPoseMechanic.cpp`](../engine/runtime/src/mechanics/builtin/hand_pose/HandPoseMechanic.cpp) also applies the same curl targets to the `player_avatar` skeleton when BodySlots resolve finger joints. No other engine systems were touched.

### Thumb input limitation

The engine exposes **thumbrest** / face-button capsense, not a separate “thumbstick touched” bit. Rest your thumb on the thumbrest (or press A/B/X/Y) to fold the thumb. Literal thumbstick-touch would need another OpenXR binding change.

## Layout

```
Uthana_Test/
  requirements.txt
  README.md
  scripts/
    _paths.py
    rig_avatar.py       # Uthana upload + download
    inspect_rig.py      # joint / BodySlots probe
    build_quest_apk.py  # quest_build via local ProjectStore
  projects/
    uthana-avatar-rig-test/
      project_state.json   # target_device: quest
      scene.json           # hand-authored playable scene
      assets/
        player_range_avatar/model.glb
        player_range_avatar/uthana_rig.json
        range_pistol/model.glb
```

Global `PROJECTS_DIR` / the main `projects/` tree are **not** modified. `build_quest_apk.py` points `ProjectStore` at `Uthana_Test/projects` only.

## Prerequisites

- Repo venv: `vrvenv` (Python 3.13)
- `UTHANA_API_KEY` in the repo-root `.env`
- Android / Quest build tools already working for this repo (`gradlew`, NDK, `adb`, Quest Link / wireless debug)
- Source meshes (read-only):
  - `projects/20260907-144543-shooting-range-game/assets/player_range_avatar/model.glb`
  - `projects/20260907-144543-shooting-range-game/assets/range_pistol/model.glb`

## Setup

```powershell
cd C:\Users\micha\Documents\Prompt-To-VR
uv pip install --python .\vrvenv\Scripts\python.exe -r .\Uthana_Test\requirements.txt
```

## 1. Auto-rig with Uthana

```powershell
.\vrvenv\Scripts\python.exe .\Uthana_Test\scripts\rig_avatar.py
```

Parameters forced on the GraphQL upload:

| Param | Value |
|-------|--------|
| `auto_rig` | `true` |
| `auto_rig_front_facing` | `true` |
| `include_fingers` | `true` |
| `auto_rig_strip_namespace` | `true` |

(The published `uthana` Python client omits `auto_rig_strip_namespace`, so the script posts the documented multipart mutation itself.)

## 2. Inspect the skeleton

```powershell
.\vrvenv\Scripts\python.exe .\Uthana_Test\scripts\inspect_rig.py
```

Expect Mixamo-style names (`LeftHandIndex1`, …) and all probed BodySlots = OK. Finger bones should be present (~30 finger joints).

## 3. Build the Quest APK

```powershell
.\vrvenv\Scripts\python.exe .\Uthana_Test\scripts\build_quest_apk.py
```

This calls `stage_quest_build.run` with `ProjectStore(Uthana_Test/projects)`. No BuildSpec / MechanicPlan / SceneSpec / worldgen / codegen agents.

First Gradle/NDK runs can take a long time. Log: `Uthana_Test/projects/uthana-avatar-rig-test/logs/gradle.log`.

## 4. Install on Quest 3

The script prints the APK path and `adb` commands, typically:

```powershell
adb install -r "<path-to-apk>"
adb shell am start -n com.prompttovr.uthana_avatar_rig_test/android.app.NativeActivity
```

(Exact package name depends on `android_package_prefix` in settings.)

## Playtest checklist

1. Boot into the room; look down — see torso/arms (not the inside of the skull).
2. Thumbstick: slide (head-relative).
3. Trigger / grip / thumbrest: avatar fingers curl; release grip to drop non-equipped props.
4. Grab pistol + cubes; throw them.

## What was deliberately skipped

- All LLM / agent stages (`build_spec`, `npc_plan`, `mechanic_plan`, `scene_spec`, `world_generation`, `code_generation`, Tripo, etc.)
- Generated C++ under `engine/generated/`
- Changes to renderer, OpenXR, physics core, or schemas (beyond the hand_pose wiring above)
