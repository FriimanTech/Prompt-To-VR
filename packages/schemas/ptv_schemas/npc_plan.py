"""How every non-player character in a game is supposed to behave.

Why this is its own artifact
----------------------------
Placing a goblin model in a scene is not the same as producing an enemy. The
prompt says what the NPCs are FOR -- patrolling guards, a squad that follows
you, ten ducks that slide at you and nothing else -- and that intent has to be
read once, deliberately, and written down. Otherwise every downstream stage
re-derives it from the same prose and they disagree: the manifest rigs a duck
that the codegen agent then writes as a static slider, and thirty credits are
gone.

It runs BEFORE rigging on purpose. "Does this thing need animation at all" is
the single most expensive question in the pipeline -- a rig is ~30 credits and
each animation clip another ~20 -- and it cannot be answered from the asset
alone. Only the prompt knows that the ducks are meant to slide.

Reading the prompt correctly
----------------------------
* Explicit instructions win. "The ducks should not be animated, they just
  slide" means ``movement_style = slide_no_animation`` and no intents.
* Silence is not "do nothing". A prompt about Green Lanterns flying together
  never says the allied Lanterns can fly; the engine still has to give them
  flight, because a Green Lantern that walks is not what was asked for.
* Games with no NPCs at all produce an empty plan, and that is a valid,
  complete answer -- a book-reading experience needs no combatants.
"""

from __future__ import annotations

from enum import Enum
from typing import List, Optional

from pydantic import Field

from .asset_manifest import AnimationIntent
from .common import IdStr, StrictModel


class NpcRole(str, Enum):
    """What this character is to the player."""

    #: Fights the player and their allies.
    enemy = "enemy"
    #: Fights alongside the player -- a squadmate, a summoned creature.
    ally = "ally"
    #: Talks, trades, or exists as set dressing; does not fight.
    neutral = "neutral"
    #: Ambient life. Reacts to the player but has no combat role.
    critter = "critter"
    #: A boss: an enemy with distinct phases or far more health.
    boss = "boss"


class MovementStyle(str, Enum):
    """How the character gets around, which decides whether it is rigged."""

    #: Walk/run cycles driven by the engine's character locomotion. The
    #: default for anything with legs.
    animated_locomotion = "animated_locomotion"
    #: Moves through the world with NO animation -- the raw model slides.
    #: Only when the prompt explicitly asks for it, or the thing genuinely
    #: has no gait (a drone, a floating orb, a rolling boulder). Skips
    #: rigging and animation entirely, which also skips their cost.
    slide_no_animation = "slide_no_animation"
    #: Never moves from where it is placed. A turret, a shopkeeper.
    stationary = "stationary"
    #: Moves in three dimensions rather than over the floor.
    flying = "flying"


class NpcBehavior(str, Enum):
    """A capability this character needs. Combine as many as the prompt implies.

    These are what the code-generation agent turns into C++. Each one is a
    thing the character DOES, not a state machine -- how they interleave is
    the generated mechanic's business.
    """

    idle = "idle"
    #: Walks a route or wanders inside a home area.
    patrol = "patrol"
    #: Moves toward its target.
    chase = "chase"
    #: Moves away when hurt or outmatched.
    flee = "flee"
    #: Attacks at range with a projectile.
    ranged_attack = "ranged_attack"
    #: Attacks in contact.
    melee_attack = "melee_attack"
    #: Damages whatever it touches, without a distinct attack. The ducks.
    contact_damage = "contact_damage"
    #: Breaks line of sight behind geometry.
    take_cover = "take_cover"
    #: Moves to a new position when the current one is exposed.
    reposition = "reposition"
    #: Stays near a leader, keeping spacing.
    follow_leader = "follow_leader"
    #: Holds a location against intruders.
    guard_point = "guard_point"
    #: Reacts to being hit.
    react_to_damage = "react_to_damage"


