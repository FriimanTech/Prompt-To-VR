# Stage 16b: Quest On-Headset Verification

> **Pipeline position:** after `quest_build` → before `engine_promotion`  
> **Target:** Quest only (`target_device == quest`); never in the PC VR tail  
> **Primary code:** [`apps/orchestrator/ptv_orchestrator/pipeline/stage_quest_verify.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/stage_quest_verify.py), wrapped by `_verify_with_memory_corrections` in [`orchestrator.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/orchestrator.py)  
> **Primary artifacts:** `projects/<project_id>/runtime_verification.json`, `memory_status.json`, `memory_feedback.json` (only after a correctable memory failure), `logs/runtime/verify_<ts>*.log`, `projects/memory_calibration.json`

## Main stage summary

### What this stage is

This stage installs the APK from `quest_build` on a connected Quest, runs it in the engine's verification modes, reads the engine's `ptv` logcat telemetry, and checks **only what this game asks for**. That means its NPC plan, game design, weapons, textures, the user's optimization locks and memory. The game can be launched up to six times: base, game loop, weapons, texture gallery, memory soak and memory pressure.

It is **report-only**. Nothing is repaired automatically, apart from the memory correction loop (below), which re-runs packaging stages. When no headset is available the stage is **skipped, never failed**.

### Why it exists

A clean compile, package and install proves nothing about runtime behavior. NPCs launched into the sky by their own ground probe, generated AI that was never instantiated, and hands loaded 20 m long all built and installed cleanly. Memory is the other reason: `memory_status` can only be `verified_safe` from measurements taken here.

### Inputs

| Input | Source |
|-------|--------|
| APK | `ProjectState.latest_build_apk` |
| Compiled scene | `projects/<id>/scene.json` (entities, `navigation.regions`, `performance` block: locks, textures, memory estimate, targets) |
| NPC plan | `npc_plan` artifact (optional; a game without NPCs still gets base checks) |
| Game design | `game_design` artifact (optional; enables the game-loop pass) |
| Memory compromises | `asset_normalization.json` → `__memory__.compromises` |
| Offline renderer | newest `ptv_glb_views[.exe]` under `engine/build/` (texture gallery reference) |
| Headset | `adb devices` |

### Outputs

| Output | Location |
|--------|----------|
| Verification report (`RuntimeVerification`: status, reason, checks, npc_summary, performance, memory) | `projects/<id>/runtime_verification.json` |
| Device memory status | `projects/<id>/memory_status.json` (`source: "device"`) |
| Correction feedback | `projects/<id>/memory_feedback.json` (`round`, reasons, measured, measured/estimated `ratio`) |
| Calibration (all projects) | `projects/memory_calibration.json` via `memory_model.update_calibration` |
| Logs | `logs/runtime/verify_<YYYYmmdd-HHMMSS>.log`, plus `_flow`, `_weapons`, `_textures`, `_soak`, `_pressure` siblings |
| Screenshots | `logs/runtime/textures_verify_<ts>/{background_first,background,asset_NN}.jpg` |

Stage artifacts are the report and the base log.

### Where output goes next

- `engine_promotion` runs when this stage succeeds or is skipped.
- `GET /projects/{id}/apk` reads `memory.status` from `runtime_verification.json` into the `X-PTV-Memory-Status` header. The APK is downloadable whatever the result.
- `asset_optimization` and `scene_compile` read `memory_calibration.json` on the next build, including a correction round.

### Failure behavior

- **Skipped** (stage recorded `succeeded` with message `skipped: <reason>`, event `skipped`, report status `skipped`, memory = the build-time estimate `estimated_safe_not_device_tested` or `at_risk`). This happens when the project is not Quest, `quest_runtime_verify` is off, there is no APK, adb is missing or unusable, there is no device, the device is `unauthorized`/`offline`, several devices are connected and none is configured, or the headset disconnects mid-run (`DeviceGone`).
- **Failed:** any check with outcome `failed` makes the stage record `failed` and raise `RuntimeError("runtime verification failed: …")`, which stops the pipeline (so `engine_promotion` does not run). Memory failures are listed first. Install failure or a game that will not start after six launch attempts also raises.
- `warning` and `not_applicable` outcomes never fail the stage.

---

## Detailed stage deep dive

### Pass structure (`run_with_adb`)

**Setup.** If `quest_verify_store_pin` is set, Meta Scriptable Testing (`content://com.oculus.rc SET_PROPERTY`) disables Guardian, dialogs and autosleep. The stage saves and then sets `debug.oculus.guardian_pause 1`, broadcasts `com.oculus.vrpowermanager.prox_close` so the unworn headset counts as worn, sets `debug.ptv.verify 1`, runs `adb install -r` (600 s timeout), and clears logcat and the crash buffer. Logcat `-v time -s ptv:*` is **streamed to disk** for every pass, because the ring buffer holds under a minute of telemetry.

