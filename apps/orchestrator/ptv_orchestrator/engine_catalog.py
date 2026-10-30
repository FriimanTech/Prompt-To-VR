"""Engine catalog: the single source of truth for what the VR runtime
can already do, plus a registry of mechanics promoted from past games.

The mechanic-planner agent is shown this catalog at runtime so it can
prefer reuse over generation. Any time a generated mechanic compiles
cleanly, ``stage_engine_promotion`` appends an entry to
``engine/promoted/registry.json``; on the next pipeline run that entry
is exposed to the planner as a ``promoted.*`` reusable id, so the
engine "grows" with each game built on it.

Design notes
------------

* Built-in entries below mirror the actual ``PTV_REGISTER_MECHANIC`` calls
  in ``engine/runtime/src/mechanics/builtin/<mechanic>/``. Every built-in is
  a real ``IMechanic`` module there -- nothing is implemented inline by
  ``Application.cpp`` any more. When you add a new built-in module, add it
  here too.
* The locomotion ids use the ``builtin.locomotion.*`` namespace introduced
  alongside Meta's terminology (``slide.head_relative`` /
  ``slide.controller_relative`` / ``teleport``). The legacy short ids
  ``builtin.teleport`` and ``builtin.snap_turn`` are kept as aliases so
  existing scenes keep working. ``builtin.smooth_locomotion`` has been
  REMOVED -- use the explicit ``slide.*`` id.
* Reading the promoted registry never raises: a missing or malformed
  file just yields an empty list. Promotion is best-effort enrichment,
  not a load-bearing dependency.
"""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable

# ---------------------------------------------------------------------------
# Static built-in catalog
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class CatalogEntry:
    """A single mechanic the planner is allowed to pick by id."""

    id: str
    summary: str
    category: str
    params: dict[str, str] = field(default_factory=dict)
    inputs: tuple[str, ...] = ()
    comfort_notes: str = ""
    aliases: tuple[str, ...] = ()
    # The player job this mechanic fills (see PLAYER_ROLES), or "". At most
    # one bound mechanic may fill each role in a scene.
    role: str = ""

    def to_json(self) -> dict[str, object]:
        out: dict[str, object] = {
            "id": self.id,
            "summary": self.summary,
            "category": self.category,
            "params": dict(self.params),
            "inputs": list(self.inputs),
            "comfort_notes": self.comfort_notes,
            "aliases": list(self.aliases),
        }
        if self.role:
            out["role"] = self.role
        return out


