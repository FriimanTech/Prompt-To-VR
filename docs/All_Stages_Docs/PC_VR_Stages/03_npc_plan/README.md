# Stage 3: NPC Plan

> **Pipeline position:** after `interaction_design` → before `game_design`  
> **Target:** shared (PC VR and Quest)  
> **Primary code:** [`apps/orchestrator/ptv_orchestrator/pipeline/stage_npc_plan.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/stage_npc_plan.py)  
> **Primary artifacts:** `projects/<project_id>/npc_plan.json` (may also rewrite `asset_manifest.json`)

## Main stage summary

### What this stage is

The **NPC plan stage** reads the prompt and decides what every non-player
character type is supposed to *do*: its role (enemy, ally, neutral, critter,
boss), faction and who it fights, movement style, behaviours, which animation
intents those behaviours actually use, locomotion / perception / combat
numbers, and the **population** — how many are placed at load
(`persistent_count`) and which arrive in **encounter waves**. The result is
`npc_plan.json`.

### Why it exists

Placing a goblin GLB in a scene does not make an enemy. Before this stage
every downstream stage re-derived NPC intent from the same prose and they
disagreed — the manifest would rig a duck that the codegen agent then wrote
as a static slider, and the rig was billed for nothing.

It runs **before rigging and animation** because "does this thing need
animation at all?" is the most expensive question in the pipeline (~30 Tripo
credits per rig, ~20 per clip) and only the prompt can answer it ("the ducks
should just slide" means `slide_no_animation` and no rig). It runs **before
`game_design`, `mechanic_plan` and `scene_spec`** because all of them need to
know who can hurt the player, which factions exist and which waves come.

### Inputs (what enters)

| Input | Location | Used for |
|-------|----------|----------|
| User prompt (verbatim, first 20,000 chars) | `projects/<id>/prompt.txt` | Wording a summary would lose ("just slide") |
| BuildSpec | `build_spec.json` | `npc_roster` (families, variants, singular vs plural) |
| AssetManifest | `asset_manifest.json` | Candidate characters (`category: character`, `character_role != player_avatar`) |
| InteractionDesign (optional) | `interaction_design.json` | Excerpt of weapons with `npc` wielders, so a `ranged_attack` NPC names a weapon that actually fires |

### Outputs (what it produces)

| Output | Location |
|--------|----------|
| NpcPlan | `projects/<id>/npc_plan.json` |
| Reconciled manifest (only when it changed) | `projects/<id>/asset_manifest.json` — `rigged` / `animations` rewritten to match the plan |
| Agent log | `projects/<id>/logs/npc_plan.log` |

### Where output goes next

- **`game_design`** — hostile attackers imply player health; waves enable `clear_waves` objectives.
- **`mechanic_plan`** — `reconcile_npc_mechanics` adds `builtin.encounter_director`, `builtin.npc.follow`, `builtin.npc.melee_chase`; `reconcile_npc_ai_items` turns generated AI a built-in fully covers into `builtin+config`; `reconcile_weapon_items` adds `builtin.npc.ranged_attack` for `ranged_attack` NPCs.
- **`scene_spec`** — placement context (role, faction, health, speeds, spawn counts).
- **`character_routing` / `uthana_motion_plan`** — which characters need a rig and which motions.
- **`asset_rigging` / `asset_animation`** — see the reconciled `rigged` / `animations` flags.
- **`code_generation`**, **`world_generation`**, **`scene_compile`** (per-type stats into the NPC built-ins, encounter plan into the director) and **`quest_runtime_verify`** (which NPC checks to run).

### Models / providers

- OpenAI Agents SDK agent ("NPC director"), model `gpt-5.6-sol`, reasoning effort `high`, verbosity `medium`, **no tools**.
- Output type `NpcPlan`, non-strict schema (`make_capturing_schema(..., strict=False)`); Pydantic validates and the raw output is kept for diagnosis.
- No Tripo / Gemini calls. Cost is one agent run, plus at most one schema-retry and one population-revision run.

### Failure behaviour

This stage **never fails the run** on agent problems:

| Case | Result |
|------|--------|
| No candidate characters (none, or only the player avatar) | Empty `NpcPlan`, **no agent call**, succeeds with "only the player's own avatar…" or "no character assets…" |
| Agent error / unusable output after retries | Empty `NpcPlan`, `warning` event, logged; stage succeeds (pipeline behaves as before this stage existed) |
| Entry names an asset id not among the candidates | Dropped with a `warning` |
| Population rule violations | One revision request to the agent, then deterministic repair |

---

## Detailed stage deep dive

### Files and functions

| File | Role |
|------|------|
| `pipeline/stage_npc_plan.py` | `run(ctx)` → `run_with_director(ctx, director=None)` (tests inject a stub `NpcDirector`); `_is_actor_candidate`, `_interaction_lines`, `_read_prompt`, `_drop_unknown_assets`, `_reconcile_manifest` |
| `pipeline/npc_plan_agent.py` | `NpcDirector` (lazy SDK import, retry loop), `NpcPlanAgentInput`, `build_director_message` |
| `pipeline/npc_population.py` | `npc_plan_violations`, `repair_npc_plan`, `default_encounters`, `DEFAULT_ANIMATED_BUDGET = 12`; also the mechanic-plan reconciles `reconcile_npc_mechanics` / `reconcile_npc_ai_items` |
| `pipeline/interaction_rules.py` | `design_excerpt` (the weapons excerpt) |
| `pipeline/prompts.py` | `NPC_PLAN_SYSTEM` (not device-templated) |
| `packages/schemas/ptv_schemas/npc_plan.py` | `NpcPlan`, `NpcSpec`, `EncounterPlan`, enums |

### Execution trace

1. Emit `running`, `record_stage(running)`; load BuildSpec and AssetManifest.
2. **Candidates** = manifest items with `category == character` and `character_role != player_avatar`. A role of `none` still counts, so an unset upstream role never silently removes an enemy.
3. No candidates → save an empty plan and succeed (no API call).
4. Bridge `OPENAI_API_KEY` from settings into `os.environ` (this is the first Agents-SDK stage of a run, so nothing else has done it yet).
5. Build `NpcDirector` with `NPC_PLAN_SYSTEM`, `session_id=ptv-npc-plan-<project_id>`, and the settings below.
6. Message (`build_director_message`): process instructions (find actors → decide movement style first → role/faction/behaviours/intents → numbers within budget), extra instructions (candidate asset ids, "at most one NpcSpec per asset id", the interaction-design weapons excerpt), then the verbatim prompt, BuildSpec JSON and AssetManifest JSON.
7. `director.run()`: up to `npc_plan_max_retries + 1` attempts. A schema failure (`NpcPlanOutputError`) is retried with the field-level issues listed; other agent errors re-send the brief.
8. `_drop_unknown_assets` removes entries for ids that are not candidates.
9. `npc_plan_violations` → if any, **one** revision run with the problems listed (a failed revision is only logged).
10. `repair_npc_plan` applies deterministic fixes and emits each as `info`.
11. `save_npc_plan` writes `npc_plan.json`.
12. `_reconcile_manifest` rewrites the manifest if needed and saves it.
13. Succeed with "N NPC type(s), A animated, C clip(s) to retarget[; W wave(s) of T type(s)]".

### Population validator (`npc_plan_violations`)

- Every BuildSpec `npc_roster` variant has an `NpcSpec`.
- Waves name only NPC types that exist, and never a singular (companion) roster entry or an `ally`.
- NPCs with `spawn.waves = true` (enemy / boss / critter / neutral) require an `encounters` plan.

### Deterministic repair (`repair_npc_plan`)

- Drops wave groups naming unknown, ally or singular NPCs (an empty wave list clears `encounters`).
- Singular roster characters and allies that nothing spawns get `persistent_count = 1` (otherwise they would never appear).
- Wave enemies with no encounter plan get `default_encounters`: variants ordered by physique (`small_fast` → … → `boss`); one type = three growing waves, several types = one wave introducing each type plus a finale of the strongest; `start: delay`, `start_delay_s: 5`, `max_concurrent` ≤ 8.
- The encounter alive cap is lowered to fit `12 − sum(persistent_count)` (the animated-agent budget).

### Manifest reconcile (`_reconcile_manifest`)

The plan is authoritative over the manifest:

- `slide_no_animation`, or no animation intents → `rigged = false`, `animations = []` (emits "skipping rig and animation").
- Otherwise → `rigged = true`, `animations = animation_intents` (in order).

Assets the plan does not mention are left untouched.

### Key schema fields (`NpcSpec`)

| Field | Notes |
|-------|-------|
| `asset_id`, `role`, `family`, `variant` | Keyed to a manifest item and a roster variant |
| `persistent_count` | Placed at load and stay (companion, guards); 0 for wave arrivals |
| `faction`, `hostile_to` | Same faction never fights |
| `movement_style` | `animated_locomotion` (default), `slide_no_animation`, `stationary`, `flying` |
| `behaviors` | `idle`, `patrol`, `chase`, `flee`, `ranged_attack`, `melee_attack`, `contact_damage`, `take_cover`, `reposition`, `follow_leader`, `guard_point`, `react_to_damage` |
| `animation_intents` (≤5) | Billed per clip; must be empty for `slide_no_animation` |
| `animation_fallback` | `nearest_preset` (default), `procedural_cpp`, `none` |
| `locomotion` | walk 1.4 / run 4.0 m/s, turn 540°/s, `leash_radius_m`, `hover_height_m` |
| `perception`, `combat`, `spawn` | Sight 25 m / FOV 120° / hearing 12 m; health 100, damage 10, range 2 m, cooldown 1.5 s, `projectile_asset_id`; `initial_count`, `max_concurrent`, `waves` |

`NpcPlan` also carries `faction_notes` and optional `encounters` (`mode` waves/continuous/fixed, `start` on_load/delay/player_grabs_tag/signal, caps, intervals, `min_player_distance_m`, 1–50 waves of `{asset_id, count}` groups). At runtime `builtin.encounter_director` runs it; generated code never spawns NPCs.

### Settings (`apps/orchestrator/ptv_orchestrator/settings.py`)

Env vars are the upper-cased field names (read from the repo `.env`).

| Setting | Env var | Default |
|---------|---------|---------|
| `npc_plan_model` | `NPC_PLAN_MODEL` | `gpt-5.6-sol` |
| `npc_plan_reasoning_effort` | `NPC_PLAN_REASONING_EFFORT` | `high` |
| `npc_plan_verbosity` | `NPC_PLAN_VERBOSITY` | `medium` |
| `npc_plan_agent_max_turns` | `NPC_PLAN_AGENT_MAX_TURNS` | 30 (4–200) |
| `npc_plan_max_retries` | `NPC_PLAN_MAX_RETRIES` | 1 (0–4) |

### Quest notes

The 12-character animated budget (`DEFAULT_ANIMATED_BUDGET`) is sized for what a Quest comfortably skins and ticks; it is applied on both targets. The scene compiler and `asset_optimization` enforce their own runtime caps on top. On Quest, `quest_runtime_verify` picks its NPC checks (floor / no-drift, follower band, spawn and close-and-hit) from this plan.

### SSE events

`running` → `warning` (agent unavailable, dropped ids, plan needs revising) → `info` (repairs, manifest reconcile) → `succeeded`.

## See also

- [`docs/npc-and-ai.md`](../../../npc-and-ai.md) — runtime NPC systems, encounter director
- [`docs/uthana.md`](../../../uthana.md) — how rigged characters are animated
- [`docs/weapons.md`](../../../weapons.md) — NPC ranged attacks
- [`docs/architecture.md`](../../../architecture.md)
