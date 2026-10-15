"""What each animation provider can give a character.

The character-routing agent compares these two libraries for every humanoid
NPC and picks the provider that covers what the NPC actually needs. Keeping
them here, beside the clients that call the providers, means the prompt the
agent reads and the validation the pipeline applies use the same lists.

* ``TRIPO_BIPED_V1_PRESETS`` -- every preset of Tripo's rig model
  ``v1.0-20240301`` (biped / humanoid only, 90+ presets). Tripo animations
  are fixed clips: actions, gestures and a few gaits.
* ``UTHANA_STYLES_FALLBACK`` -- Uthana locomotion styles. The pipeline asks
  the live API (``list_locomotion_styles``) and only falls back to this list
  when that query fails. Every Uthana style is a GAIT: the character travels
  the whole clip, at a chosen speed and stride count.

The speed factors are the engine's: a clip's apparent ground speed is
``factor x body height``. They are mirrored in
``engine/runtime/src/mechanics/builtin/LocomotionClipSelector.h`` and in the
scene compiler, and a parity test keeps all three equal.
"""

from __future__ import annotations

#: Walk clip ground speed per metre of body height (m/s per m).
WALK_SPEED_PER_M = 0.78
#: Run clip ground speed per metre of body height (m/s per m).
RUN_SPEED_PER_M = 2.2

TRIPO_BIPED_V1_PRESETS: tuple[str, ...] = (
    "preset:biped:afraid", "preset:biped:agree",
    "preset:biped:angry_01", "preset:biped:angry_02",
    "preset:biped:angry_03", "preset:biped:basketball_shot",
    "preset:biped:bow", "preset:biped:box_01",
    "preset:biped:box_02", "preset:biped:box_03",
    "preset:biped:cast_a_spell", "preset:biped:cheer",
    "preset:biped:chop", "preset:biped:clap",
    "preset:biped:climb", "preset:biped:complain_01",
    "preset:biped:complain_02", "preset:biped:cross_body_crunch",
    "preset:biped:crossover_dribble", "preset:biped:cry",
    "preset:biped:dance_01", "preset:biped:dance_02",
    "preset:biped:dance_03", "preset:biped:dance_04",
    "preset:biped:dance_05", "preset:biped:dance_06",
    "preset:biped:defeat_02", "preset:biped:defeat_03",
    "preset:biped:depressed", "preset:biped:dig",
    "preset:biped:dive", "preset:biped:dribble",
    "preset:biped:fall", "preset:biped:fire",
    "preset:biped:flee_01", "preset:biped:flee_02",
    "preset:biped:flip", "preset:biped:fold_arms",
    "preset:biped:football_catch", "preset:biped:football_save",
    "preset:biped:football_pass", "preset:biped:freaky",
    "preset:biped:frightened", "preset:biped:front_kick_01",
    "preset:biped:front_kick_02", "preset:biped:frustrated_01",
    "preset:biped:frustrated_02", "preset:biped:golf",
    "preset:biped:greet_01", "preset:biped:greet_02",
    "preset:biped:greet_03", "preset:biped:greet_04",
    "preset:biped:heart_pose", "preset:biped:hit_to_body_01",
    "preset:biped:hit_to_body_02", "preset:biped:hit_to_head",
    "preset:biped:hit_to_side", "preset:biped:hit_to_stomach",
    "preset:biped:hug", "preset:biped:hurt",
    "preset:biped:idle", "preset:biped:jump_down",
    "preset:biped:jump", "preset:biped:jump_rope_01",
    "preset:biped:jump_rope_02", "preset:biped:laugh_01",
    "preset:biped:laugh_02", "preset:biped:lift_heavy",
    "preset:biped:look_around", "preset:biped:make_a_call_01",
    "preset:biped:make_a_call_02", "preset:biped:pitch_baseball",
    "preset:biped:play_mobile_game", "preset:biped:play_video_game",
    "preset:biped:press-up", "preset:biped:run_upstairs",
    "preset:biped:run", "preset:biped:scared_01",
    "preset:biped:scared_02", "preset:biped:scratch",
    "preset:biped:shoot", "preset:biped:shovel",
    "preset:biped:sing_01", "preset:biped:sing_02",
    "preset:biped:sing_03", "preset:biped:sing_04",
    "preset:biped:sit", "preset:biped:slash",
    "preset:biped:sob", "preset:biped:standing_relax",
    "preset:biped:surf", "preset:biped:swagger",
    "preset:biped:swim", "preset:biped:turn",
    "preset:biped:victory_celebration", "preset:biped:volleyball",
    "preset:biped:wait", "preset:biped:walk",
    "preset:biped:warm_up", "preset:biped:wave_goodbye_01",
    "preset:biped:wave_goodbye_02",
)