_BUILTINS: tuple[CatalogEntry, ...] = (
    # ----- Locomotion (canonical Meta-aligned ids) -----
    CatalogEntry(
        id="builtin.locomotion.teleport",
        summary=(
            "Discrete teleport locomotion. Push the left stick FORWARD to "
            "raise a parabolic arc from the controller; tilt the controller "
            "up to reach farther and down to land closer; release the stick "
            "to land there (feet at the target, wherever the player stands "
            "in their playspace). No fade and no blink -- teleport is "
            "already the comfort option."
        ),
        category="locomotion",
        params={
            # ``max_distance_m`` / ``max_horizontal_m`` are accepted as
            # synonyms -- older scenes use those spellings.
            "max_range_m": "float, default 12.0 (farthest landing)",
            "min_range_m": "float, default 1.0 (closest landing)",
            "hand": "'left' (default) or 'right'",
        },
        inputs=("left thumbstick pushed forward", "controller pitch"),
        comfort_notes="Discrete; safe for VR-novice users. Never add a vignette.",
        aliases=("builtin.teleport",),
        role="role:teleport",
    ),
    CatalogEntry(
        id="builtin.locomotion.slide.head_relative",
        summary=(
            "Sliding (continuous) locomotion. Movement direction is the "
            "HMD's forward vector projected onto the floor; turning the "
            "head re-steers smoothly. Meta term: 'sliding, head-relative'."
        ),
        category="locomotion",
        params={
            "max_speed_mps": (
                "float, default: the player avatar's walk-clip speed "
                "(else 1.5). Walking pace. Leave unset unless the prompt "
                "names a speed."
            ),
            "accel_tau_s": "float, default 0.12",
            "decel_tau_s": "float, default 0.18",
            "sprint": (
                "object, BUILT IN and on by default: {enabled: bool, "
                "speed_mps: float (default: the avatar's run-clip speed, "
                "else 3.0)}. Hold the left stick IN while pushing forward "
                "to run; the player picks Hold / Toggle / Off in the "
                "settings menu. The avatar's run clip plays only while "
                "sprinting. Never generate a sprint/run mechanic."
            ),
            "vignette": (
                "object, default off: {enabled: bool, user_toggle: bool, "
                "strength 0..1, inner_fov_deg, feather_deg}. Only when the "
                "prompt asks for comfort options."
            ),
        },
        inputs=(
            "left thumbstick (X=strafe, -Y=forward)",
            "left thumbstick click (hold to sprint)",
        ),
        comfort_notes=(
            "Comfort ramp + circular dead zone. The player stays on the "
            "ground and follows terrain. Sprint ramps in and out and "
            "narrows the vignette while running. Optional Meta-style "
            "tunnel-vision vignette only when requested."
        ),
        role="role:slide_head",
    ),
    CatalogEntry(
        id="builtin.locomotion.slide.controller_relative",
        summary=(
            "Sliding (continuous) locomotion. Movement direction is the "
            "left controller's pointing vector projected onto the floor; "
            "head yaw is irrelevant. Meta term: 'sliding, controller-relative'."
        ),
        category="locomotion",
        params={
            "max_speed_mps": (
                "float, default: the avatar's walk-clip speed (else 1.5)"
            ),
            "accel_tau_s": "float, default 0.12",
            "decel_tau_s": "float, default 0.18",
            "sprint": (
                "object, built in and on by default (same shape as "
                "head-relative slide): hold the left stick in to run"
            ),
            "vignette": (
                "object, default off (same shape as head-relative slide)"
            ),
        },
        inputs=(
            "left thumbstick (X=strafe, -Y=forward)",
            "left thumbstick click (hold to sprint)",
        ),
        comfort_notes=(
            "Same ramps as head-relative; lets the user move sideways "
            "while looking elsewhere. Common in shooters / combat games."
        ),
        role="role:slide_controller",
    ),
    CatalogEntry(
        id="builtin.locomotion.snap_turn",
        summary=(
            "Right-stick snap rotation: one turn per flick, about the "
            "player's head. Never blinks or darkens the screen."
        ),
        category="locomotion",
        params={
            "degrees": "float, default 45.0",
            "cooldown_s": "float, default 0.30 (minimum gap between flicks)",
            "activation_threshold": "float, default 0.70 (stick push that turns)",
            "recenter_threshold": (
                "float, default 0.30 (stick must return inside this before the "
                "next turn)"
            ),
            "allowed_degrees": (
                "list of floats, e.g. [15, 30, 45]; the angle in force snaps to "
                "the nearest one"
            ),
            "degrees_key": (
                "string; a persisted number (ctx.persistence) that overrides "
                "degrees every frame, so a settings panel can change the angle"
            ),
        },
        inputs=("right thumbstick X",),
        comfort_notes=(
            "Discrete rotation; already the comfort option, so never add a "
            "vignette or blink to it."
        ),
        aliases=("builtin.snap_turn",),
        role="role:snap_turn",
    ),
    CatalogEntry(
        id="builtin.locomotion.smooth_turn",
        summary=(
            "Right-stick continuous rotation, eased in. Bind alongside snap "
            "turn when the prompt lets the player choose; the active turn "
            "mode decides which one reacts."
        ),
        category="locomotion",
        params={
            "degrees_per_second": "float, default 90",
            "ease_tau_s": "float, default 0.08",
            "vignette": (
                "object, default off: {enabled, user_toggle, strength, "
                "inner_fov_deg, feather_deg}. Only when the prompt asks for "
                "comfort options."
            ),
        },
        inputs=("right thumbstick X",),
        comfort_notes=(
            "Continuous rotation is the strongest vection cue; offer a "
            "vignette only when the prompt asks for comfort settings."
        ),
        role="role:smooth_turn",
    ),
    CatalogEntry(
        id="builtin.movement_settings_menu",
        summary=(
            "The player's movement and comfort settings panel, opened with a "
            "controller button. It lists ONLY what the game offers: "
            "locomotion modes when two or more are bound, snap vs smooth "
            "turning when both are bound, and vignette switches (only while "
            "a slide mode or smooth turn is active, and only when a vignette "
            "was offered as a user option). A game with nothing to choose "
            "never shows it. Use this instead of generating a locomotion "
            "menu."
        ),
        category="locomotion",
        params={
            "open_button": "'x' (default) / 'y' / 'a' / 'b'",
            "open_on_start": "bool, default false",
            "distance_m": "float, default 1.1",
        },
        inputs=("the open button", "either trigger to pick"),
        role="role:movement_settings_menu",
    ),
    # ----- Object interaction -----
    CatalogEntry(
        id="builtin.grab",
        summary=(
            "Hand-attach an object on grip squeeze; release drops it. "
            "Binding this mechanic to an entity is what MAKES it grabbable "
            "(it tags the entity), so no separate 'grabbable' tag is needed. "
            "When the asset has a derived 'grip' socket the object snaps so "
            "that socket sits in the hand, which is what stops a weapon from "
            "being held sideways just because the player reached for it from "
            "above."
        ),
        category="interaction",
        params={
            "radius_m": "float, default 0.18 (reach around the hand)",
            "snap_to_grip": (
                "bool, default true. Align the object's 'grip' socket to the "
                "hand. Set false for something with no single correct "
                "orientation (a ball, a rock, a crate)."
            ),
        },
        inputs=("either grip",),
    ),
    CatalogEntry(
        id="builtin.throw",
        summary=(
            "Turns releasing a grabbed object into a throw, scaling the "
            "smoothed hand velocity. Bind it to the object alongside "
            "builtin.grab; an object with grab but no throw simply drops."
        ),
        category="interaction",
        params={"impulse": "float, default 4.0 (throw strength)"},
    ),
    # ----- Feedback / juice -----
    CatalogEntry(
        id="builtin.recoil",
        summary=(
            "Kicks an object backward and pitches its nose up when the bound "
            "control is pressed, then springs it home. Purely visual (it "
            "drives the engine's draw-time transform-offset layer), so it "
            "never fights physics or builtin.grab. Bind it to a weapon "
            "ALONGSIDE the mechanic that actually fires: both read the same "
            "trigger, so they stay in sync with no coupling. This is the "
            "cheapest 'the weapon fired' cue available -- no sockets, no "
            "moving parts and no skinning required."
        ),
        category="interaction",
        params={
            "kick_back_m": "float, default 0.035 (shove along the object's +Z)",
            "kick_up_deg": "float, default 3.0 (muzzle rise)",
            "recover_tau_s": "float, default 0.07 (spring-back time constant)",
            "require_held": (
                "bool, default true (only kick while a hand holds the object)"
            ),
        },
        inputs=("the bound control on whichever hand holds the object",),
    ),
    # ----- Weapons (interaction_design.json decides which, with what tuning) --
    CatalogEntry(
        id="builtin.weapon",
        summary=(
            "A ranged weapon that SHOOTS: pistols, rifles, shotguns, blasters, "
            "launchers, beam guns. Fires from the item's 'muzzle' socket when "
            "the player holds it and pulls that hand's trigger (or an NPC "
            "wielding it pulls ctx.weapons->trigger). Delivery: hitscan "
            "(instant ray), hybrid (instant ray hit + a visible tracer / bolt "
            "flying to it -- the default for real firearms), projectile "
            "(ctx.projectiles: travel time, gravity, splash -- rockets, "
            "grenades, slow bolts) or beam (continuous ray while held). "
            "Ammo + reload (B / Y face button, auto, overheat, infinite), fire "
            "modes (semi, burst, auto, charge, hold_beam), spread / pellets, "
            "damage falloff, recoil, haptics, muzzle flash, impacts and decals "
            "are all built in; damage goes through ctx.combat. Its rays ignore "
            "the weapon, the hands and the player. The scene compiler binds "
            "it from each WeaponSpec; plan NO generated fire / ammo / reload / "
            "projectile / recoil mechanic for a weapon it covers."
        ),
        category="weapon",
        params={
            "delivery": "'hitscan' | 'hybrid' | 'projectile' | 'beam'",
            "fire_mode": "'semi' | 'burst' | 'auto' | 'charge' | 'hold_beam'",
            "rpm": "float", "damage": "float", "range_m": "float",
            "spread_deg": "float", "pellets": "int",
            "ammo": "{magazine, reserve, infinite, reload, reload_time_s, heat_per_shot}",
            "projectile": "{speed_mps, gravity_scale, radius_m, on_hit, splash_radius_m, "
                          "splash_damage, visual_template}",
            "visual": "'tracer' | 'energy_bolt' | 'mesh' | 'none'",
            "beam": "{damage_per_s, width_m, max_length_m, color_rgb}",
            "recoil": "{kick_back_m, kick_up_deg, recover_s}",
        },
        inputs=("trigger of the hand holding it", "B / Y to reload"),
    ),
    CatalogEntry(
        id="builtin.bow",
        summary=(
            "A bow: hold it by its grip, bring an arrow (or an empty hand) to "
            "the string and squeeze to nock, pull back to draw (haptics "
            "tighten), let go to loose the arrow through ctx.projectiles at a "
            "speed set by the draw; it flies with gravity and sticks where it "
            "lands. The string is drawn procedurally between the limb tips. "
            "Pair with builtin.item_source on the arrow template for a "
            "quiver over the shoulder."
        ),
        category="weapon",
        params={"max_draw_m": "float, default 0.6", "max_speed_mps": "float, default 70",
                "damage": "float, default 60", "arrow_template": "entity id",
                "allow_empty_draw": "bool, default true"},
        inputs=("grip of the non-bow hand to nock / draw / release",),
    ),
    CatalogEntry(
        id="builtin.melee_weapon",
        summary=(
            "Swords, axes, hammers, clubs, bats: swing to hit. The strike part "
            "is the segment between its strike_base / strike_tip sockets (or a "
            "sphere at strike_face for blunt weapons), swept between frames so "
            "fast swings never pass through a target. Damage scales with swing "
            "speed above a threshold, once per target per cooldown, through "
            "ctx.combat, with impacts and haptics."
        ),
        category="weapon",
        params={"damage": "float", "min_speed_mps": "float, default 1.5",
                "speed_scale": "float", "cooldown_s": "float", "blunt": "bool"},
        inputs=("swinging the hand that holds it",),
    ),
    CatalogEntry(
        id="builtin.shield",
        summary=(
            "A held shield blocks: projectiles and rays already stop on its "
            "body, and this registers it with ctx.combat so melee hits whose "
            "path from the attacker passes through it are blocked (with an "
            "optional leak fraction), with block haptics and sparks. Held by "
            "builtin.grab's forearm-strap hold."
        ),
        category="weapon",
        params={"blocks_melee": "bool, default true", "leak": "float 0..1, default 0"},
    ),
    CatalogEntry(
        id="builtin.item_source",
        summary=(
            "Reach to a body slot (back / shoulder for a quiver, hip for "
            "magazines, chest for grenades) or a world spot and squeeze an "
            "empty hand: a copy of the item is spawned into that hand. Bound "
            "to the item's hidden template."
        ),
        category="interaction",
        params={"body_slot": "'back' | 'shoulder_r' | 'shoulder_l' | 'hip_r' | 'hip_l' | "
                             "'waist' | 'chest'",
                "radius_m": "float, default 0.25", "count": "int, 0 = unlimited"},
        inputs=("grip of an empty hand near the slot",),
    ),
    CatalogEntry(
        id="builtin.npc.ranged_attack",
        summary=(
            "An NPC that shoots with the weapon attached to it: finds the "
            "nearest visible hostile, keeps its firing distance (closing in / "
            "backing off) through CLocomotionIntent, faces the target, and "
            "fires builtin.weapon in bursts via ctx.weapons (trigger + aim "
            "point). Executes the NpcPlan's ranged_attack behaviour."
        ),
        category="npc",
        params={"preferred_range_m": "float, default 8", "min_range_m": "float, default 4",
                "sight_m": "float, default 25", "burst": "int, default 3",
                "burst_pause_s": "float, default 1.2", "weapon": "entity id (optional)"},
    ),
    CatalogEntry(
        id="builtin.hand_pose",
        summary=(
            "Curls the PLAYER'S OWN fingers from the controller: index "
            "follows the trigger, middle/ring/pinky follow the grip, and the "
            "thumb lowers toward the index when it rests on the thumbstick. "
            "It curls whichever hands the player actually sees -- the "
            "default rigged hands (Hands Only) or the avatar's own finger "
            "bones (Full Player Avatar) -- about axes measured from the rig. "
            "EVERY game plans exactly one of these (role:fingers); the scene "
            "compiler binds it on the player rig. Never generate a mechanic "
            "that curls the player's fingers, and never couple a weapon to "
            "the hand (both read the same trigger value)."
        ),
        category="interaction",
        params={
            "smoothing_tau_s": (
                "float, default 0.045 (finger curl smoothing time constant)"
            ),
            "hold_curl": (
                "float, default 0.85 (how closed the fingers sit while the "
                "hand is holding something)"
            ),
            "thumb_sources": (
                "list, default ['thumbstick']. Add 'thumbrest' and/or "
                "'face_buttons' only when the prompt asks for that."
            ),
            "relaxed_curl": "float, default 0.08 (gentle bend when idle)",
        },
        inputs=("both triggers", "both grips", "thumbstick/trigger touch"),
        role="role:fingers",
    ),
    CatalogEntry(
        id="builtin.character_locomotion",
        summary=(
            "Moves an NPC and keeps its animation in agreement. Bind it to "
            "every character that moves; AI code then only writes a desired "
            "velocity into CLocomotionIntent and this does the rest: sweeps "
            "the body so it cannot walk through walls, snaps it to the "
            "floor, turns it to face travel, picks idle/walk/run from the "
            "speed it ACHIEVED, and scales clip playback so the stride "
            "matches the ground covered (which is what stops the feet "
            "skating). Set 'animated': false and the same mechanic slides an "
            "un-animated model instead -- that is the correct answer when a "
            "prompt explicitly asks for no character animation, and it needs "
            "no separate mechanic. NEVER generate a mechanic that plays "
            "locomotion clips or moves a character by writing CTransform; "
            "it will fight this one and get the foot-sliding wrong."
        ),
        category="npc",
        params={
            "walk_speed_mps": "float, default 1.4",
            "run_speed_mps": "float, default 4.0",
            "turn_rate_deg_s": "float, default 540",
            "animated": (
                "bool, default true. false = move the model with no clips "
                "(the 'ducks just slide' case)"
            ),
            "ground_snap": "bool, default true",
            "reference_walk_mps": (
                "float, default 0 = derive from the character's height. Only "
                "set this when a clip's authored stride is unusual."
            ),
            "reference_run_mps": "float, default 0 = derive from height",
        },
        inputs=("CLocomotionIntent written by AI code",),
        comfort_notes=(
            "Not player-facing. Animated characters are the most expensive "
            "thing on a Quest 3, so respect the NpcPlan's max_concurrent."
        ),
    ),
    CatalogEntry(
        id="builtin.encounter_director",
        summary=(
            "The scene's NPC population: clones the hidden template of each "
            "NPC type into live actors, in WAVES (or continuously, or a fixed "
            "set), starting on a trigger (on load, after a delay, when the "
            "player grabs a tagged item, or on a signal from game code), "
            "with a cap on how many are alive at once, spawn points away from "
            "the player and out of their sight, defeat counting and a "
            "countdown between waves. Publishes everything through "
            "ctx.encounters (phase, wave_index, wave_count, alive, "
            "remaining_in_wave, defeated_total, countdown_s, start/pause/"
            "reset) for a wave HUD or mission flow. The scene compiler binds "
            "it automatically from the NpcPlan's encounters -- NEVER plan or "
            "generate a wave spawner, an NPC pool or a spawn loop."
        ),
        category="npc",
        params={
            "mode": "'waves' (default) | 'continuous' (last wave repeats) | 'fixed'",
            "start": "{kind: on_load | delay | player_grabs_tag | signal, delay_s, tag}",
            "max_concurrent": "int, default 6 (also capped by the animated-agent budget)",
            "spawn_interval_s": "float, default 1.5",
            "inter_wave_delay_s": "float, default 6.0",
            "min_player_distance_m": "float, default 10",
            "waves": "[{groups: [{template: <entity id>, count}]}] -- compiled from the NpcPlan",
            "zones": "[{id, position, radius, tags}] -- compiled from the world's npc_spawn_zones",
        },
        inputs=("NpcPlan.encounters (compiled)", "player grab state"),
        comfort_notes=(
            "Spawns outside the player's sight and at least "
            "min_player_distance_m away, so nothing pops in at arm's length."
        ),
    ),
    CatalogEntry(
        id="builtin.npc.follow",
        summary=(
            "A companion that follows its leader (the player by default): "
            "keeps a trailing band, routes around walls with the navigation "
            "service, walks and runs to keep up, and -- when lost behind "
            "geometry or left far behind by a teleport -- is re-placed behind "
            "the leader, out of their view. Never fights. Bind together with "
            "builtin.character_locomotion. Use it for 'an ally that follows "
            "me around'; generate code only for behaviour it cannot express."
        ),
        category="npc",
        params={
            "leader_tag": "string, default 'player'",
            "min_distance_m": "float, default 1.8 (stops when this close)",
            "max_distance_m": "float, default 2.8 (starts moving when farther)",
            "run_distance_m": "float, default 6.0 (runs when farther)",
            "walk_speed_mps": "float, default 1.4",
            "run_speed_mps": "float, default 3.5",
            "catchup_distance_m": "float, default 18 (re-place when farther)",
            "stuck_catchup_s": "float, default 4 (re-place when stuck this long)",
            "catch_up": "bool, default true",
        },
        inputs=("leader position", "navigation"),
    ),
    CatalogEntry(
        id="builtin.npc.melee_chase",
        summary=(
            "An enemy that chases its target (nearest hostile, or a tag such "
            "as 'player'), routes around walls, telegraphs a melee attack "
            "with a wind-up, lands the hit only if still in reach with a "
            "clear line, respects a cooldown, and staggers briefly when hurt. "
            "Plays the 'attack' and 'hurt' intents; death is the engine's. "
            "Every number is a param, so the variants of one NPC family "
            "(standard / tall-fast / big-bulky) are this one mechanic with "
            "different settings. Bind together with "
            "builtin.character_locomotion."
        ),
        category="npc",
        params={
            "target_tag": "string, default '' = nearest hostile by faction",
            "sight_range_m": "float, default 30",
            "chase_speed_mps": "float, default 3.5",
            "walk_speed_mps": "float, default 1.4 (used when close)",
            "sprint": "bool, default true (run clip while chasing)",
            "attack_range_m": "float, default 1.6",
            "windup_s": "float, default 0.5 (telegraph before the hit)",
            "attack_cooldown_s": "float, default 1.5",
            "damage": "float, default 10",
            "hit_reach_m": "float, default attack_range_m + 0.6",
            "stagger_s": "float, default 0.35",
            "attack_clip": "intent, default 'attack'",
            "hurt_clip": "intent, default 'hurt'",
            "gait_variant_when_chasing": "string, optional (e.g. 'punching')",
        },
        inputs=("perception", "navigation", "combat"),
    ),
    # ----- The player's own body -----
    CatalogEntry(
        id="builtin.avatar_body",
        summary=(
            "The player's own body, first person. Stands the avatar under "
            "the headset every frame (so physical walking carries it and "
            "looking down shows the chest, legs and feet), faces it the way "
            "the player faces whatever axis the GLB was authored on, scales "
            "it to the player's eye height, drives the neck and head from "
            "the headset, and solves two-bone IK on both arms to the "
            "controllers. Head visibility is PER-VIEW: the head is hidden in "
            "the player's own view, while a mirror still shows a complete "
            "avatar. The avatar's OWN hands are the player's hands: their "
            "fingers curl through builtin.hand_pose when the rig has finger "
            "bones; a rig that stops at the wrist keeps rigid hands. No "
            "substitute hands are ever drawn. Only in Full Player Avatar "
            "games (role:body); bound on the player_avatar entity itself. "
            "Never write finger curl here, and never move the avatar by "
            "writing CTransform."
        ),
        category="avatar",
        params={
            "coverage": (
                "'full_body' (default) / 'neck_down' / 'hands_only'. Match "
                "the avatar asset. Default to full_body: a head that exists "
                "can be hidden, a head never generated cannot be shown."
            ),
            "head_visibility": (
                "'hidden_in_self_view' (default) / 'always_visible' / "
                "'toggleable'. Use toggleable when the prompt wants a control "
                "to show or hide the head, e.g. admiring an outfit."
            ),
            "leg_animation": (
                "bool, default true. false = the avatar slides without leg "
                "movement, which is the 'I don't want my legs to move' case."
            ),
            "arm_ik": "bool, default true",
            "head_tracking": "bool, default true",
            "eye_height_offset_m": (
                "float, default 0. Trim where the camera sits relative to "
                "the rig's head joint; rigs disagree about whether that is "
                "the skull base or between the eyes."
            ),
            "walk_speed_mps": "float, default 1.4",
            "run_speed_mps": "float, default 4.0",
            "height_calibration": (
                "'auto' (default: scale to the player's eye height) / 'off'"
            ),
            "torso_yaw_deadzone_deg": "float, default 40",
        },
        inputs=("headset pose", "controller poses", "body velocity"),
        comfort_notes=(
            "One extra skinned rig plus two analytic IK solves per frame."
        ),
        role="role:body",
    ),
    CatalogEntry(
        id="builtin.mirror_surface",
        summary=(
            "A planar mirror that reflects the player's avatar and what it is "
            "wearing. Bind it to a flat quad entity; the plane comes from the "
            "entity's own orientation. Implemented as reflected geometry "
            "drawn straight into the output buffer, NOT a render-to-texture "
            "pass, because an offscreen target on a Quest's tile-based GPU "
            "forces a GMEM round-trip and bandwidth is the bottleneck. The "
            "reflected set is deliberately small and one mirror is active at "
            "a time. Use this whenever the prompt mentions a mirror, a "
            "reflection, or avatar customization."
        ),
        category="rendering",
        params={
            "plane_normal": (
                "[x,y,z] local-space normal, default [0,0,1] (a quad facing "
                "the viewer)"
            ),
            "max_active_distance_m": "float, default 6.0",
            "reflect_tags": (
                "list of entity tags to include beyond the avatar and "
                "anything tagged 'mirror_visible'"
            ),
            "max_reflected": "int, default 24 -- a hard draw-call guard",
        },
        inputs=("player position",),
        comfort_notes=(
            "Mirrors are the classic mobile-VR frame-budget trap. Published "
            "Quest 3 numbers put a full-scene stereo mirror at 2.5-3.5 ms, "
            "which is why only the avatar is reflected. Keep max_reflected "
            "small and check the perf line on device."
        ),
    ),
    # ----- Wearing things -----
    CatalogEntry(
        id="builtin.equip",
        summary=(
            "Wear an object on a body, and take it off again. With NO params "
            "an equippable object goes anywhere on the body. Two placement "
            "modes cover what prompts actually ask for: a slot whitelist for "
            "a glove that belongs on a hand, and a COVERAGE SPAN for "
            "something that has to cover a stretch of limb -- an Iron Man "
            "gauntlet from the fingertips to the elbow. A span is measured on "
            "the rig at runtime and the object is fitted to it, because the "
            "distance from a fingertip to an elbow is a property of that rig "
            "and its pose, not a constant. equip_on and unequip_on are "
            "INDEPENDENT, so a suit that can never come off is "
            "unequip_on='never' and an escape-room game whose items only ever "
            "come off is equip_on='never' plus scene_start. Prefer this with "
            "config over generating an equip mechanic."
        ),
        category="interaction",
        params={
            "slots": (
                "list of slot names this object may occupy, e.g. "
                "['hand_l','hand_r'] for gloves. Empty = anywhere."
            ),
            "coverage_start": (
                "body slot at one end of the span, e.g. 'index_tip_r'"
            ),
            "coverage_end": "body slot at the other end, e.g. 'elbow_r'",
            "fit_to_span": (
                "bool, defaults true when coverage is set: scale the object "
                "so its long axis matches the measured span"
            ),
            "rest_slots": (
                "extra contact points that refine orientation -- how glasses "
                "sit on the nose and both ears while anchored at the face"
            ),
            "equip_on": (
                "'grab_release' (default) / 'button' / 'contact' / 'api' / "
                "'scene_start' / 'never'"
            ),
            "unequip_on": (
                "'grab_pull' (default) / 'button' / 'api' / 'never'"
            ),
            "passive_grab_protection": (
                "bool, default true. Brushing a hand past a worn item must "
                "not knock it off. This does NOT block deliberate removal."
            ),
            "removable": (
                "bool, default true. Whether deliberate removal is allowed "
                "right now. Runtime-mutable via ctx.equip->set_removable, "
                "which is how a helmet is fixed in flight and free on the "
                "ground on the same item."
            ),
            "re_equippable": (
                "bool, default true. Whether this instance may be equipped "
                "again -- a sterile glove that came off is contaminated."
            ),
            "unequip_requires_free_hand": "bool, default true",
            "unequip_with_other_hand_only": (
                "bool, default true. Structural, not a preference: a glove "
                "worn on the right hand cannot be grabbed by that hand."
            ),
            "exclusive": "bool, default true -- replace rather than stack",
            "on_replace": (
                "'to_hand' (default) / 'drop' / 'despawn' / 'inventory'"
            ),
            "group": "outfit-set id, so one button equips several pieces",
            "snap_radius_m": "float, default 0.25",
            "auto_side": "bool, default true",
            "rotation_offset_deg": "[x,y,z] manual trim",
            "position_offset_m": "[x,y,z] manual trim",
        },
        inputs=("grip (equip on release, unequip on a deliberate pull)",),
        comfort_notes=(
            "Equip state is engine-owned via ctx.equip, so loadout and "
            "outfit survive an environment transition without any mechanic "
            "writing save code. A menu-driven wardrobe is generated UI code "
            "over ctx.equip, not a second equip implementation."
        ),
    ),
    CatalogEntry(
        id="builtin.ui_panel",
        summary=(
            "A world-anchored virtual panel with a laser pointer: a list of "
            "rows, either hand may point, trigger to click. The engine owns "
            "where the panel sits (Meta's comfort placement), which row the "
            "pointer is on, and what counts as a click; the GAME owns what a "
            "row means. Use this for a wardrobe or loadout screen ('show what "
            "I am wearing, let me take it off'), a mission select, an "
            "inventory, or dialogue choices. A fixed menu needs NO generated "
            "code at all -- declare its rows in params. A dynamic menu is thin "
            "generated code that calls ctx.ui->set_rows each frame and "
            "ctx.ui->poll_click to act; pair it with ctx.equip for a wardrobe "
            "and ctx.transition for a mission select. Never generate a menu "
            "from scratch. NOTE: the engine has no in-world TEXT rendering, so "
            "a row reads as a state-coloured plate plus an optional thumbnail "
            "of the actual item -- which is better in VR anyway, since the "
            "player recognises their gauntlet by looking at it."
        ),
        category="ui",
        params={
            "panel_id": (
                "string, required in practice: the id generated code passes "
                "to ctx.ui->open / set_rows / poll_click"
            ),
            "open_on_start": "bool, default false",
            "rows": (
                "list of {id, label, enabled} for a FIXED menu. Leave empty "
                "for a panel whose rows generated code sets each frame."
            ),
            "distance_m": (
                "float, default 1.5 -- how far in front of the eyes it sits"
            ),
            "below_eye_m": (
                "float, default 0.20 -- drop below eye level, so the resting "
                "line of sight lands on the panel without neck strain"
            ),
            "row_height_m": "float, default 0.12",
            "width_m": "float, default 0.42",
            "hit_radius_m": (
                "float, default 0.09 -- how close the ray must pass to select"
            ),
            "pointer_hand": (
                "0 = left, 1 = right, -1 (default) = either. Pin it only for "
                "a panel meant to be held in one hand and operated with the "
                "other."
            ),
        },
        inputs=("trigger (click the highlighted row)",),
        comfort_notes=(
            "Anchored once when opened and then left alone: re-anchoring on "
            "every change would jerk the panel around exactly when the player "
            "is reading it. Placed ~1.5 m forward and slightly below eye level "
            "per Meta's Head/Comfort guidance. A disabled row is skipped by "
            "the pointer rather than highlighted-then-refused, because a "
            "highlight the player cannot act on reads as a broken panel."
        ),
    ),
    CatalogEntry(
        id="builtin.head_interior",
        summary=(
            "The player is INSIDE this character's head, at miniature scale, "
            "in real walkable geometry. Bind it to the HOST -- the character "
            "whose head it is, which may be the player's own avatar or a "
            "separate NPC. The interior is a NESTED ENVIRONMENT authored by "
            "the world-generation stage (BuildSpec scene with kind='interior', "
            "environment_mode='nested'), never a Tripo asset: image-to-3D "
            "cannot describe an enclosure you stand inside. There is no "
            "render-to-texture and no portal in this path -- the player is "
            "genuinely shrunk and both they and the interior hang off the same "
            "head joint, so the host can walk around while the passenger "
            "notices nothing."
        ),
        category="perspective",
        params={
            "interior_scene_id": (
                "REQUIRED, and never inferred: the scene id of the nested "
                "interior environment. A nested interior has no scene of its "
                "own to merge into (it is loaded at runtime against a host), "
                "so the world.<scene_id> convention does not apply here."
            ),
            "anchor": (
                "body slot to mount on, default 'head'. Naming a joint "
                "matters: the root transform of a walking character sits at "
                "its feet and does not turn with the neck."
            ),
            "fit_to_host_extent_m": (
                "float, default 0.22 -- the real size the interior is shrunk "
                "to. Author it at HUMAN scale; the engine measures what was "
                "built and scales it down. A level laid out inside a 22 cm box "
                "produces geometry and nav regions too degenerate to walk on."
            ),
            "player_scale": (
                "float, default derived from fit_to_host_extent_m so the "
                "player is proportioned to the room they are standing in"
            ),
            "head_tracking": (
                "'decoupled' (default) = physical head rotation looks around "
                "the skull and never moves the host, which is right for a "
                "PASSENGER -- turning to look behind you must not wrench a "
                "stranger's neck around. 'drives_host_head' = HMD rotation "
                "drives the host's neck, right when the host IS the player."
            ),
            "control_routing": (
                "'host_locomotion' (default) = sticks move the host, the "
                "player is a passenger who steers. 'self_locomotion' = sticks "
                "move the miniature player around inside the head while the "
                "host lives its own life as a normal NPC with its own "
                "behaviours. 'host_piloted' = sticks drive the host's movement "
                "AND its turning."
            ),
            "host_speed_mps": "float, default 1.6",
            "host_turn_deg_per_s": "float, default 90",
        },
        inputs=("thumbstick (routed to the host or the player per config)",),
        comfort_notes=(
            "Host movement is written as CLocomotionIntent, never as a "
            "transform, so builtin.character_locomotion still owns the body "
            "and its stride matches the ground covered. Host head rotation "
            "goes through ISkeleton::set_joint_rotation so it composes with "
            "the animation system instead of replacing the pose. The player's "
            "clip planes and collision capsule scale with them, or a "
            "two-centimetre passenger cannot see their own hands. For "
            "'self_locomotion' the ordinary locomotion built-ins already move "
            "the player inside the head, because their rig is frame-local -- "
            "never generate a second mover."
        ),
    ),
    # ----- Affordances: things in the world the player can act on -----
    #
    # These bind to entities the WORLD BUILDER declared as interactive, and
    # they read and write their state through ctx.interactive. That is what
    # makes an opened door still open after the player leaves and comes back
    # -- with no save code in any mechanic. Never generate a mechanic that
    # duplicates one of these; configure it instead.
    CatalogEntry(
        id="builtin.door",
        summary=(
            "A hinged or sliding door, gate, window, garage door or trapdoor. "
            "Grab it and it swings between the range the world builder "
            "declared (0-90 degrees for a standard door, wider where the "
            "design allows). The hinge point, axis and limits come from the "
            "environment's own declaration, so this mechanic never has to "
            "guess where the hinge is. Opening or closing it is recorded "
            "automatically and survives an environment transition."
        ),
        category="affordance",
        params={
            "motion_tau_s": "float, default 0.18 -- swing smoothing",
            "reach_m": (
                "float, default 0.18. How close a hand must be to the handle."
            ),
        },
        inputs=("grip near the handle",),
        comfort_notes=(
            "Requires the world builder to have declared an interactive of "
            "kind 'door' on this entity. Its collider is driven with the "
            "visual, so a closed door genuinely blocks movement and gated "
            "navigation links respect it."
        ),
    ),
    CatalogEntry(
        id="builtin.switchable",
        summary=(
            "Anything with discrete states that does not move: lights, "
            "switches, faucets, televisions, monitors, fireplaces, steam "
            "vents, conveyors, traffic lights. States are ENUMERATED rather "
            "than on/off, so a traffic light cycling green-amber-red is the "
            "same mechanic as a lamp."
        ),
        category="affordance",
        params={
            "state_key": (
                "string, default 'on'. The natural word for this object's "
                "state -- 'on' for a lamp, 'running' for a conveyor, 'lit' "
                "for a fireplace."
            ),
            "reach_m": "float, default 0.18",
            "cooldown_s": "float, default 0.25",
        },
        inputs=("grip near the object",),
        comfort_notes=(
            "The state list comes from the world builder's declaration; with "
            "no list this is a simple flag."
        ),
    ),
    CatalogEntry(
        id="builtin.container",
        summary=(
            "A drawer, cabinet or chest. Same motion as a door, but the "
            "state worth recording is open versus closed, and a partially "
            "open drawer is a real authored state rather than a rounding "
            "error. Contents are tracked separately from the lid."
        ),
        category="affordance",
        params={
            "motion_tau_s": "float, default 0.18",
            "reach_m": "float, default 0.18",
        },
        inputs=("grip near the handle",),
        comfort_notes="Needs an interactive of kind 'container'.",
    ),
    CatalogEntry(
        id="builtin.mover",
        summary=(
            "Something that travels between NAMED positions: an elevator "
            "between floors, a drawbridge raised or lowered, a parking "
            "barrier, retractable stairs, a folding bunk, curtains, a ladder "
            "stored or deployed. Interacting cycles to the next declared "
            "position; generated code can set a specific one directly via "
            "ctx.interactive->set_state."
        ),
        category="affordance",
        params={
            "motion_tau_s": "float, default 0.18",
            "reach_m": (
                "float, default 0.18. Raise it for something operated at a "
                "distance, like a bridge winch."
            ),
        },
        inputs=("grip near the object",),
        comfort_notes=(
            "The position list comes from the world builder's declaration."
        ),
    ),
    CatalogEntry(
        id="builtin.breakable",
        summary=(
            "Goes from intact to destroyed, one way: glass, a destructible "
            "wall, a rock blockage. Pair it with a gated navigation link and "
            "destroying the obstacle genuinely opens the route rather than "
            "just changing how it looks."
        ),
        category="affordance",
        params={
            "breaks_by_hand": (
                "bool, default true. False for something that needs a "
                "reason -- a reinforced wall broken by an explosion rather "
                "than a touch. Generated code then sets the state."
            ),
            "hide_when_destroyed": "bool, default true",
            "reach_m": "float, default 0.18",
        },
        inputs=("grip near the object",),
        comfort_notes="Needs an interactive of kind 'breakable'.",
    ),
    CatalogEntry(
        id="builtin.harvestable",
        summary=(
            "A REGION that yields one object per pick: grass, flowers, "
            "rubble, berries, coins. This is the correct answer to 'the "
            "player can pick grass' -- the field is batched visual geometry "
            "with ONE harvestable region, and each pick spawns a single "
            "grabbable instance from a scene template. Never declare an "
            "entity per blade: measured VR foliage tests fall under 10 FPS "
            "at around 2,000 plant instances, and a busy Quest 3 scene has "
            "a budget of 200-300 draw calls."
        ),
        category="affordance",
        params={
            "max_picks": (
                "int, default 0 = unlimited. Set it for a one-off cache."
            ),
            "into_hand": (
                "bool, default true -- the picked object arrives in the hand "
                "that reached for it."
            ),
            "reach_m": "float, default 0.5 (an area, not a doorknob)",
            "cooldown_s": "float, default 0.25",
        },
        inputs=("grip inside the region",),
        comfort_notes=(
            "The yielded object must exist in the scene as a template "
            "entity, named by the world builder's yields_asset_id."
        ),
    ),
    # ----- Game loop -----
    CatalogEntry(
        id="builtin.game_flow",
        summary=(
            "The game LOOP, run from game_design.json: notices the player's "
            "health reaching zero, spends lives, fades out, respawns them "
            "(spawn / checkpoint / last safe spot, health restored, loadout "
            "kept, a few seconds of invulnerability, nearby enemies cleared), "
            "tracks objectives (survive a time, defeat N, clear the waves, "
            "reach a place, collect items, custom ones reported by game code "
            "through ctx.game), and shows the end screens -- Game Over with "
            "Try Again, You Win / The End with Play Again, Leave Game -- plus "
            "a pause menu on the left menu button. Try Again reloads the "
            "scene fresh; Leave Game quits the app properly on Quest and PC. "
            "The scene compiler binds it from game_design.json -- NEVER plan "
            "or generate lives, respawn, a run flow, a results/game-over "
            "screen, a restart or a quit button. Game-specific objectives "
            "call ctx.game->add_progress / complete / fail instead."
        ),
        category="game",
        params={
            "(compiled)": "everything comes from game_design.json; do not author params",
        },
        inputs=("player health (ctx.combat)", "objectives", "left menu button",
                "end-screen clicks"),
        comfort_notes=(
            "Every relocation happens under a fade to black; end screens are "
            "world-locked panels 1.5 m away, slightly below eye level; the "
            "sticks are locked and the player invulnerable while one is up."
        ),
    ),
    CatalogEntry(
        id="builtin.game_hud",
        summary=(
            "The loop's numbers on the left wrist, showing only what the "
            "game has: a health bar, lives, the clock of a timed objective, "
            "score, and the current objective. Bound by the scene compiler "
            "from game_design.json's hud block. A game-specific HUD element "
            "(an ammo counter on the weapon) is still generated; the health "
            "/ lives / timer / objective HUD is not."
        ),
        category="game",
        params={
            "(compiled)": "health / lives / timer / score / objective flags from game_design.json",
        },
        inputs=("ctx.game", "ctx.combat"),
        comfort_notes=(
            "Wrist-mounted, never head-locked: a HUD glued to the face cannot "
            "be looked away from."
        ),
    ),
)


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------


