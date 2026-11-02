# Stage 4: MechanicPlan

> **Pipeline position:** after `game_design` → before `scene_spec`  
> **Target:** shared (PC VR and Quest)  
> **Primary code:** [`apps/orchestrator/ptv_orchestrator/pipeline/stage_mechanic_plan.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/stage_mechanic_plan.py)  
> **Primary artifacts:** `projects/<project_id>/mechanic_plan.json`

## Main stage summary

### What this stage is

The **MechanicPlan stage** decides how every gameplay mechanic will be
implemented: reuse an engine built-in as-is, tune its parameters, copy and
edit a built-in's C++ into a new generated mechanic, or generate new C++. It
assigns a **controller binding** (control + hand) to input-driven mechanics
and emits a nested **CodeTask** for everything that needs code. A chain of
deterministic reconciles then forces the plan to agree with the BuildSpec's
player roles, the NPC plan, the game design and the interaction design.

### Why it exists

The engine ships reusable VR mechanics (locomotion, turning, grab, hand pose,
avatar body, NPC behaviours, the game loop, the weapon set). Regenerating them
per game wastes time and breaks determinism, and a second generated copy (a
private lives counter, a private gun) fights the built-in. This stage is
**reuse-first** and is where those overlaps are removed before any C++ is
written. It also defines the CodeTask dependency graph (`depends_on`) that
`code_generation` schedules from.

### Inputs (what enters)

| Input | Location | Used for |
|-------|----------|----------|
| BuildSpec | `build_spec.json` | One plan item per BuildSpec mechanic; `movement` and `player_experience` decide player roles |
| AssetManifest | `asset_manifest.json` | Asset ids CodeTasks may name |
| NpcPlan | `npc_plan.json` | Included in the brief when it has NPCs; drives NPC built-ins |
| GameDesign | `game_design.json` | Pasted into the brief; drives the game-loop reconcile |
| InteractionDesign | `interaction_design.json` | Weapons excerpt in the brief; drives the weapon reconcile |
| Engine catalog | `engine_catalog.py` `as_planner_text()` — built-ins plus `promoted.*` entries from `engine/promoted/registry.json` | What can be reused |
| Editable built-ins note | `builtin_source.editable_builtins_note()` | Which built-ins can be code-edited and where their source lives |

### Outputs (what it produces)

| Output | Location |
|--------|----------|
| MechanicPlan | `projects/<id>/mechanic_plan.json` |
| Agent + reconcile log | `projects/<id>/logs/mechanic_plan.log` |

Each `MechanicPlanItem` has `id`, `classification`, `builtin_id` (builtin /
builtin+config), `base_builtin_id` (code edit), `config`, optional `binding`
and optional `code_task`.

### Where output goes next

- **`scene_spec`** — binding ids: `builtin.*` or `generated.<code_task_id>`; `config` is forwarded.
- **`code_generation`** — runs every `code_task` in `depends_on` order (it re-runs the NPC-AI, game-loop and weapon reconciles at start, so older plans are corrected too).
- **`scene_compile`** — materializes bindings and config into `scene.json`.
- **`engine_promotion`** — promoted mechanics re-enter the catalog for later games.

### Models / providers

- OpenAI Agents SDK agent (`MechanicPlanner`), `gpt-5.6-sol`, reasoning effort `high`, verbosity `medium`, **no tools**.
- System prompt `mechanic_plan_system(target_device)` — the device clause names Quest's mobile budget (tighter triangles / draw calls, fewer animated characters) or PC VR.
- No other provider. Cost: one agent run plus schema retries.

### Failure behaviour

- An agent error after retries raises and **fails the stage** (no fallback plan).
- The binding validator only normalizes and warns; the "binding conflicts" failure branch in the stage is effectively unreachable because `validate_bindings` never records errors.
- Reconciles never fail; every change is emitted as `info` and logged.

---

## Detailed stage deep dive

### Files and functions

| File | Role |
|------|------|
| `pipeline/stage_mechanic_plan.py` | `run(ctx)` → `run_with_planner(ctx, planner=None)` (tests inject a stub) |
| `pipeline/mechanic_plan_agent.py` | `MechanicPlanner`, `MechanicPlanAgentInput`, `build_planner_message` |
| `engine_catalog.py` | `as_planner_text`, `PLAYER_ROLES`, `role_of_plan_item`, promoted registry |
| `pipeline/builtin_source.py` | `editable_builtins_note` |
| `pipeline/binding_validator.py` | `validate_bindings` |
| `pipeline/movement.py` | `required_roles`, `reconcile_mechanic_plan` |
| `pipeline/npc_population.py` | `reconcile_npc_mechanics`, `reconcile_npc_ai_items` |
| `pipeline/game_flow_plan.py` | `reconcile_game_flow_items` |
| `pipeline/weapon_plan.py` | `reconcile_weapon_items`, `weapon_params` / `bow_params` / `melee_params` / `shield_params` |
| `packages/schemas/ptv_schemas/mechanic_plan.py` | `MechanicPlan`, `MechanicPlanItem`, `ControllerBinding`, `BUILTIN_MECHANIC_IDS` |

### Execution trace

1. Emit `running`; load BuildSpec, manifest; render the catalog.
2. Bridge `OPENAI_API_KEY`; build `MechanicPlanner` (`session_id=ptv-mechanic-plan-<project_id>`).
3. Load NpcPlan, GameDesign, InteractionDesign. Build the brief: BuildSpec, manifest, NpcPlan (only if it has NPCs), catalog, editable-built-ins note, and extra instructions:
   - one item per BuildSpec mechanic (1:1 by id); escalate params → code_edit → generate only as needed;
   - hub mechanics are generated first and branches `depends_on` them;
   - the exact **player roles** list from `required_roles(build_spec)`, and no other locomotion / turning / finger / body / movement-menu items;
   - **NPC population**: spawning belongs to `builtin.encounter_director`; follow → `builtin.npc.follow`, chase + melee → `builtin.npc.melee_chase`; generate only what they cannot express;
   - **GAME LOOP** rule plus the full `game_design.json` (or "no game design");
   - **WEAPONS** rule plus the interaction-design excerpt (or "no weapons are designed").
4. `planner.run()` → `MechanicPlan`.
5. `validate_bindings` (warnings emitted as `warn`).
6. `reconcile_mechanic_plan` (player roles).
7. `reconcile_npc_mechanics`, then `reconcile_npc_ai_items`.
8. `reconcile_game_flow_items`.
9. `reconcile_weapon_items`.
10. Save `mechanic_plan.json`; emit `reuse=…, code_edit=…, generate=…`; succeed with "N to generate, M reused".

### Classifications

| Classification | Required fields | CodeTask? |
|----------------|-----------------|-----------|
| `builtin` | `builtin_id` | No |
| `builtin+config` | `builtin_id`, `config` | No |
| `builtin+config_code_edit` | `base_builtin_id`, `code_task` — copies the built-in's source into a NEW generated mechanic; the built-in is never replaced | Yes |
| `generate` | `code_task` | Yes |

`MechanicPlan.items` has no upper bound (min 1). `depends_on` lists other CodeTask ids; reconciles prune dependencies on tasks they remove.

### Binding validator (`validate_bindings`)

Controls: `trigger`, `grip`, `thumbstick`, `a`, `b`, `x`, `y`, `none`; hands `left` / `right` / `either`.

- A/B forced to `right`, X/Y to `left`, with a warning.
- Warns when a mechanic binds a control the built-ins use (left thumbstick = locomotion, right thumbstick = snap turn, both grips = grab / throw) unless its id or built-in id looks like that system.
- Sharing a control across mechanics is allowed and never an error.

### Player roles reconcile (`movement.reconcile_mechanic_plan`)

The engine injects no player mechanic, so the plan must name exactly the roles the BuildSpec implies (`required_roles`):

| Role | Built-in | When |
|------|----------|------|
| `role:fingers` | `builtin.hand_pose` | every game |
| `role:body` | `builtin.avatar_body` | Full Player Avatar only |
| `role:teleport` / `role:slide_head` / `role:slide_controller` | `builtin.locomotion.teleport` / `.slide.head_relative` / `.slide.controller_relative` | one per `movement.modes` entry (none for a stationary game) |
| `role:snap_turn` / `role:smooth_turn` | `builtin.locomotion.snap_turn` / `builtin.locomotion.smooth_turn` | one per `movement.turning` entry |
| `role:movement_settings_menu` | `builtin.movement_settings_menu` | only when there is a choice (2+ modes, both turn styles, or a vignette user toggle on a motion that exists) |

- Missing roles get their stock built-in (ids `hand_pose`, `avatar_body`, `teleport`, `slide_head_relative`, …).
- Two items for one role collapse: a `builtin+config_code_edit` replacement beats its base, otherwise the first wins.
- Items filling a role the game does not have are removed.
- Motion mechanics never blink / fade / black out: such config keys are dropped; a code-edit copy whose only addition was a blink becomes the stock built-in, any other copy gets an engine rule appended to its task.
- Stock slide and smooth-turn items get `vignette.enabled` / `user_toggle` from `movement.vignette`; `role:body` gets `coverage`, `head_visibility`, `leg_animation` from `player_avatar`.
- A legacy BuildSpec without a `movement` block is mapped from `locomotion` (`effective_movement`).

### NPC reconciles (`npc_population.py`)

- `reconcile_npc_mechanics` adds `builtin.encounter_director` (item `encounter_director`) when the NPC plan has encounters, `builtin.npc.follow` (`npc_follow`) for follow-only NPCs, and `builtin.npc.melee_chase` (`npc_melee_chase`) for chase / melee NPCs — with empty config; the scene compiler fills per-type numbers from the NPC plan.
- `reconcile_npc_ai_items` converts an `ai` CodeTask to `builtin+config` when every NPC it drives walks or slides and all their behaviours fall inside one built-in's coverage (`follow`: follow_leader, idle; `melee_chase`: chase, melee_attack, react_to_damage, idle). Config keys the built-in does not read are dropped and named; dependents lose the dependency.

### Game-loop reconcile (`game_flow_plan.py`, only with a GameDesign)

- A generated item whose id / title looks like the loop (run / game flow, lives, respawn, game over, victory / results / end screen, restart, pause, quit, player health — but not pickups, enemies, NPCs, bosses) becomes `builtin+config` of `builtin.game_flow`.
- A HUD showing only health / lives / timer / objective becomes `builtin.game_hud`; a HUD with game-specific content (ammo, waves, crosshair, minimap…) stays generated with a note not to redraw what `builtin.game_hud` shows.
- Dependents of converted tasks lose the dependency and are told to read `ctx.game`.

### Weapon reconcile (`weapon_plan.py`, only with an InteractionDesign)

- Every designed weapon gets a `builtin+config` item with config from the design: `weapon_<asset>` (`builtin.weapon`, binding trigger / either), `bow_<asset>` (`builtin.bow`, binding grip / either), `melee_weapon_<asset>`, `shield_<asset>`; `item_source_<asset>` (`builtin.item_source`) for quivers / body-slot sources; `ranged_attack_<npc>` (`builtin.npc.ranged_attack`) for NPCs with `ranged_attack`.
- Removed, named: `builtin.recoil` items; existing engine-weapon items (re-added from the design); generated firing / ammo / reload / recoil / projectile / tracer / melee-hit / shield-block / bow-draw items (not HUDs, scoring targets, NPCs, pickups, holsters…); a code-edit copy of `builtin.grab` holding a designed weapon becomes `builtin.grab`.
- Dependents (or tasks that merely name a removed task) lose the dependency and are told to use `ctx.weapons` / `ctx.projectiles`.
- Idempotent: an already-reconciled plan comes back unchanged.

### Settings (`apps/orchestrator/ptv_orchestrator/settings.py`)

| Setting | Env var | Default |
|---------|---------|---------|
| `mechanic_plan_model` | `MECHANIC_PLAN_MODEL` | `gpt-5.6-sol` |
| `mechanic_plan_reasoning_effort` | `MECHANIC_PLAN_REASONING_EFFORT` | `high` |
| `mechanic_plan_verbosity` | `MECHANIC_PLAN_VERBOSITY` | `medium` |
| `mechanic_plan_agent_max_turns` | `MECHANIC_PLAN_AGENT_MAX_TURNS` | 40 (4–200) |
| `mechanic_plan_max_retries` | `MECHANIC_PLAN_MAX_RETRIES` | 1 (0–4) |

### SSE events

`running` → `warn` (binding normalization) → `info` (`player roles:` / `NPCs:` / `game loop:` / `weapons:` notes, reuse summary) → `succeeded`.

## See also

- [`docs/engine-growth.md`](../../../engine-growth.md) — catalog and promotion
- [`docs/input-and-bindings.md`](../../../input-and-bindings.md) — controller bindings
- [`docs/locomotion.md`](../../../locomotion.md) — player roles and movement
- [`docs/game-flow.md`](../../../game-flow.md), [`docs/weapons.md`](../../../weapons.md), [`docs/npc-and-ai.md`](../../../npc-and-ai.md)
- Stage 13 [`13_code_generation`](../13_code_generation/README.md) — CodeTask DAG execution
