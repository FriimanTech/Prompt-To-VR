> **Shared content:** Identical to [PC_VR_Stages/03b_game_design](../../PC_VR_Stages/03b_game_design/README.md). Update both copies when this stage changes.

# Stage 3b: Game Design

> **Pipeline position:** after `npc_plan` → before `mechanic_plan`  
> **Target:** shared (PC VR and Quest)  
> **Primary code:** [`apps/orchestrator/ptv_orchestrator/pipeline/stage_game_design.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/stage_game_design.py)  
> **Primary artifacts:** `projects/<project_id>/game_design.json`

## Main stage summary

### What this stage is

The **game design stage** decides the rules of the game loop, once, from the
prompt: what kind of experience it is, whether the player has health, how
many lives, what happens when a life is lost (where they respawn, after how
long, how protected, what happens to nearby enemies), the objectives, how the
game is won or lost, which end screens exist and what each button does, the
pause menu, what a restart resets, and what the HUD shows. The result is
`game_design.json`.

### Why it exists

Before this stage every generated game invented its own run flow. One APK
reached zero health with no defined answer: the generated director paused,
no screen appeared and the player was stuck in a frozen arena. The engine now
runs the loop itself (`builtin.game_flow` + `builtin.game_hud`) from this
document, so the loop is designed once and is guaranteed to have no dead
ends. Generated code never keeps lives, respawns, restarts or quits; it reads
`ctx.game` and reports custom objectives.

### Inputs (what enters)

| Input | Location | Used for |
|-------|----------|----------|
| User prompt (verbatim, first 20,000 chars) | `projects/<id>/prompt.txt` | The only source of `explicit` decisions; quotes are checked against it |
| BuildSpec | `build_spec.json` | Game type, target device, `prompt_id` |
| NpcPlan | `npc_plan.json` | Who can hurt the player; whether waves exist |
| AssetManifest | `asset_manifest.json` | Real asset ids for `collect` objectives |
| InteractionDesign (optional) | `interaction_design.json` | Held items / weapons excerpt (the HUD can show ammo) |

### Outputs (what it produces)

| Output | Location |
|--------|----------|
| GameDesign | `projects/<id>/game_design.json` |
| Agent log | `projects/<id>/logs/game_design.log` |

### Where output goes next

- **`mechanic_plan`** — the design is pasted into the planner brief, and `reconcile_game_flow_items` turns any generated run flow / lives / respawn / end screen / player-health item into `builtin+config` of `builtin.game_flow`, and a plain HUD into `builtin.game_hud`.
- **`world_generation`** — declares the spawn points `reach_location` objectives name.
- **`code_generation`** — the `ctx.game` contract and the loop-ownership checks (API contract v4).
- **`scene_compile`** — binds one `ptv_game_director` entity (`builtin.ui_panel` + `builtin.game_flow` + `builtin.game_hud`) and fails if the loop cannot complete in that scene.
- **`quest_runtime_verify`** (Quest only) — the game-loop launch (`debug.ptv.verify_flow`) checks exactly what this design promises.

### Models / providers

- OpenAI Agents SDK agent ("game designer"), `gpt-5.6-sol`, reasoning effort `high`, verbosity `medium`, **no tools**, prompt `GAME_DESIGN_SYSTEM`.
- Output type `GameDesign`, non-strict schema; Pydantic validates.
- No other provider. Cost: one run, plus schema retries and up to `game_design_max_repair_rounds` revision runs.

### Failure behaviour

| Case | Result |
|------|--------|
| Agent unavailable / unusable output after retries | `warning`; `fallback_design()` derived from the NPC plan is used; stage succeeds |
| Validator finds problems | Sent back to the agent (up to `game_design_max_repair_rounds`), then deterministic repair |
| An `explicit` quote not found in the prompt | Downgraded to `inferred` |
| Design still incomplete after all of that | Stage **fails** with `GameDesignError` ("game design is still incomplete: …") |

---

## Detailed stage deep dive

### Files and functions

| File | Role |
|------|------|
| `pipeline/stage_game_design.py` | `run(ctx)` → `run_with_designer(ctx, designer=None)` (tests inject a stub); `_downgrade_unquoted`, `_items_line`, `_read_prompt` |
| `pipeline/game_design_agent.py` | `GameDesigner` (lazy SDK import, schema-retry loop), `GameDesignAgentInput`, `build_designer_message` |
| `pipeline/game_design_rules.py` | `game_design_violations`, `repair_game_design`, `fallback_design`, `hostile_attackers` |
| `packages/schemas/ptv_schemas/game_design.py` | `GameDesign` and its parts |

### Execution trace

1. Emit `running`; load BuildSpec, AssetManifest, NpcPlan and the prompt.
2. Bridge `OPENAI_API_KEY` into `os.environ`; build `GameDesigner` with `session_id=ptv-game-design-<project_id>`.
3. Message: prompt, BuildSpec, NpcPlan and manifest JSON, plus extra instructions — the target device ("menus are world-space panels the player points at; keep end screens to at most three buttons") and the interaction-design items excerpt.
4. `designer.run()`. On `GameDesignAgentError` → `fallback_design`, `source = fallback`.
5. Otherwise, up to `game_design_max_repair_rounds` times: run `game_design_violations`; if there are problems, ask for the complete design with them fixed (a failed revision stops the loop).
6. Stamp `prompt_id` from the BuildSpec; `repair_game_design` (each fix emitted as `info`).
7. Re-validate; if problems remain, `_downgrade_unquoted` turns unverifiable explicit decisions into inferred ones and validates again.
8. Still problems → `failed` + `GameDesignError`. Otherwise save `game_design.json` and succeed with a summary such as `combat (designer): health 100, lives 3, 1 objective(s), screens ['game_over', 'victory']`.

### Provenance

Every major decision (`player_vitality`, `lives`, `on_life_lost`, `victory`, each `failure`, each objective) has a `provenance`: `explicit` with a `prompt_quote` that must appear verbatim in the prompt (whitespace and case ignored), or `inferred` with a `rationale`. Explicit decisions win; BuildSpec text never counts as explicit. Nothing the game does not need is invented — a book reader has no health, lives or Game Over.

### Validator (`game_design_violations`)

All problems are reported at once, worded for the designer:

- Explicit decisions without a quote, or with a quote not in the prompt.
- NPCs that attack the player (`hostile_to` contains `player`, an attack behaviour, damage > 0) but the player has no health and is not `invulnerable`.
- Lives without health; `lives_exhausted` failure without a player that can die.
- Objectives: `survive_time` needs `duration_s`; `defeat_count` needs `count` and targets that are NPC types; `clear_waves` needs NPC-plan encounters; `collect` must name manifest assets; `reach_location` must name a spawn point (checked against world markers only when they are supplied — this stage does not pass them).
- End conditions naming unknown objectives; `all_objectives` / `any_objective` with no objectives.
- Every reachable ending (lose / win / complete) has its screen; every screen has buttons; `game_over` offers `try_again`.
- Something offers `leave_game` (an end screen or the pause menu).
- The game can end somehow, or `free_play` is set.

### Deterministic repair (`repair_game_design`)

Only completes what the design already chose: explicit-without-quote → inferred; attackers → player health (and `damage_sources`); no health → lives and `lives_exhausted` removed; lives → `lives_exhausted` failure added; `survive_time` default 180 s; `defeat_count` default 10; unknown objective ids pruned; missing Game Over (`try_again`, `leave_game`), victory / completion screens (`play_again`, `leave_game`) added; `try_again` inserted on Game Over; Leave Game added to the pause menu if nothing offers it; `free_play` when nothing can end; the HUD trimmed to the systems that exist (health when there is health, lives when more than one, timer for `survive_time`).

### Fallback design (`fallback_design`)

Derived only from earlier structure: experience from `BuildSpec.game_type` (`combat` whenever NPCs attack the player); attackers → health, 3 lives, health + lives HUD; attackers plus waves → a `clear_waves` objective that wins; then run through `repair_game_design`.

### Schema summary (`GameDesign`)

| Field | Contents |
|-------|----------|
| `experience` | `combat`, `puzzle`, `exploration`, `narrative`, `sandbox`, `score_attack`, `simulation`, `media` |
| `player_vitality` | `has_health`, `max_health` (100), `damage_sources`, `regen_per_s`, `invulnerable` |
| `lives.count` | `null` = unlimited, 1–99 |
| `on_life_lost` | `respawn_at` (spawn / checkpoint / last_safe), `delay_s` 2, `restore_health`, `keep_loadout`, `invulnerable_s` 3, `enemy_policy` (none / clear_nearby / reset_wave / reset_all), `clear_radius_m` 6 |
| `objectives` (≤16) | `survive_time`, `defeat_count`, `clear_waves`, `reach_location`, `collect`, `interact`, `custom` (reported by generated code) |
| `victory`, `failure[]` | `all_objectives`, `any_objective`, `lives_exhausted`, `timer_expired`, `custom`, `none` |
| `end_screens` (≤3) | `victory` / `game_over` / `completed`, title, subtitle, buttons (`try_again`, `play_again`, `keep_playing`, `leave_game`), `show_stats` |
| `pause_menu` | `resume`, `restart`, `leave_game` |
| `reset_policy` | What Try Again / Play Again restores (lives, health, timer, score, encounters, position, loadout, world state) |
| `hud`, `free_play`, `omitted[]` | HUD flags; no-ending games; systems deliberately left out, with reasons |

### Settings (`apps/orchestrator/ptv_orchestrator/settings.py`)

| Setting | Env var | Default |
|---------|---------|---------|
| `game_design_model` | `GAME_DESIGN_MODEL` | `gpt-5.6-sol` |
| `game_design_reasoning_effort` | `GAME_DESIGN_REASONING_EFFORT` | `high` |
| `game_design_verbosity` | `GAME_DESIGN_VERBOSITY` | `medium` |
| `game_design_agent_max_turns` | `GAME_DESIGN_AGENT_MAX_TURNS` | 20 (4–200) |
| `game_design_max_retries` | `GAME_DESIGN_MAX_RETRIES` | 1 (0–4) |
| `game_design_max_repair_rounds` | `GAME_DESIGN_MAX_REPAIR_ROUNDS` | 2 (0–6) |

### Quest notes

On Quest the design is what `quest_runtime_verify`'s second launch checks: the APK runs with `debug.ptv.verify_flow` so the player can die and the engine clicks its own end-screen buttons — health 0 costs a life, respawn, Game Over after the last life, Try Again / Play Again restart and reset, victory when the game is winnable, and Leave Game exits (`xrRequestExitSession` + `ANativeActivity_finish`) and the app relaunches. The "at most three buttons" instruction keeps panels pointable on the headset.

### SSE events

`running` → `warning` (designer unavailable, revision rounds) → `info` (repairs) → `succeeded` | `failed`.

## See also

- [`docs/game-flow.md`](../../../game-flow.md) — `builtin.game_flow`, `builtin.game_hud`, `ctx.game`
- [`docs/npc-and-ai.md`](../../../npc-and-ai.md) — attackers and encounters
- [`docs/quest-build.md`](../../../quest-build.md) — on-headset verification