UTHANA_STYLES_FALLBACK: tuple[str, ...] = (
    "aeroplane", "akimbo", "angry", "arms_above_head",
    "arms_behind_back", "arms_by_side", "arms_folded", "avoiding_crowd",
    "balancing", "beating_chest", "being_followed", "bent_forward",
    "bent_knees", "big_steps", "bouncy_left", "bouncy_right",
    "cat", "chicken", "cross_over", "crouching",
    "depressed", "dinosaur", "dragging_leg_left", "dragging_leg_right",
    "drunk", "duck_foot", "elated", "elderly",
    "fairy_steps", "feeling_in_the_dark", "flapping_arms", "flicking_legs",
    "graceful_arms", "hands_between_legs", "hands_in_pockets", "heavyset",
    "high_knees", "holding_sword", "holding_torch_a", "holding_torch_b",
    "hop_both", "hop_left", "hop_right", "karate_chop",
    "kicking", "lawn_mower", "lean_back", "lean_left",
    "lean_right", "legs_apart", "limp_left", "limp_right",
    "looking_up", "lunge", "march", "monk",
    "morris_dance", "neutral_female_a", "neutral_female_b", "neutral_male_a",
    "neutral_non_gendered", "on_fire", "on_heels", "on_phone_left",
    "on_phone_right", "on_toes_bent_forward", "on_toes_crouching", "pendulum_hands",
    "penguin", "pigeon_toed_a", "pigeon_toed_b", "pistol_female",
    "pistol_non_gendered", "proud", "punching", "quail",
    "raised_arm_left", "raised_arm_right", "rifle_female", "rifle_male",
    "roadrunner", "robot", "rocket", "rushed",
    "shielding_left", "shielding_right", "skipping", "sliding_feet",
    "spinning_clockwise", "spinning_counter_clockwise", "star", "start_stop",
    "step_together", "stiff", "strutting", "superman",
    "swatting", "sweeping", "swimming", "swinging_arms",
    "swinging_shoulders", "teapot", "tiptoe", "walking_stick_left",
    "walking_stick_right", "waving", "whirling_arms", "wide_legs",
    "wiggling_hips", "wild_arms", "wild_legs", "wounded",
    "zombie",
)

#: Uthana's documented move_speed bounds (m/s).
UTHANA_MIN_MOVE_SPEED = 0.25
UTHANA_MAX_MOVE_SPEED = 8.0
#: Uthana's documented stride-pair bounds.
UTHANA_MIN_STRIDES = 1
UTHANA_MAX_STRIDES = 5

#: Tripo's batch limit for one retarget call.
TRIPO_MAX_PRESETS_PER_ASSET = 5


def tripo_preset_key(preset: str) -> str:
    """The animation key a Tripo preset is exposed under (``box_01``)."""
    return preset.rsplit(":", 1)[-1]


def is_tripo_preset(preset: str) -> bool:
    return preset in TRIPO_BIPED_V1_PRESETS


def gait_speed(height_m: float, gait: str) -> float:
    """Apparent ground speed (m/s) of a ``walk`` or ``run`` clip for a body."""
    factor = RUN_SPEED_PER_M if gait == "run" else WALK_SPEED_PER_M
    return factor * float(height_m)


def tripo_catalog_text() -> str:
    """The Tripo preset list, one per line, for an agent prompt."""
    return "\n".join(TRIPO_BIPED_V1_PRESETS)