def builtin_catalog() -> list[dict[str, object]]:
    """JSON-friendly list of built-in mechanic descriptors."""
    return [e.to_json() for e in _BUILTINS]


# ---------------------------------------------------------------------------
# Player roles
#
# A role is a job slot, not code: "the one thing in this game that does X".
# It is filled either by its stock built-in or by a generated replacement
# (``builtin+config_code_edit`` inherits its base built-in's role and REPLACES
# it). At most one mechanic may fill each role in a scene -- two teleporters
# bound at once is how a game ended up with two aim arcs.
#
# ``host`` is the entity whose ``mechanic_bindings`` carries the binding.
# ---------------------------------------------------------------------------

PLAYER_ROLES: dict[str, dict[str, str]] = {
    "role:fingers": {"builtin": "builtin.hand_pose", "host": "player_rig"},
    "role:body": {"builtin": "builtin.avatar_body", "host": "player_avatar"},
    "role:teleport": {
        "builtin": "builtin.locomotion.teleport", "host": "player_rig"},
    "role:slide_head": {
        "builtin": "builtin.locomotion.slide.head_relative",
        "host": "player_rig"},
    "role:slide_controller": {
        "builtin": "builtin.locomotion.slide.controller_relative",
        "host": "player_rig"},
    "role:snap_turn": {
        "builtin": "builtin.locomotion.snap_turn", "host": "player_rig"},
    "role:smooth_turn": {
        "builtin": "builtin.locomotion.smooth_turn", "host": "player_rig"},
    "role:movement_settings_menu": {
        "builtin": "builtin.movement_settings_menu", "host": "player_rig"},
}

