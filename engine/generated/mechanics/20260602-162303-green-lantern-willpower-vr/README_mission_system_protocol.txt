Mission System Tag Protocol (MVP)

Inputs this mechanic consumes (push as CTag strings on the Mission System entity):
- mission.select:{"id":"<mission_id>","difficulty":"easy|normal|hard"}
- mission.confirm:{}
- portal.activated:{}
- objective.progress:{"id":"<objective_id>","delta":1,"state":"update|complete|fail"}

Additionally, you can drive via binding params on the same entity:
{
  "select_mission_id":"zero_g_flight_gauntlet",
  "difficulty":"normal",
  "confirm":true,
  "portal_activated":true,
  "objective_progress":{"id":"gate_3","delta":1,"state":"update"}
}

Outputs this mechanic publishes:
- ui.page.register:{"id":"mission_log","title":"Missions","order":5}
- ui.toast:{...}                      // HUD/Wrist menu displays
- portal.travel.request:{"mission_id":"..","difficulty":"..","scenes":["scene_a",...]}
- ai.difficulty.profile:{"tier":"normal","scale":1.0}
- mission.complete:{"id":"..","difficulty":"..","medal":"gold|silver|bronze|"}
- save.section.push:{"section":"mission","data":{...}}
- save.request:{"reason":"mission_complete","immediate":true}

Medals:
- medal_thresholds in mechanic params can be time (lower better) or score (higher better). Time thresholds should fill bronze/silver/gold with seconds. For score mode, use numbers.

Notes:
- Objective chains are linear in MVP. Branches/retries can be emulated by posting objective.progress for different ids.
- Difficulty scaling is advisory via ai.difficulty.profile tag; AI packs may consume this to scale spawns.
