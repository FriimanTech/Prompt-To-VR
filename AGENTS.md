# AGENTS.md

Authoritative agent instructions for this repository. These rules apply to every
AI-driven change in this codebase, in any IDE, CLI, or CI.

## Mission

Take a detailed VR game prompt and produce a **playable, native C++ VR
executable** on Windows PC VR (Meta Quest 3 via Quest Link) through a local
orchestrator and a reusable Vulkan + OpenXR engine core.

## Hard constraints (never violate)

1. Runtime is a native C++ executable. **No Unity, Unreal, Godot, WebXR,
   Three.js, Babylon.js, or browser-rendered VR.**
2. Rendering is Vulkan. XR is OpenXR. Build system is CMake. Dependencies are
   managed with vcpkg manifest mode.
3. Local-first only. **No Supabase, S3, Temporal, cloud auth/storage,
   multiplayer, speech-to-text, marketplace publishing, Steam packaging** in
   this version. Quest standalone packaging IS supported: the `quest`
   target_device builds a local debug APK (Gradle + NDK) that is sideloaded to
   the headset over `adb` for local testing only. Meta Horizon **Store**
   submission / release signing remains out of scope.
4. Schema-first development. Once a schema exists in `packages/schemas`,
   downstream stages MUST consume the structured artifact, not the raw prompt.