#: BuildSpec movement mode -> role.
MOVEMENT_MODE_ROLES: dict[str, str] = {
    "teleport": "role:teleport",
    "slide_head": "role:slide_head",
    "slide_controller": "role:slide_controller",
}
#: BuildSpec turn style -> role.
TURN_ROLES: dict[str, str] = {
    "snap": "role:snap_turn",
    "smooth": "role:smooth_turn",
}


def role_of(mechanic_id: str | None) -> str:
    """The player role a built-in (or alias) fills, or ""."""
    if not mechanic_id:
        return ""
    e = entry(mechanic_id)
    return e.role if e is not None else ""


def role_of_plan_item(item: object) -> str:
    """The role a MechanicPlan item fills.

    A built-in fills its own role; a ``builtin+config_code_edit`` copy fills
    its BASE built-in's role, because it replaces that built-in. A fresh
    ``generate`` item fills no role.
    """
    classification = getattr(item, "classification", None)
    cls = getattr(classification, "value", classification)
    if cls in ("builtin", "builtin+config"):
        return role_of(getattr(item, "builtin_id", None))
    if cls == "builtin+config_code_edit":
        return role_of(getattr(item, "base_builtin_id", None))
    return ""


def builtin_ids() -> list[str]:
    """Just the canonical ids (no aliases) — handy for assertions/tests."""
    return [e.id for e in _BUILTINS]


