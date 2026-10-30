"""GameDesign: the rules of the game loop, decided once from the prompt.

What can hurt the player, what happens when they die, how many lives they
have, what they are trying to do, how the game ends, what screen they see when
it does, and what every button on that screen does. None of that lived
anywhere before: each generated game invented its own run flow, and the
humans-vs-aliens APK reached zero health with no defined answer -- the
director paused, nothing appeared, and the player was stuck.

Every decision records WHERE it came from. ``explicit`` means the prompt said
so and ``prompt_quote`` holds the words (checked verbatim against the prompt);
``inferred`` means the designer judged it from what kind of game this is.
Explicit decisions always win, and no system is invented that the game does
not need: a book reader has no health, no lives and no Game Over screen.

The engine's ``builtin.game_flow`` runs this document as data. Generated code
never re-implements lives, respawn, restart or the end screens; a game-specific
objective reports its progress through ``ctx.game``.
"""

from __future__ import annotations

from enum import Enum
from typing import List, Optional

from pydantic import Field

from .common import IdStr, StrictModel


class DecisionSource(str, Enum):
    explicit = "explicit"
    inferred = "inferred"


class Provenance(StrictModel):
    source: DecisionSource = DecisionSource.inferred
    #: The prompt's own words when ``source`` is explicit. Must appear in the
    #: prompt verbatim (whitespace and case aside).
    prompt_quote: str = Field(default="", max_length=400)
    #: Why, in one sentence, when inferred.
    rationale: str = Field(default="", max_length=1000)


class ExperienceKind(str, Enum):
    """What kind of experience this is -- decides which systems it needs."""

    combat = "combat"
    puzzle = "puzzle"
    exploration = "exploration"
    narrative = "narrative"
    sandbox = "sandbox"
    score_attack = "score_attack"
    simulation = "simulation"
    #: Reading, watching, listening: no challenge, no failure.
    media = "media"


class PlayerVitality(StrictModel):
    has_health: bool = False
    max_health: float = Field(default=100.0, gt=0.0, le=100_000.0)
    #: What can hurt the player: NPC asset ids, or ``hazard`` / ``fall``.
    damage_sources: List[str] = Field(default_factory=list, max_length=32)
    regen_per_s: float = Field(default=0.0, ge=0.0, le=1000.0)
    #: The prompt says the player cannot be hurt (a training mode).
    invulnerable: bool = False
    provenance: Provenance = Field(default_factory=Provenance)


class Lives(StrictModel):
    #: ``None`` = unlimited (respawn forever). 1 = one life, no respawn.
    count: Optional[int] = Field(default=None, ge=1, le=99)
    provenance: Provenance = Field(default_factory=Provenance)


class RespawnPoint(str, Enum):
    spawn = "spawn"
    checkpoint = "checkpoint"
    last_safe = "last_safe"


class EnemyPolicy(str, Enum):
    """What happens to enemies when the player loses a life."""

    none = "none"
    clear_nearby = "clear_nearby"
    reset_wave = "reset_wave"
    reset_all = "reset_all"


class OnLifeLost(StrictModel):
    respawn_at: RespawnPoint = RespawnPoint.spawn
    delay_s: float = Field(default=2.0, ge=0.0, le=30.0)
    restore_health: bool = True
    keep_loadout: bool = True
    invulnerable_s: float = Field(default=3.0, ge=0.0, le=30.0)
    enemy_policy: EnemyPolicy = EnemyPolicy.clear_nearby
    clear_radius_m: float = Field(default=6.0, ge=0.0, le=200.0)
    provenance: Provenance = Field(default_factory=Provenance)


class ObjectiveKind(str, Enum):
    survive_time = "survive_time"
    defeat_count = "defeat_count"
    clear_waves = "clear_waves"
    reach_location = "reach_location"
    collect = "collect"
    interact = "interact"
    #: Tracked by generated code through ``ctx.game->report_progress``.
    custom = "custom"


