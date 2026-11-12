# Interactions: buttons, screens, challenges, things that break, explosives

Five engine built-ins run the interactive objects a game describes, wired to
each other and to generated code through two services, `ctx.signals` and
`ctx.vars`. Like weapons ([weapons.md](weapons.md)), they are planned from
the interaction design, bound by `scene_compile`, never generated.

The shooting range is why: its red button waited for
`ctx.contacts->began(button, hand)` (the hands had no colliders, so it never
pressed), its 20-second challenge HUD was folded into the wrist HUD and
deleted, its lane-1 targets had hit points nothing acted on, and its grenade
hub ran a fuse and never exploded.

## Signals and vars

* `ctx.signals` (`ISignals`): `emit(name)`, `count(name)`,
  `fired_since(name, seen)` -- named events with counts. Start `seen` at
  `count(name)` in `on_attach`.
* `ctx.vars` (`IVars`): `set_number` / `set_text`, `number` / `text`, `has`,
  `seq` -- a transient board of live values (not saved).

Both are reset on a scene transition (`core/GameBoards.cpp`).

## The built-ins

| built-in | what | publishes |
| --- | --- | --- |
| `builtin.press_button` | a button / switch: an index-fingertip poke through its cap (Meta's poke model: the tip crosses the cap plane inward inside a >= 22 mm footprint, 7 mm travel, re-arms after leaving; the tip is the drawn finger, or the controller's own poke point -- grip pose + (0, -1, -9.5) cm -- when the drawn finger is more than 8 cm from it, as a full-body avatar's lagging arm can be), or trigger / grip with an empty hand within reach; the cap dips, haptics tick, a hover highlight shows | signal `<signal>` (default `<entity>.pressed`), var `<signal>.count` |
| `builtin.world_display` | a screen IN THE WORLD: SDF text on the display's `screen` socket (or its collider's face toward the player), lines templated from vars: `"TIME {lane2.time_left:0.0}"` | -- |
| `builtin.target_challenge` | a timed / score challenge: waits for `start_signal`, counts down, spawns targets from hidden templates at a world anchor set (`hang_from`, `on_surface`, `mount_on_wall`), counts destroyed targets, ends after `duration_s`, clears, restarts on the next signal | vars `<id>.state` (ready / countdown / running / finished), `.time_left`, `.score`, `.best`; signals `<id>.started`, `<id>.finished` |
| `builtin.destructible` | something shot or blown up that BREAKS: any `ctx.combat` damage (bullets, beams, melee, blasts) lowers its health; at zero it breaks with an effect (`shatter`, `explode`, `knock_down`, `vanish`), hides, loses its collider, sets its interactive `destroyed` state, and comes back after `respawn_after_s` (0 = never) | signals `<entity>.destroyed`, `destructible.destroyed`, `<entity>.restored`; var `destructibles.destroyed` |
| `builtin.explosive` | a grenade / flashbang / smoke / impact explosive: trigger while held pulls the pin, the fuse starts when it LEAVES the hand (`builtin.throw` gives it the hand's velocity); frag = radial damage through `ctx.combat`, full inside `inner_radius_m`, falling to 0 at `radius_m`, blocked by level geometry, impulse on loose props, fireball + light + scorch; flash = a white-out for the player scaled by distance and gaze (`IApp::request_flash`) and `CStun` on NPCs in sight; smoke = a cloud; impact = detonates on the first hit. A scene-placed one restocks where it started after `respawn_after_s` (4 s) | signals `<entity>.detonated`, `explosive.detonated` |

`CStun` stops `builtin.npc.melee_chase` and `builtin.npc.ranged_attack`
(stand still, no attacks) while it lasts. A world `breakable` interactive
uses the state key `destroyed`; scene_compile renames `broken` / `shattered`
/ `smashed`, and a breakable whose id or tags say `target` / `destructible`
also gets `builtin.destructible` (with health) and `breaks_by_hand: false`.

## Pipeline

1. **`interaction_design`**: `ItemUsage.pressable` with a `press` spec
   (`press_with`, `travel_m`, `signal`) and a `press_surface` socket;
   `ItemUsage.display` with a `screen` socket; `destructible`
   (`max_health`, `effect`, `respawn_after_s`); `explosive` (kind,
   activation, fuse, radii, damage, impulse, flash radius, stun, hurts the
   player) on thrown items. The repair (`interaction_rules._repair_interactive`)
   fills specs and sockets; a held or thrown item is never smaller than
   `vr_min_handheld_size_m` (12 cm) in VR (the real size is kept in
   metadata).
2. **`mechanic_plan`**: `interaction_plan.reconcile_interaction_items` maps
   generated timed challenges that spawn targets onto
   `builtin.target_challenge` (`duration_s` parsed from "20 seconds"), a
   `builtin.game_hud` item naming a display onto `builtin.world_display`
   (the wrist HUD is only `game_hud`), and adds `press_button`,
   `world_display` and `destructible` items from the design;
   `weapon_plan.reconcile_weapon_items` adds `builtin.explosive` per
   explosive and removes generated grenade / fuse / detonation /
   "activation and throwing" tasks, telling dependants to read
   `<entity>.detonated`. `code_generation` re-runs both on resume.
3. **`scene_compile`** (`interaction_scene.apply_interactions`): binds
   press_button (static aabb collider), world_display (never
   `static_batch`), destructible (health, tags), explosive (+ grab + throw,
   dynamic, 0.4 kg); for each challenge a host entity
   `ptv_challenge_<id>` when none is bound, templates made kinematic, its
   anchor set from the templates' placement, `start_signal` = the single
   button's signal, and default display lines when a display exists:
   `{id.state}`, `TIME {id.time_left:0.0}`, `HITS {id.score}   BEST {id.best}`.

## Generated code

Wires these together and reacts to them -- `ctx.signals->fired_since(...)`,
`ctx.vars->number(...)`, `ctx.anchors` for places -- and never re-implements
them. API contract v6 (`codegen_contracts.py`) rejects a button pressed by
`ctx.contacts`, a private fuse with blast damage, private scoreboard text
when the game has a world display, and spawns at typed-in coordinates when
the world declares anchors.

## Tests

`ptv_smoke`: `press_button`, `target_challenge`, `destructible`, `explosive`.
Orchestrator: `test_interactions.py`, `test_contract_v6.py`. On the headset
the interactions pass pokes every button, starts every challenge and throws
every explosive ([quest-build.md](quest-build.md)).