def entry(mechanic_id: str) -> CatalogEntry | None:
    """One catalog entry by canonical id or alias, or None.

    Exists so callers and tests can inspect a built-in's documented params
    without reaching into the module's private tuple.
    """
    for e in _BUILTINS:
        if e.id == mechanic_id or mechanic_id in e.aliases:
            return e
    return None


def all_known_ids() -> set[str]:
    """Canonical ids + aliases. Used by the planner to validate a plan
    references something the engine actually knows about."""
    out: set[str] = set()
    for e in _BUILTINS:
        out.add(e.id)
        out.update(e.aliases)
    return out


def promoted_registry_path(repo_root: Path) -> Path:
    """Where promoted generated mechanics get indexed."""
    return repo_root / "engine" / "promoted" / "registry.json"


def promoted_catalog(repo_root: Path) -> list[dict[str, object]]:
    """Read the promoted registry; tolerate missing/malformed file."""
    path = promoted_registry_path(repo_root)
    if not path.exists():
        return []
    try:
        text = path.read_text(encoding="utf-8")
        data = json.loads(text)
    except (OSError, json.JSONDecodeError):
        return []
    if not isinstance(data, dict):
        return []
    items = data.get("items")
    if not isinstance(items, list):
        return []
    return [it for it in items if isinstance(it, dict) and "id" in it]