**Launching** (`start_game`). The stage waits 8 s, runs `am start -n <package>/android.app.NativeActivity` (no `-W`), and polls `pidof` for 15 s. It tries up to 6 times, with `prox_close` + `KEYCODE_WAKEUP` + `KEYCODE_BACK` between attempts to clear the Guardian interstitial. An exit is only declared after two failed `pidof` 2 s apart.

| # | Pass | Runs when | Properties | Duration |
|---|------|-----------|-----------|----------|
| 1 | Base | always | `debug.ptv.verify=1` (scripted square walk, player cannot die) | NPC games: `start_delay_s + quest_verify_capture_seconds + 35 s × index of the latest first-appearing wave`, ≤ 240 s; no NPCs: 20 s. `dumpsys meminfo` every 5 s; `gpumeminfo` once past half-time |
| 2 | Game loop | a `game_design` exists | `verify_flow=1` (the player can die; engine clicks end-screen buttons and finally Leave Game) | `75 + 17 × deaths + survive_time/10`, ≤ 300 s; after an exit, relaunch once with `verify=0` |
| 3 | Weapons | the scene has player `builtin.weapon` / `builtin.bow` entities (not templates, attached or faction-tagged) | `verify=1`, `verify_weapons=1` | `25 + 9 × weapons`, ≤ 240 s, or until `weapons: done` |
| 4 | Texture gallery | `performance.textures.textured_mesh_entities > 0` | `verify=1`, `verify_textures=1` | `6 + 4 × (meshes + 2) × 1.6 + 30` s, or until `texgallery: done`; screenshots via `com.oculus.metacam/.capture.CaptureService TAKE_SCREENSHOT`, pulled from `/sdcard/Oculus/Screenshots/` |
| 5 | Memory soak | `quest_memory_soak_seconds > 0` | `verify=1` | `quest_memory_soak_seconds`, PSS every 15 s |
| 6 | Memory pressure | `quest_memory_pressure_test` | `verify=1`, `debug.ptv.simulate_memory_pressure` | 12 s, then thresholds scaled to `min(0.5, max(0.001, 0.5 × measured PSS / 4608))` for 20 s, then `1` for 20 s |

A low-memory-killer scan (`logcat -d -b all`) runs after each pass. **Cleanup always runs** (`finally`): force-stop, all `debug.ptv.verify*` back to `0`, `simulate_memory_pressure` back to `1`, Guardian pause restored, `vrpowermanager.automation_disable`, and Scriptable Testing off.

### Checks (category: what is checked)

- **base** (every game): app loaded its scene (`Loaded scene`), frame loop ran, no crash (process alive, no `Fatal signal`/`FATAL EXCEPTION`), every mechanic binding resolves, every GLB loaded. Hands Only games also check that the default hands are drawn (both hands finite, joint span 0.08–0.45 m, within 1.2 m of the head). Textures load (`tex:` line: none missing, at least one resident if textured), and textured games check that the headset uses ASTC (`decoded == 0 or ktx2 > 0`).
- **persistent / stationary / follower**: each NPC placed at load is in the scene, is reported, has its skeleton and resolved clips (when animated), stands on the floor (median drawn-feet gap ≤ 5 cm, worst ≤ 1 m; hover ±0.15 m; flying not applicable), faces travel within 20° median, and stays inside nav regions. Stationary NPCs drift ≤ 0.25 m, play idle, and attack from their spot if they have an attack behaviour. Followers stay within `max_distance_m + 3.5 m` in ≥ 80% of samples while the player walks, walk, and are in a gait clip ≥ 60% of the time. Other movers move.
- **encounter / enemy**: waves start, and the first wave spawns `min(max_concurrent, wave size)` within `start_delay + 15 s`. Each spawned type spawns, gets floor/facing/area checks, and if it is a chaser hostile to the player it closes distance, plays a gait clip and lands hits on the player.
- **game_flow**: the loop starts. It then checks either that the player cannot die (games without failure) or that health 0 costs a life, the player respawns (when lives > 1 or unlimited), Game Over follows the last life with `lives=0`, Try Again restarts, and a restart resets lives. Winnable games reach victory, and Play Again restarts. Leave Game ends the app, and the app launches again afterwards.
- **weapons** (per weapon): the pass ran to `weapons: done`, the weapon is held, it fires (≥ 5, or the magazine size if smaller; 1 for beam/drawn), shots leave within 2 cm of the muzzle, it hits the target, damage lands through `ctx.combat`, a beam produces ≥ 10 damaging ticks in 2 s, the projectile pool stays under `performance.max_live_projectiles` (default 64), and frame time holds with p95 ≤ budget × 1.15 and ≤ 2% dropped frames. That last check is not applicable if the headset was not presenting.
- **textures** (gallery, per asset): the asset shows on the headset. The stage compares the screenshot minus background against a 256 px `ptv_glb_views` render (`view_6`) of the same LOD for visibility, magenta, chroma and white-out (`texture_gallery.judge`).
- **render** (if `Renderer ready` was logged): the FFR density map reaches the render pass (fails on `no fragment density map image`). A dissolved static batch or a `GPU allocation failed` line is a **warning**.
- **optimization** (only with user locks in `performance.locks`): every `perf:` line honors the locked FFR (`off` = 0, `fixed` = level, `max` ≤ level) and resolution (scale stays 1.0). `blocked by user settings` is a **warning**, never a failure.
- **memory** (`evaluate_memory`, measured data only): memory was measured at all, peak PSS ≤ `performance.memory_ceiling_mb` (5120), steady PSS (median of the second half of samples) ≤ `memory_target_mb` (4608), no low-memory kill naming the package, no leak across restarts (≤ 50 MB per cycle from `mem: pss` after each `state=playing`), no soak growth (slope ≤ 5 MB/min after 60 s warm-up, ≥ 3 samples), and memory pressure handled (reached red/critical and still running). Meta's limit `memory_limit_mb` (5888) is reported.

