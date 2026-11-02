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
                                              | cmake_build OR quest_build |
                                              | (must succeed)             |
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
3. Try `builtin+config_code_edit` (copy a built-in's source and edit it into
   a NEW generated mechanic; the built-in itself is never modified, and the
   copy takes over the base built-in's player `role:`).
4. Only emit `generate` when no catalog entry reasonably fits.

Deterministic reconciles then run over the agent's plan: the player-roles
reconcile (`PLAYER_ROLES` in `engine_catalog.py` — `role:fingers`,
`role:body`, `role:teleport`, `role:slide_head`, `role:slide_controller`,
`role:snap_turn`, `role:smooth_turn`, `role:movement_settings_menu`; one
mechanic per role, none for roles the game lacks) and the weapon reconcile
(`weapon_plan.py`), which makes every designed weapon `builtin+config` of
`builtin.weapon` / `bow` / `melee_weapon` / `shield` (+ `item_source`,
`npc.ranged_attack`) and removes generated weapon tasks and `builtin.recoil`.

The stage emits an SSE event `info: reuse=N (builtin/builtin+config),
generate=M` so the UI can highlight how much of the game came from the
shared engine vs. how much was newly generated.

### 3. Promotion (`stage_engine_promotion.py`)

After `cmake_build` or `quest_build` succeeds, the promotion stage walks
`engine/generated/{mechanics,ai,dialogue}/<project_id>/` and indexes
every successfully-compiled `.cpp` (paired with its `.codetask.json`
sidecar) into a single registry file:

```
engine/promoted/registry.json
```

Each entry there shows up on the next pipeline run as a `promoted.<id>`
catalog entry. The C++ source itself never moves: it stays under
`engine/generated/<kind>/<project_id>/`. Builds compile only the current
game's generated code (`PTV_GENERATED_PROJECT_ID`), because different games
routinely pick the same task names and would collide at link time. So when
a plan reuses a `promoted.<id>` from another game, the orchestrator passes
that task's `.cpp` files in `PTV_GENERATED_EXTRA_SOURCES`
(`engine_catalog.promoted_sources_for_plan`). This happens for the
compile-verify build, the PC VR build and the Quest build.

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

2. **Reference images + Tripo3D** generate object GLBs (characters, props).
   The **city environment** is authored later by the world-generation stage
   (`IWorldBuilder` C++ → `world.scene.json`), not as a Tripo GLB.

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

4. **World generation** writes procedural city geometry into
   `world.scene.json`. It runs BEFORE code generation, so gameplay code is
   written against the ids the world actually declared.

5. **Code-generation stage** writes
   `engine/generated/mechanics/<pid>/superman_hover_toggle.cpp` and
   `engine/generated/mechanics/<pid>/superman_flight.cpp`, both
   subclassing `IMechanic` from the public engine surface only (flight uses
   `ctx.locomotion->set_vertical_mode(VerticalMode::Free)`).
   **Scene-compile + cmake_build** (or `quest_build`) then compile the
   runtime + new mechanics and link the launcher.

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
  "smooth/sliding locomotion"), the BuildSpec `movement` block lists just
  that one. If a travelling game's prompt is silent, you get all three
  locomotion modes plus `builtin.movement_settings_menu`. The engine injects
  no player mechanic: `scene_compile` binds exactly the roles the BuildSpec
  names on `player_rig`, and a game with no locomotion is a valid stationary
  game. See [`docs/locomotion.md`](locomotion.md).

* Promotion is conservative: it only registers mechanics whose
  `cmake_build` or `quest_build` succeeded. Broken builds never pollute the catalog.

* The catalog is observable: `GET /api/engine/catalog` returns the
  exact JSON the planner is reasoning over.

## Where the code lives

| Concern              | File                                                                       |
| -------------------- | -------------------------------------------------------------------------- |
| Static built-in list + `PLAYER_ROLES` | `apps/orchestrator/ptv_orchestrator/engine_catalog.py`      |
| Weapon reconcile     | `apps/orchestrator/ptv_orchestrator/pipeline/weapon_plan.py`               |
| Reuse-first prompts  | `apps/orchestrator/ptv_orchestrator/pipeline/prompts.py`                   |
| Catalog injection    | `apps/orchestrator/ptv_orchestrator/pipeline/stage_mechanic_plan.py`       |
| Promotion stage      | `apps/orchestrator/ptv_orchestrator/pipeline/stage_engine_promotion.py`    |
| Pipeline wiring      | `apps/orchestrator/ptv_orchestrator/pipeline/orchestrator.py`              |
| HTTP endpoint        | `apps/orchestrator/ptv_orchestrator/api/routes.py` (`GET /api/engine/catalog`) |
| Promoted registry    | `engine/promoted/registry.json` (auto-generated)                           |
| Locomotion details   | [`docs/locomotion.md`](locomotion.md)                                      |