class AnimationFallback(str, Enum):
    """What to do when Tripo has no animation for something the NPC must do.

    The gameplay capability is never blocked on the animation. An NPC that
    should fly still flies; the only question is whether it looks animated
    while doing so.
    """

    #: Use the closest preset Tripo does offer. A creature rig has only
    #: locomotion, so an "attack" becomes its walk, which is wrong-looking
    #: but present.
    nearest_preset = "nearest_preset"
    #: Have the code-generation agent animate the rig itself, by driving
    #: joints each frame through ``ctx.skeleton->set_joint_rotation``. Good
    #: for simple, repetitive motion: wing flaps, a turret's recoil, a
    #: tentacle wave.
    procedural_cpp = "procedural_cpp"
    #: Ship the ability with no animation. Correct when the motion is
    #: invisible anyway or the model has no limbs to move.
    none = "none"


class NpcLocomotion(StrictModel):
    walk_speed_mps: float = Field(default=1.4, gt=0.0, le=30.0)
    run_speed_mps: float = Field(default=4.0, gt=0.0, le=60.0)
    turn_rate_deg_s: float = Field(default=540.0, gt=0.0, le=3600.0)
    #: How far it will stray from where it started. 0 = no limit. Use it for
    #: guards that hold a post rather than following the player across the
    #: level.
    leash_radius_m: float = Field(default=0.0, ge=0.0, le=1000.0)
    #: Metres between the floor and the soles while it moves, for a
    #: ``flying`` character the prompt says HOVERS (a drone, a ghost).
    #: 0 = moves freely in three dimensions. Ignored for grounded movement.
    hover_height_m: float = Field(default=0.0, ge=0.0, le=50.0)


class NpcPerception(StrictModel):
    sight_range_m: float = Field(default=25.0, ge=0.0, le=500.0)
    fov_degrees: float = Field(default=120.0, gt=0.0, le=360.0)
    #: Range at which it notices something it cannot see. 0 disables hearing.
    hearing_range_m: float = Field(default=12.0, ge=0.0, le=500.0)


class NpcCombat(StrictModel):
    max_health: float = Field(default=100.0, gt=0.0, le=100_000.0)
    damage: float = Field(default=10.0, ge=0.0, le=10_000.0)
    attack_range_m: float = Field(default=2.0, ge=0.0, le=500.0)
    attack_cooldown_s: float = Field(default=1.5, ge=0.0, le=120.0)
    #: Asset id of the projectile this NPC fires, when it fires one.
    projectile_asset_id: Optional[IdStr] = None


class NpcSpawn(StrictModel):
    #: How many exist at once at the start.
    initial_count: int = Field(default=1, ge=0, le=200)
    #: Ceiling on live instances, which the engine also enforces. More
    #: animated characters is the fastest way to miss frame rate on a Quest.
    max_concurrent: int = Field(default=8, ge=0, le=200)
    #: True when they arrive in waves rather than all being present at load.
    waves: bool = False


class NpcSpec(StrictModel):
    """One character type, keyed to the asset it is built from."""

    asset_id: IdStr = Field(
        ..., description="The AssetManifest item this NPC uses."
    )
    role: NpcRole
    #: The BuildSpec npc_roster family this type belongs to ("goblins"), and
    #: its variant physique ("tall_lean"). Variants of one family share
    #: behaviours and differ in stats.
    family: Optional[str] = Field(default=None, max_length=64)
    variant: Optional[str] = Field(default=None, max_length=32)
    #: How many of this type are PLACED in the scene at load and stay -- a
    #: companion, a shopkeeper, guards at a post. Types that arrive during
    #: play are spawned by the encounter plan instead and have 0 here.
    persistent_count: int = Field(default=0, ge=0, le=50)
    #: Short id used for target selection. Characters sharing a faction never
    #: fight each other.
    faction: str = Field(default="neutral", max_length=32)
    hostile_to: List[str] = Field(default_factory=list, max_length=8)

    movement_style: MovementStyle = MovementStyle.animated_locomotion
    behaviors: List[NpcBehavior] = Field(default_factory=list, max_length=12)

    animation_intents: List[AnimationIntent] = Field(
        default_factory=list,
        max_length=5,
        description=(
            "Clips to retarget onto this character, in priority order. Each"
            " one is billed separately, so ask for what the behaviors above"
            " actually use and nothing else. MUST be empty when"
            " movement_style is 'slide_no_animation' or when the asset is"
            " not a whole creature."
        ),
    )
    animation_fallback: AnimationFallback = AnimationFallback.nearest_preset

    locomotion: NpcLocomotion = Field(default_factory=NpcLocomotion)
    perception: NpcPerception = Field(default_factory=NpcPerception)
    combat: NpcCombat = Field(default_factory=NpcCombat)
    spawn: NpcSpawn = Field(default_factory=NpcSpawn)

    notes: str = Field(
        default="",
        max_length=2000,
        description=(
            "Anything the prompt said about this character that the fields"
            " above cannot express, written for the code-generation agent."
        ),
    )