`performance` in the report is `summarize_perf(parse_perf_lines(...))` from the base log.

### Memory status and the correction loop

`_record_memory` always writes `memory_status.json`, and updates `memory_calibration.json` when both an estimated and a measured peak exist. It writes `memory_feedback.json` only when the status is `failed_on_device` **and** a correctable check failed (peak PSS, steady PSS or low-memory kill). Leaks and pressure failures are reported, not rebuilt around. In any other case it deletes a stale feedback file.

`_verify_with_memory_corrections` (orchestrator) deletes any previous `memory_feedback.json`, then runs this stage. If the stage raises `RuntimeError` **and** the feedback file exists **and** fewer than `quest_memory_max_correction_rounds` rounds have run, it emits a `warning` and re-runs `asset_optimization` → `scene_compile` → `quest_build` → this stage. Those stages pick up the new ratio through `memory_calibration.json`. Any other failure, or running out of rounds, re-raises. Each round's APK stays downloadable.

`MemoryStatus` values: `verified_safe`, `failed_on_device` (measured), and `estimated_safe_not_device_tested` / `at_risk` (skipped; from `scene.json` `performance.memory_estimate.outcome`).

### Settings (`settings.py`, `.env`, no prefix)

| Setting | Env var | Default |
|---------|---------|---------|
| `quest_runtime_verify` | `QUEST_RUNTIME_VERIFY` | `True` |
| `quest_verify_capture_seconds` | `QUEST_VERIFY_CAPTURE_SECONDS` | `40.0` |
| `quest_verify_device_serial` | `QUEST_VERIFY_DEVICE_SERIAL` | unset |
| `quest_verify_store_pin` | `QUEST_VERIFY_STORE_PIN` | unset |
| `quest_memory_soak_seconds` | `QUEST_MEMORY_SOAK_SECONDS` | `600.0` (0 disables) |
| `quest_memory_pressure_test` | `QUEST_MEMORY_PRESSURE_TEST` | `True` |
| `quest_memory_max_correction_rounds` | `QUEST_MEMORY_MAX_CORRECTION_ROUNDS` | `2` |
| `adb_path` | `ADB_PATH` | `adb` on PATH |
| `android_package_prefix` | `ANDROID_PACKAGE_PREFIX` | `com.prompttovr` (package name) |

Tolerances are module constants in `stage_quest_verify.py` (`FEET_TOLERANCE_M`, `FACING_TOLERANCE_DEG`, `STATIONARY_DRIFT_M`, `SHOT_ORIGIN_TOLERANCE_CM`, `LEAK_MB_PER_CYCLE`, `SOAK_MB_PER_MIN`, …).

### Helpers

- `quest_metrics.py`: `parse_dumpsys_meminfo`, `parse_gpumeminfo`, `parse_mem_lines` (engine `mem:` lines), `low_memory_kills`, `parse_perf_lines`, `summarize_perf`.
- `texture_gallery.py`: `parse_gallery_lines`, `load_rgb`, `screenshot_object_stats`, `reference_stats`, `judge`.
- `memory_model.py`: `update_calibration` (and `load_calibration`, used by the stages that re-run).

### Events

`running` → `info` (install, launch attempts, per-pass budgets, one line per applicable check; failed checks as `warning`) → `succeeded` | `failed` | `skipped`. During correction rounds the wrapper also emits `warning: memory failed on the headset; correction round N of M…`.

## See also

- [`docs/quest-build.md`](../../../quest-build.md): on-headset verification
- [`docs/quest-optimization.md`](../../../quest-optimization.md): memory targets, locks, calibration
- [`docs/game-flow.md`](../../../game-flow.md), [`docs/weapons.md`](../../../weapons.md), [`docs/npc-and-ai.md`](../../../npc-and-ai.md)
- Previous: [`16_quest_build`](../16_quest_build/README.md); next: [`17_engine_promotion`](../17_engine_promotion/README.md)
