> **Shared content:** Identical to [PC_VR_Stages/03b_game_design](../../PC_VR_Stages/03b_game_design/README.md). Update both copies when this stage changes.

# Stage 3b: Game Design

> **Pipeline position:** after `npc_plan` → before `mechanic_plan`  
> **Target:** shared (PC VR and Quest)  
> **Primary code:** [`apps/orchestrator/ptv_orchestrator/pipeline/stage_game_design.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/stage_game_design.py)  
> **Primary artifacts:** `projects/<project_id>/game_design.json`

## Main stage summary

The rules of the game loop, decided once from the prompt: health, lives,
respawn, objectives, win/lose, end screens and their buttons, resets and
the HUD. On Quest the result is also what `quest_runtime_verify`'s
game-loop pass checks: it launches the APK with `debug.ptv.verify_flow=1`,
lets the engine walk death → Game Over → Try Again → victory → Play Again →
Leave Game, and confirms the app exits and launches again.

Inputs: the verbatim prompt, BuildSpec, NpcPlan and AssetManifest. Output:
`game_design.json`, consumed by `mechanic_plan`, `world_generation`,
`code_generation`, `scene_compile` and `quest_runtime_verify`.

See the PC copy for the deep dive and [`docs/game-flow.md`](../../../game-flow.md).
