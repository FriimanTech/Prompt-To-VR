"""Deterministic rules for a GameDesign: validate, repair, and fall back.

The designer agent decides what this game's loop IS. This module decides
whether that decision is COMPLETE -- that every way the game can end has a
screen, every screen has a way out, a respawn has lives to spend, an objective
names things that exist -- and repairs what the agent left broken, so the
engine never receives a flow with a dead end in it. Nothing here invents a
system the game does not need: repairs only ever complete what the design
already chose (a game that can kill the player gets a Game Over screen; a
book reader is not given health).
"""

from __future__ import annotations

import re
from typing import Iterable, Optional

from ptv_schemas import (
    ConditionKind,
    DecisionSource,
    EndButton,
    EndCondition,
    EndScreen,
    EndScreenKind,
    ExperienceKind,
    GameDesign,
    HudSpec,
    Lives,
    Objective,
    ObjectiveKind,
    PauseButton,
    PlayerVitality,
    Provenance,
)

_ATTACKS = {"melee_attack", "ranged_attack", "contact_damage"}


def _norm(text: str) -> str:
    return re.sub(r"\s+", " ", text or "").strip().lower()


def hostile_attackers(npc_plan) -> list[str]:
    """NPC asset ids that can actually hurt the player."""
    out: list[str] = []
    for n in getattr(npc_plan, "npcs", None) or []:
        behaviours = {getattr(b, "value", str(b)) for b in (n.behaviors or [])}
        if "player" in (n.hostile_to or []) and behaviours & _ATTACKS and \
                float(n.combat.damage) > 0.0:
            out.append(n.asset_id)
    return out


def _all_provenances(design: GameDesign) -> Iterable[tuple[str, Provenance]]:
    yield "player_vitality", design.player_vitality.provenance
    yield "lives", design.lives.provenance
    yield "on_life_lost", design.on_life_lost.provenance
    yield "victory", design.victory.provenance
    for i, f in enumerate(design.failure):
        yield f"failure[{i}]", f.provenance
    for o in design.objectives:
        yield f"objective '{o.id}'", o.provenance


def _ends(design: GameDesign) -> tuple[bool, bool, bool]:
    """(can lose, can win, can complete) as the design stands."""
    lose = design.can_die() and design.lives.count is not None
    lose = lose or any(f.kind in (ConditionKind.timer_expired, ConditionKind.custom)
                       for f in design.failure)
    win_kind = design.victory.kind != ConditionKind.none
    combat_like = design.experience in (ExperienceKind.combat, ExperienceKind.score_attack,
                                        ExperienceKind.puzzle)
    return lose, win_kind and combat_like, win_kind and not combat_like