def promoted_sources_for_plan(repo_root: Path, plan: object, project_id: str) -> list[str]:
    """The .cpp files of promoted mechanics this plan reuses from OTHER games.

    Builds compile only the current game's generated code, so a mechanic
    promoted from another game has to be handed to the build explicitly: all
    of its task's translation units (``<code_task_id>*.cpp`` beside the
    registered source). Returned as absolute forward-slash paths."""
    used = set()
    for item in getattr(plan, "items", None) or []:
        for attr in ("builtin_id", "base_builtin_id"):
            value = getattr(item, attr, None)
            if isinstance(value, str) and value.startswith("promoted."):
                used.add(value)
    if not used:
        return []
    out: list[str] = []
    for entry in promoted_catalog(repo_root):
        if entry.get("id") not in used or entry.get("project_id") == project_id:
            continue
        source = repo_root / str(entry.get("source_path") or "")
        task_id = str(entry.get("code_task_id") or source.stem)
        if not source.parent.is_dir():
            continue
        for cpp in sorted(source.parent.glob(f"{task_id}*.cpp")):
            out.append(str(cpp.resolve()).replace("\\", "/"))
    return out


def _render_entries(entries: Iterable[dict[str, object]]) -> str:
    lines: list[str] = []
    for e in entries:
        eid = e.get("id", "?")
        summary = e.get("summary", "")
        params = e.get("params") or {}
        inputs = e.get("inputs") or []
        comfort = e.get("comfort_notes") or ""
        aliases = e.get("aliases") or []
        bullet = f"- `{eid}` — {summary}"
        if params:
            param_bits = ", ".join(f"{k}={v}" for k, v in params.items())
            bullet += f"\n    params: {param_bits}"
        if inputs:
            bullet += f"\n    inputs: {', '.join(inputs)}"
        if comfort:
            bullet += f"\n    comfort: {comfort}"
        if aliases:
            bullet += f"\n    aliases: {', '.join(aliases)}"
        lines.append(bullet)
    return "\n".join(lines)


def as_planner_text(repo_root: Path) -> str:
    """Render the catalog (built-in + promoted) as planner-ready prompt text."""
    builtin_lines = _render_entries(builtin_catalog())
    promoted = promoted_catalog(repo_root)
    if promoted:
        promoted_lines = _render_entries(promoted)
        promoted_block = (
            "\n\nPROMOTED FROM PAST GAMES (also reusable; reference by id):\n"
            + promoted_lines
        )
    else:
        promoted_block = (
            "\n\nPROMOTED FROM PAST GAMES: (none yet — the engine will grow as games are built.)"
        )
    return (
        "ENGINE CATALOG — built-in mechanics (reference these by id whenever possible):\n"
        + builtin_lines
        + promoted_block
    )


def full_catalog_text(repo_root: Path) -> str:
    """Backwards-friendly alias for ``as_planner_text``."""
    return as_planner_text(repo_root)