5. Reuse built-in VR mechanics. The engine ships reusable, data-driven
   modules under `engine/runtime/src/mechanics/builtin/<mechanic>/`
   (teleport, head-relative and controller-relative slide, snap turn, grab,
   throw, recoil, hand_pose, and the weapon set: weapon, bow, melee_weapon,
   shield, item_source, npc.ranged_attack). Each is a real `IMechanic` written against the same public
   API generated code uses — `InputSnapshot`, `World` and `MechanicContext`
   (locomotion / physics / debug-draw / hand / transform-fx / haptics / socket
   / skeleton services, all engine-clamped) —
   so a `builtin+config_code_edit` task can copy one and edit it into a NEW
   generated mechanic. Generated code adds mechanics; it does not
   reimplement basic VR behavior, and it never modifies a built-in.
   `builtin.smooth_locomotion` has been REMOVED; use the explicit
   `builtin.locomotion.slide.*` ids. Walk / run is BUILT INTO both slides
   (hold the left thumbstick in to sprint; `InputSnapshot` exposes
   `thumbstick_down(hand)` / `thumbstick_clicked(hand)`, and
   `ILocomotion::sprinting()` is read-only for generated code), with walk
   and run speeds taken from the player avatar's measured clips. Never
   plan or generate a sprint or speed-modifier mechanic. Anything that
   follows the player (worn, attached or held) is ignored by the player's
   own collision sweep, so a worn collider cannot push the player.
   The engine INJECTS NO player
   mechanic: finger posing (`builtin.hand_pose`), the avatar body
   (`builtin.avatar_body`), each locomotion mode, snap / smooth turn and
   `builtin.movement_settings_menu` are planned in `mechanic_plan.json`
   and bound in `scene.json` from the BuildSpec `movement` block and
   `player_experience` — one mechanic per `role:` (see
   `PLAYER_ROLES` in `engine_catalog.py`), none for roles the game lacks.
   A scene with no locomotion is a valid stationary game. The player's
   hands are the engine's default rigged hands (Hands Only) or the
   avatar's own (Full Player Avatar); generated code must never
   re-implement finger curl, and must never couple a weapon to the hand
   (both read the same `input.hands[h].trigger` and stay in sync on
   their own). Walking moves go through `ctx.locomotion->move_horizontal`
   (horizontal-only, resolved by the engine's character controller);
   comfort vignettes go through `ctx.comfort`, never a drawn blink.
   NPC movement is likewise owned by `builtin.character_locomotion`:
   generated AI writes a desired velocity into `CLocomotionIntent` and the
   built-in moves the body, turns it, picks idle/walk/run from the achieved
   speed and scales clip playback so the stride matches the ground covered.
 Generated code must NEVER move a character by writing `CTransform` or
 play its locomotion clips — a second writer gets the foot-sliding wrong.
 Generated NPC AI implements `IBehaviour` with the CONTEXT-TAKING
 `on_attach` / `on_tick` (they receive `MechanicContext` + `InputSnapshot`);
 the engine hosts behaviours exactly like mechanics, and the context-free
 forms exist only for old projects (`codegen_contracts.py` rejects new AI
 written against them). An AI item whose NPCs a built-in fully covers
 (`follow_leader` -> `builtin.npc.follow`; chase / melee / react ->
 `builtin.npc.melee_chase`) is reconciled to `builtin+config`, never
 generated. The player is ONE entity, `ctx.hands->player_entity()` (the
 scene's `player_rig`, adopted by the engine); never look it up by name.
 Hits are observed with `ctx.combat->damage_seq` / `damage_events_since`,
 never through a private inbox.
 WEAPONS are the engine's too: every gun, beam, launcher, bow, sword,
 hammer and shield the interaction design describes runs on
 `builtin.weapon` / `bow` / `melee_weapon` / `shield` (NPCs that shoot on
 `builtin.npc.ranged_attack`), bound by `scene_compile`. Generated code
 never fires, keeps ammo, casts a ray from a muzzle, spawns a bullet or
 applies weapon damage; it reacts through `ctx.weapons` (`hits_since`,
 `state`, `held_weapon`), and abilities that launch something use
 `ctx.projectiles` / `ctx.vfx`. API contract v5 enforces it. See
 `docs/weapons.md`.
 INTERACTIONS are the engine's too: buttons `builtin.press_button`
 (index-fingertip poke / trigger; emits a named signal), in-world screens
 `builtin.world_display` (lines templated from `ctx.vars`), timed / score
 challenges `builtin.target_challenge` (spawns at world anchor sets),
 things that break `builtin.destructible`, grenades / flashbangs
 `builtin.explosive` (bound with grab + throw). Generated code wires them
 with `ctx.signals` / `ctx.vars` and places things at `ctx.anchors`; API
 contract v6 rejects hand-contact presses, private fuses and blasts,
 typed-in spawn coordinates when the world declares anchors, and private
 scoreboard text beside a world display. See `docs/interactions.md`.
 PHYSICS: every GLB gets a measured collider (AABB / hull / collision
 mesh); the player's body (`PLAYER_BODY`, radius from the avatar's torso,
 0.18 m Hands Only) never pushes props, while hand / forearm / shin
 colliders (`PLAYER_PART`) do and ignore the item an open hand reaches
 for; held items are `HELD` and never a floor. See `docs/physics.md`.
   Health, factions, damage, death, perception, navigation, attachment and
   runtime spawning are engine services (`ctx.combat`, `ctx.perception`,
   `ctx.nav`, `ctx.attach`, `ctx.spawn`, `ctx.contacts`), not per-game
   inventions: an enemy from one CodeTask has to be able to hurt a player
   set up by another. NPC waves are run by `builtin.encounter_director`
   and read through `ctx.encounters`. See `docs/npc-and-ai.md`.
   The GAME LOOP is the engine's too: `builtin.game_flow` runs
   `game_design.json` (player health reaching zero, lives, respawn under a
   fade, objectives, Game Over / victory / completion screens, pause menu,
   Try Again / Play Again via a fresh scene reload, Leave Game via
   `xrRequestExitSession` + `ANativeActivity_finish`), and `builtin.game_hud`
   shows health / lives / timer / objective on the left wrist. Generated code
   never keeps lives, respawns, restarts, opens a results screen or quits;
   it reads `ctx.game` (`gameplay_active()` gates weapons and timers) and
   reports custom objectives with `add_progress` / `complete` / `fail`.
   World-space text is `ctx.text` (SDF, bundled OFL font). Facing has one
   formula, `ptv/api/Facing.h` (`yaw_from_forward(f) = atan2(-f.x, -f.z)`).
   See `docs/game-flow.md`.
6. Generated C++ (`.cpp`, `.h`, `.hpp`) is confined to
   `engine/generated/{mechanics,ai,dialogue,world}/`. It MUST NOT modify the
   renderer, XR session loop, platform layer, memory core, or base ECS.
   Generated headers are allowed for same-task use and for including a
   shared header owned by a declared `depends_on` task (same project
   directory, built first — e.g. a weapon profile including
   `shooting_hub_shared.h`); both via bare-filename `#include "foo.h"`
   includes. Registration macros
   (`PTV_REGISTER_MECHANIC`, etc.) must only appear in `.cpp` files.
   Include violations cause the CodeTask to fail loudly (not silently
   strip).
   The VR **environment** is authored in C++ by a single world-architect
   agent under `engine/generated/world/` implementing `IWorldBuilder`; that
   surface has a tighter whitelist (only `ptv/api/IWorldBuilder.h` + std +
   glm) and is compiled + run OFFLINE in the `ptv_worldgen` host (no
   Vulkan/OpenXR/Jolt/renderer/ECS linked), emitting `scene.json`. The
 environment is NOT a Tripo GLB. The world builder also DECLARES the
 walkable space it creates via `nav_region` / `nav_link`; NPC routing
 reads that instead of reconstructing topology the author already knew,
 and a declaration that contradicts the geometry fails the stage. See
 `docs/navigation.md`. It declares NPC SPACE too -- `npc_spawn_zone`
 (where arrivals enter) and `npc_anchor` (where an NPC placed at load
 stands) -- but never spawns, names or counts NPCs: the NPC plan decides
 who exists, `scene_compile` places them and validates the placement
 against the merged world, and the runtime director spawns.
 It also owns static furniture (`world_builds_it`), places objects by
 their BASE (`align`, `asset_size`), and declares a `gameplay_anchor` /
 `anchor_set` for every gameplay placement request and the zone tags the
 NPC plan spawns groups in; `scene_compile` resolves gameplay entities onto
 those anchors and fails on sunk / floating / unsupported /
 interpenetrating items. See `docs/placement.md`.
7. Use Claude for the hardest gameplay planning and hardest C++ generation
   (`claude-opus-4-7`). Use `claude-sonnet-4-6` for lighter work. If Claude
   is unavailable (missing key or API error) the pipeline transparently
   falls back to OpenAI via the shared `LlmClient`
   (both tiers `gpt-5.6-sol`, separated by reasoning effort).
8. Tripo image-to-3D calls MUST enable textured, PBR output with image-aligned
   textures (`texture=True`, `pbr=True`, `texture_alignment="original_image"`)
   and MUST target the **V3** API; V2 is retired 2026-11-01. The same holds
   for `multiview-to-model` (humanoid characters).
9. Verify provider SDK signatures and model names with live docs before
   changing provider client code.
10. When code structure changes, update `/docs/` in the same change.

## Repository map

- `apps/web` – React + Vite + TypeScript control panel.
- `apps/orchestrator` – FastAPI backend that runs the pipeline.
- `engine/runtime` – **hand-authored** reusable C++ VR engine. Stable.
  Platform-split under `src/platform/{desktop,android}`; a `ptv::IAssetSource`
  seam feeds assets from the filesystem (PC VR) or the APK (Quest).
- `engine/android` – Gradle + NDK template the `quest_build` stage
  instantiates per project to package a standalone Quest APK. See
  `docs/quest-build.md`.
- `engine/generated/{mechanics,ai,dialogue,world}` – the **only** directories
  where LLM-produced C++ lives (`world/` = VR environment builders).
- `engine/worldgen` – offline `ptv_worldgen` host + `WorldContext` that runs
  generated `IWorldBuilder` code and emits `scene.json`. Links no engine core.
- `engine/tools` – scene compiler, GLB validator, bounds checker.
- `packages/schemas` – Pydantic v2 models + JSON Schemas (single source of
  truth for BuildSpec, AssetManifest, NpcPlan, MechanicPlan, SceneSpec,
  CodeTask, ProjectState, ArtifactMetadata, AcceptanceTestPlan).
- `packages/provider-clients` – Anthropic, Google GenAI, Tripo wrappers.
- `projects/<project_id>/` – per-project local artifact workspace.
- `docs/` – user-facing docs. Must stay current.
- `samples/starter-room/` – hand-authored sample scene that boots without any
  LLM calls, used to validate XR + Vulkan before spending API credits.

## Generation pipeline (order is fixed)

1. Prompt → **BuildSpec** (Claude Opus, structured output). `target_device`
   and `player_experience` (Hands Only / Full Player Avatar) are the
   user's choices and are forced to match `ProjectState`; the model
   decides the `movement` block (modes may be empty for stationary games).
   Optimization wording in the prompt never shapes the BuildSpec.
1a. Prompt + the New Project page's optional "Custom Quest Optimizations"
   box → **OptimizationPreferences** (`optimization_preferences`): a keyword
   gate (no optimization wording → no model call), one light-tier
   structured-output call through `LlmClient`, and a deterministic validator
   (each setting's quote must appear VERBATIM in its source, the box wins
   over the prompt, requests past a hard limit are clamped, eye tracking /
   no texture compression / disabling memory protection are reported
   unsupported even without the model). Only FFR, dynamic resolution, model
   detail, texture quality, draw distance, room culling, anti-aliasing and
   frame rate are user-controllable; memory safety never is. Each override
   becomes a LOCK in `performance.locks` the runtime governor never moves.
   Warnings never block. See `docs/quest-optimization.md`.
1b. Prompt + BuildSpec → **RequirementsLedger** (`requirements`, hard tier
   through `LlmClient`, structured output): every atomic thing the prompt
   asks for (`asset`, `visual`, `placement`, `layout`, `interaction`,
   `mechanic`, `rule`, `npc`), each with a VERBATIM quote (checked). Planning
   artifacts claim requirements with `satisfies` (asset items, mechanic-plan
   items, scene entities, `w.satisfies` in a world builder); after each
   owning stage a deterministic coverage gate gives ONE repair round, then
   fails with `RequirementGateError` listing what is unclaimed. Scene review
   and the headset record outcomes in `requirements_report.json` (shown on
   the project page). See `docs/requirements.md`.
2. BuildSpec → **AssetManifest** (Claude Sonnet, structured output). Only
   OBJECTS (characters, props, fx, ui) — the environment is not a GLB, and
   neither is a SOUND. Every item costs a reference image plus a paid Tripo
   call, so a `pistol_shot_audio` item buys a picture of a gunshot, a mesh of
   that picture, and a silent blob in the scene. Both prompts forbid audio
   entries and a deterministic reconcile drops any that survive, naming them
   — unless dropping would empty the manifest, which is the worse failure.
2a. Prompt + BuildSpec + AssetManifest → **InteractionDesign**
   (`interaction_design`, OpenAI Agents SDK agent, `gpt-5.6-sol` @ reasoning
   `high`, `WebSearchTool`): how every object is used -- `usage`, who wields
   it, its `hold` profile (one_hand / two_hand_line / two_hand_wrap /
   forearm_strap, aim-aligned or not), the `sockets` its mesh needs (each a
   `kind` + plain-words `part_description` for the vision stage), and for
   weapons the `weapon` (fire system hitscan / hybrid / projectile / beam,
   fire mode, rpm, spread, ammo, reload, recoil, visual), `melee`, `shield`
   or `bow` spec plus the `projectiles` fired (a mesh only when seen).
   Explicit (verbatim quote, checked) or inferred (rationale), explicit wins;
   a deterministic validate-and-repair (`interaction_rules.py`) and
   `fallback_design`. `reconcile_manifest` appends seen projectile assets at
   their real size and applies real-world dimensions. Scenery gets nothing.
   Feeds npc_plan, game_design, mechanic_plan (weapons map onto the weapon
   built-ins), socket_generation and scene_compile. See `docs/sockets.md`,
   `docs/weapons.md`. It also marks `pressable` items (a `press` spec +
   `press_surface` socket), `display` items (`screen` socket), a
   `destructible` spec on things that break and an `explosive` spec on
   thrown explosives; a held or thrown item is never under
   `vr_min_handheld_size_m` (12 cm). See `docs/interactions.md`.
2b. Prompt + BuildSpec + AssetManifest → **NpcPlan** (`npc_plan`, OpenAI
   Agents SDK agent, `gpt-5.6-sol` @ reasoning `high`, no tools). Decides per character: role,
   faction and who it fights, `movement_style`, behaviours, and which
   animation intents those behaviours actually use. Runs BEFORE rigging
   because "does this thing need animation at all" is the most expensive
   question in the pipeline (~30 credits per rig, ~20 per clip) and only
   the prompt can answer it — "the ducks should just slide" means
   `slide_no_animation` and no rig. The stage RECONCILES the manifest with
   its own decision (clearing `rigged`/`animations`), so there is one
   source of truth rather than two that drift. Also feeds `mechanic_plan`,
   `scene_spec` and codegen so all three describe the same game. A game
   with no characters emits an empty plan and makes no agent call.
   POPULATION: unique NPC types are models, decided by the BuildSpec's
   `npc_roster` (an unspecified plural such as "goblins" = 3 variants:
   regular, tall, big_bulky); instances are free runtime copies decided
   here — `persistent_count` (placed at load, e.g. a companion) and
   `encounters` (waves of `{asset_id, count}` groups, start trigger, caps).
   At runtime `builtin.encounter_director` clones one hidden template per
   type into world-declared `npc_spawn_zones`; `builtin.npc.follow` and
   `builtin.npc.melee_chase` carry per-type stats from the plan. Generated
   code never spawns or pools NPCs; HUDs read `ctx.encounters`.
   WHERE: a group the prompt places ("regular goblins at the front of the
   lane") gets `zone_tags` (+ verbatim `zone_quote`, `zone_visibility`);
   the director spawns it only in world zones with that tag, and the world
   is required to declare them. "They all respawn once all are dead" is
   `waves` with `repeat: true`.
2c. Prompt + BuildSpec + NpcPlan + AssetManifest → **GameDesign**
   (`game_design`, OpenAI Agents SDK agent, `gpt-5.6-sol` @ reasoning
   `high`, no tools): the rules of the loop -- experience kind, player
   health, lives, what happens on a lost life, objectives, victory / failure,
   end screens and their buttons, pause menu, reset policy, HUD -- each
   decision `explicit` (with the prompt's verbatim quote, checked) or
   `inferred` (with a rationale); explicit always wins and nothing the game
   does not need is invented (a book reader has no health). A deterministic
   validate-and-repair loop (`game_design_rules.py`) guarantees no dead
   ends: every ending has a screen, every screen a button, something offers
   Leave Game, attackers imply health, lives imply Game Over with Try Again.
   Feeds mechanic_plan, world_generation (objective spawn points), codegen
   and scene_compile, which binds one `ptv_game_director` entity
   (`builtin.ui_panel` + `builtin.game_flow` + `builtin.game_hud`) and fails
   if the loop cannot complete in that scene. See `docs/game-flow.md`.
3. AssetManifest + BuildSpec + NpcPlan → **MechanicPlan** (OpenAI Agents SDK agent,
   `gpt-5.6-sol` @ reasoning `high`, no tools). Each mechanic is classified `builtin`,
   `builtin+config`, `builtin+config_code_edit` (copy a built-in's source and
   edit it into a NEW generated mechanic — never replaces the built-in), or
   `generate`. The agent also assigns a controller `binding`
   (control + hand) to every input-driven mechanic; a deterministic
   validator normalizes face-button hands (sharing a control is allowed,
   not a conflict). BuildSpec labels each mechanic `regular` or
   `hub` (a shared foundation such as a spell-casting hub -- weapons are
   never hubs). A deterministic
   player-roles reconcile then adds / collapses / removes player
   mechanics so the plan matches the BuildSpec exactly, and the weapon
   reconcile (`weapon_plan.py`, also run at codegen start) makes every
   designed weapon `builtin+config` of `builtin.weapon` / `bow` /
   `melee_weapon` / `shield` (+ `item_source`, `npc.ranged_attack`),
   removing generated shooting hubs, fire profiles, ammo / reload / recoil
   tasks and `builtin.recoil`, named; designed explosives become
   `builtin.explosive` (generated fuse / detonation tasks removed). The
   interaction reconcile (`interaction_plan.py`, also at codegen start)
   maps timed target challenges onto `builtin.target_challenge`, in-world
   displays onto `builtin.world_display`, and adds `press_button` /
   `destructible` items from the design.
4. All above → **SceneSpec**, one per scene (OpenAI Agents SDK agent,
   `gpt-5.6-sol` @ reasoning `medium`, no tools) with a deterministic validate-and-repair loop
   (asset coverage, environment presence, valid mechanic-binding ids);
   problems are fed back into the same agent session up to
   `scene_spec_max_repair_rounds`. Scenes fan out across
   `scene_spec_max_parallel_workers` agents. Entities carry a `placement`
   intent (`on_surface`, `on_floor`, `hang_from`, `mount_on_wall`,
   `at_anchor` + anchor / anchor_set id) instead of coordinates for things
   that rest on world furniture; world-owned furniture is not placed here.
   See `docs/placement.md`.
5. Reference images for each object `AssetItem` → Gemini
   (`gemini-3.1-flash-image`). T-posed characters get a hand clause (open
   hands, five spread fingers, bare or full-finger gloves) and
   `image_config.image_size` 2K (4K for the player avatar); a Gemini vision
   hand check (`gemini-3.8-flash`, Interactions API) regenerates with its
   critique up to `hand_qa_max_retries` (2) times, then keeps the best
   image with a loud `HANDS:` warning (`images/<id>/hand_qa.json`).
6. Reference images → Tripo image-to-3D (textured, PBR). The generation
   `task_id` is recorded as `assets/<id>/tripo_task.json` so rigging can
   reference it instead of re-uploading the mesh. Humanoid characters go
   multiview (`tripo_multiview_humanoids`): `POST
   /v3/generation/image-to-multiview` (four views, hands checked) then
   `POST /v3/generation/multiview-to-model` with `inputs=[{"task_id"}]`;
   the finished mesh's hands are rendered close up (`ptv_glb_views
   --focus`) and checked; each check retries up to 2 times, then the best
   attempt is kept with a warning (`assets/<id>/hand_qa.json`).
6a. **Character routing** (`character_routing`): per character, Tripo or
   Uthana rigs and animates it, written to `character_routing.json`. The
   player avatar always goes to Uthana (finger joints). A biped humanoid Tripo's
   free rig-check cannot rig goes to Uthana; creatures go to Tripo; a humanoid
   both can rig is decided by an agent comparing the NPC's needs against both
   animation libraries (it sees the reference image). "Biped humanoid" = two
   legs + two arms, human or not. See `docs/uthana.md`.
6a'. **Uthana auto-rig** (`uthana_rigging`): uploads `model.glb` with
   `auto_rig`, `auto_rig_front_facing`, `include_fingers`,
   `auto_rig_strip_namespace`, `auto_rig_feet_on_floor`; downloads `model_uthana_rigged.glb`; records the
   `character_id` in `uthana_rig.json`; centres the origin (Uthana returns the
   FIRST character for a repeated upload, so its flags are never assumed).
   Confidence <= 0.20 falls back to Tripo with a loud warning. Runs before
   Tripo rigging so the fallback happens in the same run.
6b. **Auto-rigging** (`asset_rigging`): for every Tripo-routed `AssetItem` with
   `rigged: true`, call `POST /v3/animations/rig-check` (free) and, only if
   it answers `riggable: true`, `POST /v3/animations/rig` (~30 credits).
   Both endpoints take a single `input` field — a task_id, a `file_token`,
   or a URL — NOT V2's `original_model_task_id`. The rig `model` is derived
   from the returned `rig_type`: `v1.0-20240301` for `biped`,
   `v2.5-20260210` for every non-humanoid shape (`quadruped`, `hexapod`,
   `octopod`, `avian`, `serpentine`, `aquatic`). `spec="tripo"` and
   `out_format="glb"` always — Mixamo-spec skeletons make every follow-on
   `/animations/retarget` call fail with Tripo error 1004. Output lands as
   `model_rigged.glb` beside the
   original, and `asset_rig.json` records the outcome so a Resume never
   re-bills. Runs BEFORE normalization so the rigged mesh is what every
   later stage validates, sockets, decimates and compiles. Every rig type
   Tripo offers describes a WHOLE creature, so props, weapons and body
   parts are not riggable — the free pre-check is what keeps that from
   costing anything.
6c. **Animation retarget** (`asset_animation`): for every successfully rigged
   asset whose `AssetItem.animations` is non-empty, `POST
   /v3/animations/retarget` (~20 credits PER animation, ≤5 per call).
   `AssetItem.animations` holds semantic INTENTS (`idle`, `walk`, `run`,
   `attack`, `shoot`, `hurt`, `die`, `jump`, `turn`, `climb`, `dance`,
   `wave`, `sit`), never raw preset ids — which presets exist depends on the
   `rig_type` and rig `model`, neither known until rig-check has run, and
   `resolve_animations` maps intent→preset from what `asset_rig.json`
   recorded. A creature rig has only locomotion, so `walk` and `run` share
   one preset (billed once) and other intents are dropped loudly.
   `input` MUST be a rig `task_id`; there is no upload fallback.
   `animate_in_place=True` always — a clip with baked root motion would
   translate the mesh on top of the engine moving the entity, so the
   character covers double the ground and snaps back on every loop.
   `export_with_geometry=True` because the runtime reads mesh, skin and
   clips from one GLB. Output is `model_animated.glb`; `asset_animation.json`
   records intent → the clip name Tripo ACTUALLY produced (read back out of
   the GLB, never assumed). Routed Tripo bipeds may use any of the 101 v1.0
   presets (`resolve_tripo_motions`), not only the 13 intents.
6d. **Uthana motion plan** (`uthana_motion_plan`): NOTHING is generated. Code
   measures each rigged GLB's world height; an agent picks each gait's
   `style_id`; code enforces `strides` 1–5 (else `uthana_locomotion_strides`, default 5),
   `travel_angle` 0, speed
   `0.78 x h` (walk) / `2.2 x h` (run) and the canonical name
   `style-str{N}-ms{speed}-deg{angle}`, and adds the shared library idle
   (`uthana_idle_motion_id`, default `msf9nhQ4yyTJ`) to every character.
6e. **Uthana animation** (`uthana_animation`): per motion, via pluggable
   resolvers (`pipeline/uthana/motion_library.py`): locomotion = List your
   motions [:500] -> exact-name Motion Verification -> reuse, or Generate
   Locomotion then Rename; library = fixed id. Download is always
   `glb, no_mesh=True, in_place=True` with THIS character's `character_id`.
   Only when every motion is on disk, `glb_merge` combines them into
   `model_animated.glb` (clip names = motion names, one Hips offset per
   character so the origin sits at the character's middle) and writes the
   `asset_animation.json` entry with `provider: "uthana"` and clip speeds.
7. Assets **validated** (`asset_normalization`): GLB header/JSON check plus
   scale / collision metadata. This stage does NOT modify geometry.
7a. **Socket generation** (`socket_generation`): for every item the
   interaction design gives sockets, `ptv_glb_views` renders 10 views with a
   triangle-id buffer; Gemini (`socket_vision_model`, `gemini-3.8-flash`,
   Interactions API) segments each named part per view; multi-view voting
   maps masks to triangles; per-kind numpy solvers produce exact frames
   (hand sockets in the OpenXR grip frame, the rest +Z-facing; bore diameter
   at muzzles, cross-section for projectiles); the result is drawn on the
   mesh and verified by Gemini (one retry with the critique, then the
   geometry fallback is verified too; else `low`). Cached by GLB hash +
   spec + pipeline version; a required socket that cannot be produced fails
   the stage. Without a Google key the geometry fallback runs (`heuristic`).
   See `docs/sockets.md`.
7b. **Asset contract** (`asset_contract`): measures every character's
   **model frame** (`model_frame.py`: sole = skinned bind-pose bottom,
   height, forward = up x the shoulder line confirmed by the toes; Tripo
   rigs stand on their origin and face +X, Uthana's are hips-centred and
   face +Z) -- scene_compile emits it as `mesh.model_frame`,
   `foot_offset_m` = half the height and a HEIGHT-based `unit_scale`, the
   skinned loader folds the pivot into the root joints, and a skinned
   character without a frame fails scene_compile. With an interaction
   design it merges `socket_generation`'s sockets, holds, part regions and
   projectile cross-sections, and an asset the design gave no sockets gets
   none. Projects from before that stage instead derive each asset's named
   **sockets** (`muzzle`, `grip`) from its geometry with the deterministic
   `ptv_glb_sockets` tool (principal-axis PCA + mass asymmetry — no model
   calls). Either way it writes `asset_contract.json`. Positions are stored in UNIT
   space (normalised by the longest-axis extent) so they survive the LOD
   chain; the scene compiler multiplies by `target_scale_meters`, exactly as
   it already does for `bounds_radius_m`. Runs BEFORE codegen because the
   codegen agent needs the socket NAMES in context — it writes
   `ctx.sockets->socket_world(self, "muzzle")` and must never hard-code an
   offset it cannot know. Which sockets to derive comes from
   `AssetItem.socket_hints`, plus an implicit `muzzle` + `grip` fallback for
   **`prop`** assets only: a socket is a point on a mesh, and characters
   attach via rig joints and the `BodySlots` taxonomy, UI attaches to a body
   slot, and FX spawn AT another asset's socket. Explicit hints override the
   category either way, and an asset with no sockets records a `reason`
   rather than a bare `{}`. Every artifact path resolves against
   `Settings.repo_root`, never `Path.cwd()` — resolving against the process
   directory silently emptied every contract when the server was started from
   `apps/web`, and the run continued to codegen as if that were normal. The
   stage now fails loudly when a GLB `asset_normalization.json` vouched for
   is unreadable, or when an asset carrying explicit hints cannot derive
   them.
8. **World generation** (runs BEFORE code generation, so gameplay code can
   use the ids the world actually declared): a world-architect agent per
   scene (OpenAI Agents SDK, `gpt-5.6-sol` @ reasoning `high`,
   `WebSearchTool`) writes `IWorldBuilder` C++ in
   `engine/generated/world/<project_id>/`, compiled + run offline in
   `ptv_worldgen` under a compile-and-repair loop → `world.scene.json`
   (`world.<scene_id>.scene.json` per scene; a deterministic fallback
   world replaces only an environment whose budget is exhausted). The
   builder also declares the walkable space via `nav_region` / `nav_link`,
   which a deterministic validator checks against itself (links naming real
   regions, crossing points inside the rooms they join, spawn inside a
   region, every region reachable) before the world is accepted. It places
   world-owned furniture by its BASE, declares a `gameplay_anchor` /
   `anchor_set` for every gameplay placement request, the zone tags the NPC
   plan spawns groups in, and claims its layout requirements
   (`w.satisfies`); a contract check sends missing or unsupported anchors
   back into the repair loop. See `docs/placement.md`.
8b. CodeTasks from MechanicPlan → OpenAI Agents SDK (`gpt-5.6-sol` @ `high`)
   → C++ files (`.cpp` + optional `.h`/`.hpp` headers) in
   `engine/generated/{mechanics,ai,dialogue}/`. After all tasks are
   written, a **compile-verify + repair loop** builds the
   `ptv_starter_room` executable (plus an NDK clang syntax pass for Quest,
   `codegen_quest_syntax_check`) and feeds any compiler errors back to the
   owning task's agent to regenerate (up to `codegen_max_repair_rounds`);
   the stage fails loudly if the generated C++ still does not compile
   (`codegen_verify_compile`, default on).
8c. **Asset optimization** (`asset_optimization`): derives a device
   `PerformanceProfile` (tier + triangle/draw-call budgets) from
   `target_device` and how much GLB content the project has, allocates a
   per-asset triangle budget weighted by importance, and runs
   `ptv_glb_optimize` (meshoptimizer) to emit an LOD chain
   `<asset_id>_lod0..3.glb`. Raw Tripo output is ~500k triangles per asset;
   Meta's whole-frame budget for Quest 3 is 1.3M-1.8M, so this stage is what
   makes any non-trivial project runnable. There is **no cap on asset count** —
   more content just means a smaller per-asset budget (never below a quality
   floor) plus more aggressive runtime culling/LOD. See `docs/rendering.md`.
   Animated characters get a SEPARATE budget (`max_animated_agents`,
   `animation_lod_distances_m`, `ai_tick_rates_hz`): skinning and AI are CPU
   work that competes with the render thread, so a scene can sit inside its
   triangle budget and still miss frame rate on skeletons alone.
   **Rigged assets** take a skin-preserving path: animation clips are copied
   through verbatim (they target nodes, so decimation cannot invalidate
   them — but the writer has to actually emit them, and for a while it did
   not, which silently left every LOD un-animatable),
   bindings ride inside the
   interleaved vertex through the remap, simplification uses
   `meshopt_simplifyWithAttributes` + `meshopt_SimplifyRegularize` (never bone
   data as an attribute), `--sloppy` is refused because it invents vertex
   positions that carry no bindings, and every LOD emits a byte-identical
   skeleton so one joint palette serves all four levels.
   **Textures** survive decimation (per-material submeshes, per-material
   weld, UV-weighted, seam-protected simplification), are written once per
   content hash to `assets/ptv_textures/` and compressed by
   `ptv_texture_compress` (vcpkg `ktx`) to KTX2 ASTC 6x6 (4x4 UI) with linear
   mips, sized by the closest distance the player can view them from inside
   `texture_memory_budget_mb`; a source texture lost in a LOD, or a render of
   the LOD that loses colour against the source, fails the asset. Each LOD
   records its simplification error (`lod_errors_rel`) for projected-error
   LOD at runtime. The stage then applies the user's locks
   (`apply_overrides`) and a **memory fit** against the engine's 4.5 GiB
   steady-state / 5.0 GiB peak targets (Meta's limit is 5.75 GiB PSS):
   when even the always-resident set would not fit, least-visible-first
   corrections run (far textures, background LOD0s, a user's render scale
   high, near textures last), never removing content; still over → the APK
   is built and flagged `at_risk`.
9. Deterministic scene compiler merges `world.scene.json` (environment) with
   gameplay entities and emits `scene.json` + build metadata, including each
   mesh's `lod_paths`/`bounds_radius_m`/`sockets`/`skinned`/`animations` and
   the scene-level `performance` block. `mesh.animations` is the clip
   contract: semantic intent → real glTF clip name, so generated C++ writes
   `ctx.skeleton->play_clip(self, "walk", true)` without knowing what the
   retargeter called it, exactly as `socket_world(self, "muzzle")` works. It is also the **socket validator**: it is the only
   stage that sees both a CodeTask's `required_sockets` and the asset's
   derived sockets, so a mechanic requiring a socket its asset cannot supply
   fails the build loudly, with every violation listed at once.
   `required_sockets` means sockets read on `self`, so the hard failure
   applies when the bound entity HAS a mesh; a mesh-less bound entity (a
   shooting hub on the player's hand, resolving the weapon at runtime and
   guarding with `has_socket`) only needs SOME asset in the scene to provide
   the socket, and warns instead. It also emits and validates
   **`equip_slots`**: a slot is a place on a WEARER (a hip holster) as
   opposed to a socket, which is a point on an ITEM. `SceneSpec` declares
   them, `scene_compile` writes them into `scene.json`, and `SceneLoader`
   emplaces `CEquipSlots` — the component existed for a long time with
   nothing ever populating it, so every authored holster silently accepted
   nothing. A `builtin.equip` binding naming a slot nothing declares, a
   duplicate slot name on two placed entities (spawn templates are
   exempt: every variant and clone of an NPC type shares its hand slot), or
   a mesh naming an asset absent from the manifest all fail the stage. With an interaction design it also binds the
   **weapons** (`weapon_scene.py`): each designed weapon's built-in with the
   design's numbers and socket names, hidden projectile / arrow templates, a
   `builtin.item_source` quiver, `builtin.npc.ranged_attack` for shooting
   NPCs with their weapon seated in `hand_r` by its grip socket
   (`attached_to.socket`), the scene-level `projectiles` table, stripping
   `builtin.recoil` and stale generated weapon bindings; it sizes each
   projectile to the measured muzzle bore and each arrow to the bow's draw
   (`projectile_scale.py`, explicit sizes kept), and fails on a gun without
   a muzzle, a bow without limb tips, a melee weapon without a strike
   point, or a seen projectile without a mesh. See `docs/weapons.md`.
   It fails when a placed mesh references a texture file that does not
   exist (`performance.textures` reports the rest). Then
   **`scene_complexity`** samples visibility from the walkable regions
   (p50 / p95 visible triangles and draws), estimates frame cost, detects
   rooms closed on all four sides, marks `static_batch` entities (nothing
   moves, holds, binds or reads them) and chooses each scene's starting
   settings for knobs the user did not lock: FFR level (1, or 2 when heavy),
   multiview (only above 120 p95 draws -- it measured ~10% more GPU at low
   draw counts), room culling, the batch cell size. The **memory model**
   (`memory_model.py`, exact from the built files where possible, calibrated
   by measured runs in `projects/memory_calibration.json`) writes
   `performance.memory_estimate` and `resident_memory_budget_mb`, plus
   `scene_complexity.json` and a provisional `memory_status.json`
   (`estimated_safe_not_device_tested` / `at_risk`, never "verified").
   It also writes every unskinned asset's measured collider
   (`mesh.collision`), the player's `body_radius_m`, binds the interaction
   built-ins (`interaction_scene.py`), and resolves gameplay entities onto
   the world's anchors (`placement.resolve` → `settle` → `violations`,
   failing with `PlacementContractError`; `placement_report.<scene>.json`).
9b. **Scene review** (`scene_review`, report-only): `ptv_scene_views` (the
   `ptv_glb_views` CPU rasteriser over the whole `scene.json`, with an
   entity-id buffer) renders the spawn view, an overview with the roof cut
   away and an eye-level + above view per requirement; deterministic checks
   (claimed asset missing, requirement entities nobody can see, spawn facing
   a wall), then a Gemini vision check per placement / layout / visual
   requirement. Outcomes go to the ledger and `scene_review.json`; the
   viewpoints are written into the shipped scene as `verify_viewpoints`.
10. Build (branches on `ProjectState.target_device`, the create-time UI toggle
   that is the single source of truth and is mirrored into
   `BuildSpec.target_device`; every planning prompt names the target too, so a
   Quest project is planned against a mobile GPU budget):
    - `pcvr`: CMake configure + build (Visual Studio 18 2026, toolset v145) → `.exe`.
    - `quest`: `quest_build` instantiates `engine/android`, packages
      `scene.json` + GLBs + KTX2 textures + `.spv` into the APK assets
      (stored uncompressed so the engine memory-maps them), and runs
      `gradlew assembleDebug` (NDK) → standalone APK. See `docs/quest-build.md`.
      The APK is always downloadable (`GET /projects/{id}/apk`) whatever its
      verification or memory status.
10b. **On-headset verification** (`quest_runtime_verify`, Quest tail ONLY,
 never PC VR; a failed check fails the stage but the APK stays
 downloadable): installs and launches the APK on the connected
 headset in verification mode (`debug.ptv.verify`: scripted player walk,
 player cannot die), reads the engine's NPC telemetry and checks only what
 this game's NPC plan asks for -- base checks for every game (including one
 with no NPCs), floor / no-drift checks for stationary NPCs, band checks for
 followers, spawn and close-and-hit checks for enemies; drawn feet within
 5 cm of the floor and facing within 20 degrees of travel for moving NPCs.
 With a `game_design.json` it runs a second launch (`debug.ptv.verify_flow`:
 the player can die and the game loop clicks its own end-screen buttons)
 and checks the loop the design promises: health 0 costs a life, respawn,
 Game Over after the last life, Try Again / Play Again restart and reset,
 victory when winnable, Leave Game exits and the app relaunches. With
 player weapons it runs a third launch (`debug.ptv.verify_weapons`: a
 scripted hand holds and fires every weapon at a hostile) and checks each
 was held, fired, shot from within 2 cm of its muzzle, hit, damaged through
 `ctx.combat`, kept the projectile pool inside its cap and held the frame
 budget. With textured meshes it runs a fourth launch
 (`debug.ptv.verify_textures`: the engine shows each textured asset in turn)
 and compares a Quest screenshot of each with an offline render of the same
 LOD (visible, no magenta, colour kept, no white-out). It checks the user's
 locks held in every `perf:` line (a frame miss the locks caused is a
 warning), and runs the **memory pass**: `dumpsys meminfo` / `gpumeminfo` /
 the engine's `mem:` line, a lowmemorykiller scan after every launch, PSS
 growth across restarts, a soak (`quest_memory_soak_seconds`) and a
 MemoryGuard pressure launch, against the 5.0 GiB peak and 4.5 GiB steady
 targets. `memory_status` is `verified_safe` only from those measurements;
 a memory failure re-runs asset_optimization → scene_compile → quest_build
 → verify with the measured calibration, at most
 `quest_memory_max_correction_rounds` times. With interactive things or
 review viewpoints it runs a fifth launch (`debug.ptv.verify_interactions`:
 the engine's `InteractionVerifier` drives the player and right hand -- its
 pose replaces the tracked pose at the source): every loose prop settles
 (< 3 cm), walking into a grabbable resting above the legs moves it < 1 cm,
 an open hand reaching for it moves it < 3 cm and it is then held, every
 button presses (fingertip poke, else trigger), a pressed button's
 challenge starts, spawns and runs its clock, every explosive detonates,
 and a Quest screenshot is taken at each review viewpoint; zone-tagged NPC
 types must first appear inside their zones. Results also go into the
 requirements ledger / `requirements_report.json`. Writes
 `runtime_verification.json`. SKIPPED, never failed, when no headset is
 connected (memory then reads `estimated_safe_not_device_tested` or
 `at_risk`). See `docs/quest-build.md`, `docs/quest-optimization.md`.
11. Launch (branches on target_device): `pcvr` runs the `.exe` through OpenXR
 (Quest Link); `quest` does `adb install -r` + `am start` the NativeActivity.

## Verified model / SDK choices (September 2026)

- Claude: `claude-opus-4-7` (hard), `claude-sonnet-4-6` (light). Structured
  outputs via `output_config.format = {type: "json_schema", schema: ...}`.
  Strict tool use via `strict: true`. No beta headers required.
- OpenAI: **every** OpenAI call in the pipeline runs `gpt-5.6-sol`
  (~1.05M context, 128K max output). What varies per stage is
  `reasoning.effort`, which 5.6 accepts as
  `none | low | medium (default) | high | xhigh | max`. `minimal` was valid
  on gpt-5 and is GONE in 5.6, so `Settings` rejects it at startup rather
  than letting it 400 mid-run.
  - ChatGPT fallback (Chat Completions, no tools, via `LlmClient`): both
    tiers are `gpt-5.6-sol` and are told apart by effort — `high` for the
    Opus-equivalent path (build_spec), `medium` for the Sonnet-equivalent
    one (asset_manifest, the call that decides which assets spend Tripo
    credits on rigging and animation). Structured outputs via JSON mode
    (`response_format={"type": "json_object"}`) + schema embedded in the
    system prompt; caller re-validates with the same Pydantic model.
  - Agents SDK stages (Responses API): `npc_plan` and `mechanic_plan` at
    `high`, `scene_spec` at `medium`, `world_generation` at `high`,
    `code_generation` at `high`. Codegen's effort **must** stay explicit:
    `Agent.__post_init__` back-fills `model_settings` from the model NAME
    when none is passed, and it infers `none` for `gpt-5.6-sol`, which
    would silently strip reasoning from the hardest C++ in the pipeline.
  - The tool-carrying stages (`world_generation`, `code_generation`) must
    stay on the Responses API: on 5.6, reasoning combined with function
    tools is Responses-only — Chat Completions allows tools at effort
    `none` only.
- Google GenAI: Python package `google-genai`. Reference images use
  `gemini-3.1-flash-image` with `generate_content` and
  `response_modalities=["IMAGE"]` (Imagen 4 endpoints discontinued
  2026-08-17); `ImageConfig(aspect_ratio, image_size)` takes `image_size`
  `"1K"` (default) / `"2K"` / `"4K"` (google-genai 2.18.1). Socket
  segmentation and verification, the hand checks and the scene review use
  `gemini-3.8-flash` through the Interactions API
  (`client.interactions.create(model, input, response_format,
  generation_config)`); it rejects `thinking_level: "minimal"` with a 400,
  so `GeminiVisionClient` falls back to `"low"` and remembers it.
- Agents SDK effort for the newer stages: `interaction_design` at `high`
  (with `WebSearchTool`, Responses API).
- Tripo: **V3 API** (`https://openapi.tripo3d.ai/v3`) called directly over
  `httpx` — NOT the `tripo3d` SDK, whose base URL is still
  `api.tripo3d.ai/v2/openapi` (V2 retires 2026-11-01). Flow:
  `POST /v3/files` → `POST /v3/generation/image-to-model` →
  `GET /v3/tasks/{id}` → download `output.model_url`. `model` is REQUIRED
  (no server default); allowed values are `v3.1-20260211`, `v3.0-20250812`,
  `v2.5-20250123`, `P1-20260311`. Multiview (verified 2026-11-10):
  `POST /v3/generation/image-to-multiview` (`input` = file token; output
  `generate_multiview_image.{front,left,back,right}_view_url`) →
  `POST /v3/generation/multiview-to-model` with `inputs=[{"task_id": ...}]`
  (single element; source must be a successful `image_to_multiview`).
- Vulkan SDK 1.4.341.x on Windows. Requires VS 2022 or 2026 toolchain.
- OpenXR Khronos loader via vcpkg. On Windows + Quest Link, Oculus OpenXR
  runtime is registered automatically.
- CMake generator for Visual Studio Community 2026: `"Visual Studio 18 2026"`
  with toolset `v145`. Requires CMake >= 4.2.

## Coding rules

- C++20. Headers under `engine/runtime/include/ptv/…`. Implementation under
  `engine/runtime/src/…`. Namespace `ptv`.
- Do not include ImGui from release builds of gameplay modules.
- Do not introduce new top-level dependencies without editing `engine/vcpkg.json`
  and the relevant doc.
- Python 3.11+. `ruff` + `black`-compatible formatting.
- All schema changes update pydantic model, exported JSON schema file under
  `packages/schemas/json/`, AND any C++ consumer in `engine/runtime/src/scene/`.

## When in doubt

Prefer reuse over regeneration. Prefer determinism over agent improvisation.
Prefer a smaller working slice over a bigger broken one.