class Objective(StrictModel):
    id: IdStr
    kind: ObjectiveKind
    description: str = Field(..., max_length=300)
    #: survive_time. 0 / null = not timed.
    duration_s: Optional[float] = Field(default=None, ge=0.0, le=36_000.0)
    #: defeat_count / collect / custom progress target. 0 / null = unset.
    count: Optional[int] = Field(default=None, ge=0, le=100_000)
    #: defeat_count: NPC asset ids (empty = any hostile). collect: asset ids.
    #: reach_location: the named spawn point the world builder declares.
    #: interact: interactive ids.
    target_ids: List[str] = Field(default_factory=list, max_length=32)
    #: Radius around a reach_location marker that counts as arrived.
    #: 0 = the default 2 m (other kinds ignore it).
    radius_m: float = Field(default=2.0, ge=0.0, le=100.0)
    provenance: Provenance = Field(default_factory=Provenance)


class ConditionKind(str, Enum):
    all_objectives = "all_objectives"
    any_objective = "any_objective"
    lives_exhausted = "lives_exhausted"
    #: A survive_time objective whose clock running out LOSES (escape before).
    timer_expired = "timer_expired"
    #: ``ctx.game->fail(reason)`` from generated code.
    custom = "custom"
    none = "none"


class EndCondition(StrictModel):
    kind: ConditionKind = ConditionKind.none
    objective_ids: List[str] = Field(default_factory=list, max_length=32)
    provenance: Provenance = Field(default_factory=Provenance)


class EndScreenKind(str, Enum):
    victory = "victory"
    game_over = "game_over"
    #: The non-combat ending: the book is finished, the tour is over.
    completed = "completed"


class EndButton(str, Enum):
    #: After a loss: restart fresh.
    try_again = "try_again"
    #: After a win or completion: restart fresh.
    play_again = "play_again"
    #: Close the screen and keep playing / exploring.
    keep_playing = "keep_playing"
    leave_game = "leave_game"


class EndScreen(StrictModel):
    kind: EndScreenKind
    title: str = Field(..., max_length=60)
    subtitle: str = Field(default="", max_length=160)
    buttons: List[EndButton] = Field(default_factory=list, max_length=4)
    #: Time survived / score / enemies defeated under the title.
    show_stats: bool = True


class PauseButton(str, Enum):
    resume = "resume"
    restart = "restart"
    leave_game = "leave_game"


class PauseMenu(StrictModel):
    enabled: bool = True
    buttons: List[PauseButton] = Field(
        default_factory=lambda: [PauseButton.resume, PauseButton.restart,
                                 PauseButton.leave_game],
        max_length=4,
    )


class ResetPolicy(StrictModel):
    """What a fresh restart (Try Again / Play Again) puts back."""

    lives: bool = True
    health: bool = True
    timer: bool = True
    score: bool = True
    encounters: bool = True
    player_position: bool = True
    loadout: bool = True
    #: Doors, levers, picked-up items back to how the world started.
    world_state: bool = True


class HudSpec(StrictModel):
    health: bool = False
    lives: bool = False
    timer: bool = False
    score: bool = False
    objective: bool = False


class GameDesign(StrictModel):
    prompt_id: IdStr
    experience: ExperienceKind
    summary: str = Field(default="", max_length=600)
    player_vitality: PlayerVitality = Field(default_factory=PlayerVitality)
    lives: Lives = Field(default_factory=Lives)
    on_life_lost: OnLifeLost = Field(default_factory=OnLifeLost)
    objectives: List[Objective] = Field(default_factory=list, max_length=16)
    victory: EndCondition = Field(default_factory=EndCondition)
    failure: List[EndCondition] = Field(default_factory=list, max_length=8)
    end_screens: List[EndScreen] = Field(default_factory=list, max_length=3)
    pause_menu: PauseMenu = Field(default_factory=PauseMenu)
    reset_policy: ResetPolicy = Field(default_factory=ResetPolicy)
    hud: HudSpec = Field(default_factory=HudSpec)
    #: No ending at all (a sandbox, a gallery): play until Leave Game.
    free_play: bool = False
    #: Each system the designer chose NOT to include, and why ("no health:
    #: nothing in a book reader can hurt the reader").
    omitted: List[str] = Field(default_factory=list, max_length=16)

    def screen(self, kind: EndScreenKind) -> Optional[EndScreen]:
        return next((s for s in self.end_screens if s.kind == kind), None)

    def can_die(self) -> bool:
        return self.player_vitality.has_health and not self.player_vitality.invulnerable
