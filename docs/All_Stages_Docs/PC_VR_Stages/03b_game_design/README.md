# Stage 3b: Game Design

> **Pipeline position:** after `npc_plan` → before `mechanic_plan`  
> **Target:** shared (PC VR and Quest)  
> **Primary code:** [`apps/orchestrator/ptv_orchestrator/pipeline/stage_game_design.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/stage_game_design.py)  
> **Primary artifacts:** `projects/<project_id>/game_design.json`

## Main stage summary

### What this stage is

The **game design stage** decides the rules of the game loop: whether the
player has health, how many lives, what happens on death (where they
respawn, with what, how protected), what they are trying to do, how the game
is won or lost, which end screens exist and what every button on them does,
what Try Again resets, and what the HUD shows.

### Why it exists

Without it every generated game invented its own run flow, and one of them
reached zero health with no defined answer: the arena froze, no screen
appeared and there was no way to restart. The engine now runs the loop
(`builtin.game_flow`) from this document, so the loop is designed once and
complete.

### Inputs (what enters)

| Input | Location |
|-------|----------|
| User prompt (verbatim) — the source of explicit rules | `projects/<id>/prompt.txt` |
| BuildSpec | `build_spec.json` |
| NpcPlan — who hurts the player, which waves exist | `npc_plan.json` |
| AssetManifest — real item ids for `collect` / `interact` | `asset_manifest.json` |

### Outputs (what it produces)

| Output | Location |
|--------|----------|
| GameDesign | `projects/<id>/game_design.json` |

### Where output goes next

- **`mechanic_plan`** — loop items become `builtin.game_flow` / `builtin.game_hud`
- **`world_generation`** — spawn points that `reach_location` objectives need
- **`code_generation`** — `ctx.game` contract, loop-ownership lint
- **`scene_compile`** — the game director entity, plus a static check that the loop can complete
- **`quest_runtime_verify`** — the game-loop pass on the headset

## Detailed stage deep dive

- Agent: OpenAI Agents SDK, `gpt-5.6-sol` at reasoning `high`, no tools
  (`pipeline/game_design_agent.py`, prompt `GAME_DESIGN_SYSTEM`).
- Every decision carries `provenance`: `explicit` with the prompt's exact
  words, or `inferred` with a rationale. Explicit wins; BuildSpec text is
  never "explicit".
- Validate-and-repair (`pipeline/game_design_rules.py`): problems go back to
  the agent up to `game_design_max_repair_rounds` times, then are repaired
  deterministically (completing what the design chose, never inventing a
  system). Unverifiable quotes are downgraded to inferred. A design that is
  still incomplete fails the stage.
- No OpenAI key / agent failure: a conservative design derived from the NPC
  plan (`fallback_design`).

See also: [`docs/game-flow.md`](../../../game-flow.md).
