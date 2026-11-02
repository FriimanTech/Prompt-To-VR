# Run lifecycle: starting, aborting, and recovering

A "run" is one pass through some or all of the pipeline for one project. The
web panel starts runs through `POST /api/projects/{id}/run` in four modes
(full, resume, resume-from, single stage), all of which land in
`apps/orchestrator/ptv_orchestrator/pipeline/orchestrator.py`.

## Full run, Resume, Resume from, Run only

| Button | Runs | Reuses earlier output? |
|---|---|---|
| **Run full pipeline** | every stage, from the prompt | No. The previous run is archived first |
| **Resume** | from the first stage whose latest record is not `succeeded` | Yes |
| **Resume from…** | the chosen stage and everything after it | Yes, for stages before it |
| **Run only…** | just the chosen stage | Yes |
| **Verify on headset** (Quest only) | `quest_runtime_verify` via `POST /api/projects/{id}/verify` | Yes; refused for PC VR projects |

The request body picks the mode: `stage` → `run_stage`, `resume_from` →
`resume_from_stage`, `resume: true` → `resume_pipeline`, none →
`run_pipeline`. The stage list comes from `_stages_for(target_device)`:
the shared stages, then `cmake_build` + `engine_promotion` (PC VR) or
`quest_build` + `quest_runtime_verify` + `engine_promotion` (Quest).

Every stage is written to make a Resume cheap:

- the codegen board keeps finished tasks;
- asset stages reuse any image, mesh, rig or clip already on disk under the same asset id;
- ledgers remember paid results.

On a full run the same reuse would leak the previous game into the new one. Old codegen tasks would be claimed and fail the stage, assets with a recycled id would be reused even though their description changed, and scene files of scenes the new plan dropped would be globbed in. So `run_pipeline` first calls `run_reset.archive_previous_run`, which moves the following into `projects/<id>/previous_runs/<UTC stamp>/`:

- every top-level project file except `prompt.txt` and `project_state.json`;
- `logs/`, `images/`, `assets/` and `generated_code/`;
- `engine/generated/{mechanics,ai,dialogue,world}/<id>/`, which must leave the tree the desktop and Android builds glob;
- the stage history. Records from the archived run would otherwise make Resume skip stages this run has not reached.

`builds/` stays: it holds only build caches, rebuilt from sources. `full_run_archive_keep` (default 1) sets how many archives are kept; 0 deletes the previous run instead. If a file stays locked, the run stops before any paid stage, naming the file.

Resume paths also defend themselves against leftovers:

- **The codegen board** drops tasks the current MechanicPlan no longer has, deletes the files only they wrote, and regenerates a finished task (and its dependents) when its CodeTask changed.
- **Stale scene files:** `scene_spec` removes `scene_spec.<id>.json` and `scene_compile` removes `scene.<id>.json` for scenes the BuildSpec no longer has.
- **Ledgers keyed by asset id** (`asset_rig.json`, `asset_animation.json`, `tripo_rig_check.json` and the Uthana ledgers) drop assets the manifest no longer has.
- **Paid results are recorded before files move.** Rigging and animation store the provider task id as `downloaded` before moving the file into place, so a move that fails is finished on the next run instead of being paid for again.

## One run per project

`apps/orchestrator/ptv_orchestrator/runs.py` holds a `RunRegistry` mapping a
project id to a live `RunHandle`. Claiming the project is how the orchestrator
enforces one run at a time: two concurrent `cmake.exe` processes against the
same build directory race on the vcpkg install lock and one inevitably dies
with `STATUS_CONTROL_C_EXIT`. A second run request emits a `skipped` SSE event
and returns rather than raising, because it is usually just a double-click.

## Abort

`POST /api/projects/{id}/abort`, or the **Abort** button in the project header
(enabled only while the project is running).

Abort does two things:

1. Sets the handle's cancel flag. Long-running stages check
   `ctx.raise_if_cancelled()` at their loop boundaries -- between stages, per
   asset, per scene, per repair round, per codegen task claim, per compile
   round -- and raise `RunCancelled`. The orchestrator records the current
   stage as `aborted`.
2. Kills the child processes the run registered. On Windows this is a
   `taskkill /F /T` on the process tree, because cmake spawns msbuild which
   spawns the compilers: terminating only the parent would leave them running
   and the build directory locked.

What Abort cannot do is recall a request already sent to a provider. A Tripo
generation you have already paid for keeps running on their side; whatever
artifacts landed on disk are kept, and a **Resume** reuses them instead of
re-billing. Cancellation is cooperative, so a stage stops at its next
checkpoint rather than instantly.

After an abort the project's status is `aborted`, which is deliberately
distinct from `failed`: nothing is broken, and Run / Resume / Resume-from are
all immediately available.

## Recovering from a killed orchestrator

`ProjectState.status` lives on disk; the run registry lives in memory. So
pressing Ctrl+C in the orchestrator terminal mid-stage used to leave
`"status": "running"` persisted with nothing running -- and because the web
panel disables every button while a project claims to be running, the project
became permanently untouchable until someone hand-edited
`project_state.json`.

The app's lifespan hook (`main.py`) calls `reconcile_interrupted_runs()` at
startup. A process that has just started owns no runs, so every project still
marked `running` -- or with a stage record left at `running` and no terminal
record after it -- is by definition a leftover and gets stamped `aborted` with
the message `interrupted by an orchestrator restart`. The stage that was
interrupted stays as `current_stage`, so **Resume** picks up exactly there.

The Abort button performs the same reconciliation when no run is in flight, so
it doubles as a manual unstick.

## Resuming older projects after new stages are added

**Resume** restarts at the first stage whose latest record is not
`succeeded`. A stage added to the pipeline later (for example
`optimization_preferences`, `interaction_design`, `game_design`,
`character_routing`, `uthana_rigging`, `uthana_motion_plan`,
`uthana_animation`, `socket_generation`) has no record at all in an older project, so Resume starts
there and runs everything after it.

The paid stages after it keep their ledgers, so they re-bill nothing:

- `asset_rig.json`
- `asset_animation.json`
- `uthana_rig.json`
- `uthana_animation.json`

Uthana motions are additionally reused by name across the whole Uthana
organisation, and `socket_generation` caches each asset's result by GLB hash +
spec + pipeline version (`assets/<id>/sockets/result.json`).

`world_generation` now runs **before** `code_generation` (codegen reads the
`world*.scene.json` files for the ids the world declared). Resume from
`world_generation` re-runs both; Resume from `code_generation` reuses the
world already on disk.

## The Quest memory correction loop

On Quest, `quest_runtime_verify` is wrapped by
`_verify_with_memory_corrections`. When the headset measures a memory failure
the stage writes `memory_feedback.json` (and updates
`projects/memory_calibration.json`); the orchestrator then re-runs
`asset_optimization` → `scene_compile` → `quest_build` → verify, at most
`quest_memory_max_correction_rounds` (default 2) times, emitting a
`correction round N of M` warning each time. A stale `memory_feedback.json`
from a previous run is deleted first. Every round's APK stays downloadable.

## Terminal status is guaranteed

Every orchestrator entry point runs inside `_claimed_run`, which stamps a
terminal status (`succeeded`, `failed` or `aborted`) on the way out no matter
how the run ends. Previously `resume_pipeline` and `run_stage` had no failure
handler at all, so an exception there left `running` behind and produced the
same lockout as a Ctrl+C.

## Statuses

| Status | Meaning | Buttons enabled |
|--------|---------|-----------------|
| `idle` | Created, never run | Run, Resume |
| `running` | A run is in flight | Abort |
| `succeeded` | The last run finished its stages | Run, Resume, Launch |
| `failed` | A stage raised | Run, Resume |
| `aborted` | The user aborted, or the server restarted mid-run | Run, Resume |
