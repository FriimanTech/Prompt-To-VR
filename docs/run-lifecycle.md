# Run lifecycle: starting, aborting, and recovering

A "run" is one pass through some or all of the pipeline for one project. The
web panel starts runs through `POST /api/projects/{id}/run` in four modes
(full, resume, resume-from, single stage), all of which land in
`apps/orchestrator/ptv_orchestrator/pipeline/orchestrator.py`.

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