def game_design_violations(design: GameDesign, *, prompt: str, npc_plan=None,
                           manifest=None, world_markers: Optional[set[str]] = None) -> list[str]:
    """Every problem, all at once, worded so the designer can fix it."""
    problems: list[str] = []
    norm_prompt = _norm(prompt)

    for where, prov in _all_provenances(design):
        if prov.source == DecisionSource.explicit:
            quote = _norm(prov.prompt_quote)
            if not quote:
                problems.append(f"{where} is marked explicit but has no prompt_quote")
            elif quote not in norm_prompt:
                problems.append(
                    f"{where}: prompt_quote {prov.prompt_quote!r} does not appear in "
                    "the prompt; quote it verbatim or mark the decision inferred")

    attackers = hostile_attackers(npc_plan)
    pv = design.player_vitality
    if attackers and not pv.has_health and not pv.invulnerable:
        problems.append(
            f"NPCs {attackers} attack the player but the player has no health; "
            "give the player health, or set invulnerable if the prompt says the "
            "player cannot be hurt")
    if not pv.has_health and design.lives.count is not None:
        problems.append("lives are set but the player has no health to lose them with")
    if any(f.kind == ConditionKind.lives_exhausted for f in design.failure) and \
            not design.can_die():
        problems.append("failure 'lives_exhausted' needs a player that can die")

    ids = {o.id for o in design.objectives}
    npc_ids = {n.asset_id for n in (getattr(npc_plan, "npcs", None) or [])}
    asset_ids = {i.id for i in (getattr(manifest, "items", None) or [])}
    for o in design.objectives:
        if o.kind == ObjectiveKind.survive_time and not o.duration_s:
            problems.append(f"objective '{o.id}' (survive_time) needs duration_s")
        if o.kind == ObjectiveKind.defeat_count:
            if not o.count:
                problems.append(f"objective '{o.id}' (defeat_count) needs count")
            bad = [t for t in o.target_ids if t not in npc_ids]
            if bad:
                problems.append(f"objective '{o.id}' targets {bad}, which are not NPC "
                                f"types in the NPC plan ({sorted(npc_ids)})")
        if o.kind == ObjectiveKind.clear_waves and \
                getattr(npc_plan, "encounters", None) is None:
            problems.append(f"objective '{o.id}' (clear_waves) but the NPC plan has "
                            "no encounter waves")
        if o.kind == ObjectiveKind.collect:
            bad = [t for t in o.target_ids if t not in asset_ids]
            if not o.target_ids or bad:
                problems.append(f"objective '{o.id}' (collect) must name manifest "
                                f"assets; unknown: {bad or 'none named'}")
        if o.kind == ObjectiveKind.reach_location:
            if not o.target_ids:
                problems.append(f"objective '{o.id}' (reach_location) must name the "
                                "spawn point the world builder declares there")
            elif world_markers is not None:
                missing = [t for t in o.target_ids if t not in world_markers]
                if missing:
                    problems.append(f"objective '{o.id}' needs world spawn points "
                                    f"{missing}, which the world does not declare")
    for cond in [design.victory, *design.failure]:
        unknown = [i for i in cond.objective_ids if i not in ids]
        if unknown:
            problems.append(f"{cond.kind.value} names unknown objectives {unknown}")
        if cond.kind in (ConditionKind.all_objectives, ConditionKind.any_objective) \
                and not (cond.objective_ids or ids):
            problems.append(f"{cond.kind.value} but there are no objectives")

    lose, win, complete = _ends(design)
    needed = {EndScreenKind.game_over: lose, EndScreenKind.victory: win,
              EndScreenKind.completed: complete}
    for kind, want in needed.items():
        if want and design.screen(kind) is None:
            problems.append(f"the game can reach '{kind.value}' but has no "
                            f"{kind.value} end screen")
    for s in design.end_screens:
        if not s.buttons:
            problems.append(f"end screen '{s.kind.value}' has no buttons; the "
                            "player would be stuck on it")
        if s.kind == EndScreenKind.game_over and EndButton.try_again not in s.buttons:
            problems.append("the game_over screen must offer try_again")
    leave = any(EndButton.leave_game in s.buttons for s in design.end_screens) or (
        design.pause_menu.enabled and PauseButton.leave_game in design.pause_menu.buttons)
    if not leave:
        problems.append("nothing offers leave_game: add it to the pause menu or an "
                        "end screen, or the player can only quit from the OS")
    if not (lose or win or complete) and not design.free_play:
        problems.append("the game has no way to end; set free_play if that is the "
                        "intent (a sandbox, a gallery)")
    return problems


