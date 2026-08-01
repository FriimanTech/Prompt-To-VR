# Engine growth: catalog + reuse-first planner + promotion

The Prompt-to-VR system is designed so that **every game built on it makes
the engine more capable**. This document describes the three pieces of
plumbing that make that work: the engine catalog, the reuse-first planner,
and the promotion stage.

## Three roles, one contract

```
                +----------------------+
                |   ENGINE CATALOG     |
                |  (single source of   |
                |   truth, Python)     |
                +----------+-----------+
                           |
        seen by the planner |        served by
        as prompt context   |        GET /api/engine/catalog
                           v
+------------------+   +---v-----------------+   +----------------------+
| BuildSpec stage  |-->| Mechanic-plan stage |-->| Code-generation stage|
+------------------+   +---------------------+   +----------+-----------+
                                                            |
                                                            v
                                              engine/generated/<kind>/<pid>/*.cpp
                                                            |
                                              +-------------v-------------+
                                              | cmake_build (must succeed) |
                                              +-------------+-------------+
                                                            |
                                                            v
                                              +-------------+-------------+
                                              |   engine_promotion stage  |
                                              |  appends to               |
                                              |  engine/promoted/         |
                                              |     registry.json         |
                                              +-------------+-------------+
                                                            |
                                                  next pipeline run reads this
```

### 1. The catalog (`engine_catalog.py`)

`apps/orchestrator/ptv_orchestrator/engine_catalog.py` is the single
source of truth for what the engine can already do. Each entry has:

* `id` (canonical, e.g. `builtin.locomotion.slide.head_relative`)
* `summary` (one-liner the planner reads)
* `category` (`locomotion`, `interaction`, `combat`, `ui`, `scripting`, …)
* `params` (named with default values / types)
* `inputs` (controllers / buttons / triggers used)
* `comfort_notes` (so the planner can reason about VR comfort trade-offs)
* `aliases` (so legacy ids keep working)

It exposes `as_planner_text(repo_root)` which renders the catalog as a
self-describing block we paste into the mechanic-planner system prompt.

### 2. Reuse-first planner (`stage_mechanic_plan.py`)

The mechanic-plan stage now **always** appends the live catalog (built-in
+ promoted) to the planner prompt under the heading `ENGINE CATALOG`.
The system prompt (`MECHANIC_PLAN_SYSTEM` in `prompts.py`) instructs the
model to:

1. Try `builtin` (catalog id covers it as-is).
2. Try `builtin+config` (catalog id covers it with parameter tweaks).
3. Only emit `generate` when no catalog entry reasonably fits.

The stage emits an SSE event `info: reuse=N (builtin/builtin+config),
generate=M` so the UI can highlight how much of the game came from the
shared engine vs. how much was newly generated.

### 3. Promotion (`stage_engine_promotion.py`)

After `cmake_build` succeeds, the promotion stage walks
`engine/generated/{mechanics,ai,dialogue}/<project_id>/` and indexes
every successfully-compiled `.cpp` (paired with its `.codetask.json`
sidecar) into a single registry file:

```
engine/promoted/registry.json
```

Each entry there shows up on the next pipeline run as a `promoted.<id>`
catalog entry. The C++ source itself never moves — it stays under
`engine/generated/<kind>/<project_id>/`, where the existing engine CMake
glob already links it into `ptv_runtime`. Promotion just makes the
mechanic discoverable to future planners.

## Worked example: Superman Flight Sim

> "I want you to create a game called Superman Flight Sim, where users
> spawn in a city and can walk around. At any time, they can press a
> button to transition from standing on the ground to floating in the
> air. The same joystick used for walking should also allow them to fly
> forward, backward, left, and right. Additionally, when the user is
> hovering and looks down, they should see Superman's legs positioned
> similarly to how he appears in films, with one knee slightly higher
> than the other."

Here is what the system does, step by step:

1. **BuildSpec stage** lists the city, Superman, and acceptance tests.
   Mechanics list calls out four ideas: walking, hover, flight,
   first-person Superman embodiment.