class EncounterMode(str, Enum):
    """How the population arrives over the game."""

    #: Numbered waves; clearing the last one is victory.
    waves = "waves"
    #: Waves, then the last one repeats for as long as the player survives.
    continuous = "continuous"
    #: One set, placed when the encounter starts; no progression.
    fixed = "fixed"


class EncounterStart(str, Enum):
    """What begins the first wave."""

    on_load = "on_load"
    #: ``start_delay_s`` after the scene loads.
    delay = "delay"
    #: When the player first holds an item tagged ``start_tag`` (picking up
    #: the weapon starts the fight).
    player_grabs_tag = "player_grabs_tag"
    #: When game code calls ``ctx.encounters->start()`` (a start button).
    signal = "signal"


class EncounterGroup(StrictModel):
    """``count`` of one NPC type within a wave."""

    asset_id: IdStr
    count: int = Field(..., ge=1, le=200)


class EncounterWave(StrictModel):
    groups: List[EncounterGroup] = Field(..., min_length=1, max_length=8)
    note: str = Field(default="", max_length=300)


class EncounterPlan(StrictModel):
    """Which NPC types arrive during play, in what waves, under what caps.

    The engine's ``builtin.encounter_director`` runs this: it clones each
    type's hidden template, places it at a spawn zone out of the player's
    sight, caps how many are alive at once and counts defeats. Generated code
    never spawns NPCs itself.
    """

    mode: EncounterMode = EncounterMode.waves
    start: EncounterStart = EncounterStart.on_load
    start_delay_s: float = Field(default=3.0, ge=0.0, le=600.0)
    start_tag: str = Field(default="weapon", max_length=48)
    #: Alive at once, across every type. Animated characters are the most
    #: expensive thing a Quest renders; the engine also enforces its budget.
    max_concurrent: int = Field(default=6, ge=1, le=64)
    spawn_interval_s: float = Field(default=1.5, ge=0.0, le=60.0)
    inter_wave_delay_s: float = Field(default=8.0, ge=0.0, le=600.0)
    min_player_distance_m: float = Field(default=10.0, ge=0.0, le=200.0)
    waves: List[EncounterWave] = Field(..., min_length=1, max_length=50)
    notes: str = Field(default="", max_length=1000)


class NpcPlan(StrictModel):
    prompt_id: IdStr
    npcs: List[NpcSpec] = Field(
        default_factory=list,
        max_length=32,
        description=(
            "One entry per character type. Empty is a valid, complete answer"
            " for a game with no NPCs."
        ),
    )
    #: How the factions in this game relate, in prose, for the codegen agent.
    faction_notes: str = Field(default="", max_length=2000)
    #: NPC types that arrive during play. None when every NPC is placed at
    #: load (or there are none).
    encounters: Optional[EncounterPlan] = None

    def spawned_asset_ids(self) -> list[str]:
        """Asset ids the encounter plan spawns, in first-appearance order."""
        out: list[str] = []
        if self.encounters is None:
            return out
        for w in self.encounters.waves:
            for g in w.groups:
                if g.asset_id not in out:
                    out.append(g.asset_id)
        return out

    def rigged_asset_ids(self) -> set[str]:
        """Assets that need a skeleton: anything that plays a clip."""
        return {
            n.asset_id
            for n in self.npcs
            if n.movement_style != MovementStyle.slide_no_animation
            and n.animation_intents
        }

    def intents_for(self, asset_id: str) -> list[AnimationIntent]:
        for n in self.npcs:
            if n.asset_id != asset_id:
                continue
            if n.movement_style == MovementStyle.slide_no_animation:
                return []
            return list(n.animation_intents)
        return []