def repair_game_design(design: GameDesign, *, npc_plan=None) -> tuple[GameDesign, list[str]]:
    """Complete what the design chose, deterministically. Returns notes."""
    d = design.model_copy(deep=True)
    notes: list[str] = []

    for where, prov in _all_provenances(d):
        if prov.source == DecisionSource.explicit and not prov.prompt_quote.strip():
            prov.source = DecisionSource.inferred
            notes.append(f"{where}: explicit without a quote; treated as inferred")

    attackers = hostile_attackers(npc_plan)
    if attackers and not d.player_vitality.has_health and not d.player_vitality.invulnerable:
        d.player_vitality.has_health = True
        d.player_vitality.damage_sources = sorted(set(d.player_vitality.damage_sources)
                                                  | set(attackers))
        notes.append("player given health: NPCs attack the player")
    if not d.player_vitality.has_health:
        if d.lives.count is not None:
            d.lives.count = None
            notes.append("lives removed: the player has no health")
        d.failure = [f for f in d.failure if f.kind != ConditionKind.lives_exhausted]
    elif d.lives.count is not None and not any(
            f.kind == ConditionKind.lives_exhausted for f in d.failure):
        d.failure.append(EndCondition(kind=ConditionKind.lives_exhausted))
        notes.append("added failure: lives exhausted")

    ids = {o.id for o in d.objectives}
    for o in d.objectives:
        if o.kind == ObjectiveKind.survive_time and not o.duration_s:
            o.duration_s = 180.0
            notes.append(f"objective '{o.id}': duration defaulted to 180 s")
        if o.kind == ObjectiveKind.defeat_count and not o.count:
            o.count = 10
            notes.append(f"objective '{o.id}': count defaulted to 10")
    for cond in [d.victory, *d.failure]:
        cond.objective_ids = [i for i in cond.objective_ids if i in ids]
    if d.victory.kind in (ConditionKind.all_objectives, ConditionKind.any_objective) \
            and not ids:
        d.victory.kind = ConditionKind.none

    lose, win, complete = _ends(d)
    if lose and d.screen(EndScreenKind.game_over) is None:
        d.end_screens.append(EndScreen(kind=EndScreenKind.game_over, title="Game Over",
                                       buttons=[EndButton.try_again, EndButton.leave_game]))
        notes.append("added a Game Over screen")
    if win and d.screen(EndScreenKind.victory) is None:
        d.end_screens.append(EndScreen(kind=EndScreenKind.victory, title="You Win!",
                                       buttons=[EndButton.play_again, EndButton.leave_game]))
        notes.append("added a victory screen")
    if complete and d.screen(EndScreenKind.completed) is None:
        d.end_screens.append(EndScreen(kind=EndScreenKind.completed, title="The End",
                                       buttons=[EndButton.play_again, EndButton.leave_game]))
        notes.append("added a completion screen")
    for s in d.end_screens:
        if s.kind == EndScreenKind.game_over and EndButton.try_again not in s.buttons:
            s.buttons.insert(0, EndButton.try_again)
        if not s.buttons:
            s.buttons = [EndButton.play_again if s.kind != EndScreenKind.game_over
                         else EndButton.try_again, EndButton.leave_game]
    leave = any(EndButton.leave_game in s.buttons for s in d.end_screens) or (
        d.pause_menu.enabled and PauseButton.leave_game in d.pause_menu.buttons)
    if not leave:
        d.pause_menu.enabled = True
        if PauseButton.leave_game not in d.pause_menu.buttons:
            d.pause_menu.buttons.append(PauseButton.leave_game)
        notes.append("added Leave Game to the pause menu")
    if not (lose or win or complete):
        d.free_play = True

    # The HUD shows exactly the systems that exist, nothing more.
    d.hud.health = d.hud.health and d.player_vitality.has_health
    d.hud.lives = d.hud.lives and d.lives.count is not None
    if d.player_vitality.has_health and not d.hud.health:
        d.hud.health = True
    if d.lives.count is not None and d.lives.count > 1 and not d.hud.lives:
        d.hud.lives = True
    if any(o.kind == ObjectiveKind.survive_time for o in d.objectives):
        d.hud.timer = True
    return d, notes


def fallback_design(build_spec, npc_plan, prompt: str) -> GameDesign:
    """What to run with when the designer is unavailable.

    Conservative and derived ONLY from structure the earlier stages already
    decided: a game whose NPCs attack the player can kill the player and ends
    on Game Over; one with waves can be won by clearing them; anything else is
    free play with a way to leave. It never adds a system the plan does not
    already imply.
    """
    attackers = hostile_attackers(npc_plan)
    game_type = getattr(getattr(build_spec, "game_type", None), "value", "experience")
    experience = {
        "shooter": ExperienceKind.combat, "arcade": ExperienceKind.score_attack,
        "puzzle": ExperienceKind.puzzle, "adventure": ExperienceKind.exploration,
        "narrative": ExperienceKind.narrative, "sandbox": ExperienceKind.sandbox,
        "simulation": ExperienceKind.simulation,
    }.get(game_type, ExperienceKind.exploration)
    if attackers:
        experience = ExperienceKind.combat
    why = Provenance(rationale="fallback: derived from the NPC plan, no designer run")
    design = GameDesign(
        prompt_id=getattr(build_spec, "prompt_id", "game"),
        experience=experience,
        summary="Fallback design (the game designer did not run).",
        player_vitality=PlayerVitality(has_health=bool(attackers),
                                       damage_sources=attackers, provenance=why),
        lives=Lives(count=3 if attackers else None, provenance=why),
        hud=HudSpec(health=bool(attackers), lives=bool(attackers)),
    )
    if attackers and getattr(npc_plan, "encounters", None) is not None:
        design.objectives.append(Objective(id="clear_waves", kind=ObjectiveKind.clear_waves,
                                           description="Defeat every wave", provenance=why))
        design.victory = EndCondition(kind=ConditionKind.all_objectives,
                                      objective_ids=["clear_waves"], provenance=why)
        design.hud.objective = True
    repaired, _ = repair_game_design(design, npc_plan=npc_plan)
    return repaired