2. **Reference images + Tripo3D** generate a city environment GLB and a
   Superman GLB.

3. **Mechanic plan stage** sees the catalog text. It classifies:

   * `walking` → `builtin.locomotion.slide.head_relative` and
     `builtin.locomotion.slide.controller_relative` and
     `builtin.locomotion.teleport` (let the player choose at runtime).
   * `snap_turn` → `builtin.locomotion.snap_turn`.
   * `hover_toggle` → first run: catalog has no hover. Plan says
     `generate` with a CodeTask for a new `IMechanic` that toggles
     a hover state on a face button.
   * `flight` → first run: catalog has no flight. Plan says `generate`
     with a CodeTask describing thumbstick-driven horizontal flight
     while in the hover state.
   * `superman_legs` → no catalog entry fits, so a `generate` task is
     emitted for a body-anchor hook that positions the legs mesh.

4. **Code-generation stage** writes
   `engine/generated/mechanics/<pid>/superman_hover_toggle.cpp` and
   `engine/generated/mechanics/<pid>/superman_flight.cpp`, both
   subclassing `IMechanic` from the public engine surface only.

5. **Scene-compile + cmake_build** stages compile the runtime + the new
   mechanics and link `ptv_runtime` + `ptv_<game>` together.

6. **Engine-promotion stage** sees the two new sidecars
   (`superman_hover_toggle.codetask.json`,
   `superman_flight.codetask.json`) and appends to
   `engine/promoted/registry.json`:

   ```json
   {
     "version": 1,
     "items": [
       {
         "id": "promoted.superman_hover_toggle",
         "summary": "Toggle a hovering state on left-A press; ...",
         "category": "mechanic",
         "kind": "mechanics",
         "source_path": "engine/generated/mechanics/<pid>/superman_hover_toggle.cpp",
         "code_task_id": "superman_hover_toggle",
         "project_id": "<pid>",
         "promoted_at": "2026-04-24T23:11:02.000Z"
       },
       { "id": "promoted.superman_flight", ... }
     ]
   }
   ```

Now suppose the next prompt is **"Iron Man Flight Sim"**. When that
project hits the mechanic-planner stage, the catalog the planner sees
already includes `promoted.superman_flight` and
`promoted.superman_hover_toggle` as reusable ids with the original
summaries and params. The planner is instructed to prefer those over
generating again. The result: the second game inherits the work the
first game paid for.

## Practical guidance for prompts

* If your prompt names a movement style explicitly ("teleport-only",
  "smooth/sliding locomotion"), the planner plans just that one. If your
  prompt is silent, you'll get all three locomotion modes plus the in-game
  picker — same as the starter room. Either way the engine attaches all
  four locomotion mechanics at scene load, since the runtime picker can
  switch to any of them; the plan decides which one is *selected* by
  default, not which ones exist.

* Promotion is conservative: it only registers mechanics whose
  `cmake_build` succeeded. Broken builds never pollute the catalog.

* The catalog is observable: `GET /api/engine/catalog` returns the
  exact JSON the planner is reasoning over.

## Where the code lives

| Concern              | File                                                                       |
| -------------------- | -------------------------------------------------------------------------- |
| Static built-in list | `apps/orchestrator/ptv_orchestrator/engine_catalog.py`                     |
| Reuse-first prompts  | `apps/orchestrator/ptv_orchestrator/pipeline/prompts.py`                   |
| Catalog injection    | `apps/orchestrator/ptv_orchestrator/pipeline/stage_mechanic_plan.py`       |
| Promotion stage      | `apps/orchestrator/ptv_orchestrator/pipeline/stage_engine_promotion.py`    |
| Pipeline wiring      | `apps/orchestrator/ptv_orchestrator/pipeline/orchestrator.py`              |
| HTTP endpoint        | `apps/orchestrator/ptv_orchestrator/api/routes.py` (`GET /api/engine/catalog`) |
| Promoted registry    | `engine/promoted/registry.json` (auto-generated)                           |
| Locomotion details   | [`docs/locomotion.md`](locomotion.md)                                      |
