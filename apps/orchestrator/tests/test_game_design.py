"""game_design: the loop's rules are complete, explicit beats inferred, and no
system is invented that the game does not need."""

from __future__ import annotations

import json
import threading
from pathlib import Path

from ptv_schemas import (
    ConditionKind,
    DecisionSource,
    EndButton,
    EndScreenKind,
    ExperienceKind,
    GameDesign,
    NpcPlan,
    PauseButton,
    TargetDevice,
)

from ptv_orchestrator.events import EventBus, PipelineEvent
from ptv_orchestrator.pipeline import stage_game_design as sgd
from ptv_orchestrator.pipeline.context import PipelineContext
from ptv_orchestrator.pipeline.game_design_agent import GameDesignAgentError, GameDesigner
from ptv_orchestrator.pipeline.game_design_rules import (
    fallback_design,
    game_design_violations,
    repair_game_design,
)
from ptv_orchestrator.pipeline.orchestrator import _stages_for

GOBLIN_PROMPT = ("Goblin Shooter: survive the goblin waves for 3 minutes. You have 3 lives. "
                 "When you die you respawn at the start with your blaster.")


def _goblin_plan() -> NpcPlan:
    return NpcPlan.model_validate({
        "prompt_id": "p",
        "npcs": [{"asset_id": "goblin", "role": "enemy", "faction": "goblins",
                  "hostile_to": ["player"], "movement_style": "animated_locomotion",
                  "behaviors": ["chase", "melee_attack"], "animation_intents": ["walk"],
                  "combat": {"damage": 10}}],
        "encounters": {"waves": [{"groups": [{"asset_id": "goblin", "count": 4}]}]},
    })


def _goblin_design() -> GameDesign:
    return GameDesign.model_validate({
        "prompt_id": "p", "experience": "combat",
        "player_vitality": {"has_health": True, "damage_sources": ["goblin"]},
        "lives": {"count": 3, "provenance": {"source": "explicit",
                                             "prompt_quote": "You have 3 lives"}},
        "objectives": [{"id": "survive", "kind": "survive_time", "description": "Survive",
                        "duration_s": 180,
                        "provenance": {"source": "explicit",
                                       "prompt_quote": "survive the goblin waves for 3 minutes"}}],
        "victory": {"kind": "all_objectives", "objective_ids": ["survive"]},
        "failure": [{"kind": "lives_exhausted"}],
        "end_screens": [
            {"kind": "game_over", "title": "Game Over", "buttons": ["try_again", "leave_game"]},
            {"kind": "victory", "title": "You Win!", "buttons": ["play_again", "leave_game"]},
        ],
        "hud": {"health": True, "lives": True, "timer": True},
    })


def test_stage_runs_after_npc_plan_and_before_mechanic_plan() -> None:
    for device in (TargetDevice.pcvr, TargetDevice.quest):
        order = [s.value for s, _ in _stages_for(device)]
        assert order.index("npc_plan") < order.index("game_design") < order.index("mechanic_plan")


def test_goblin_shooter_design_is_complete() -> None:
    assert game_design_violations(_goblin_design(), prompt=GOBLIN_PROMPT,
                                  npc_plan=_goblin_plan()) == []


def test_a_quote_that_is_not_in_the_prompt_is_rejected() -> None:
    d = _goblin_design()
    d.lives.provenance.prompt_quote = "you get five lives"
    problems = game_design_violations(d, prompt=GOBLIN_PROMPT, npc_plan=_goblin_plan())
    assert any("does not appear in the prompt" in p for p in problems)


def test_enemies_that_attack_force_health_and_a_game_over_screen() -> None:
    d = _goblin_design()
    d.player_vitality.has_health = False
    d.lives.count = None
    d.failure = []
    d.end_screens = []
    assert any("attack the player" in p
               for p in game_design_violations(d, prompt=GOBLIN_PROMPT, npc_plan=_goblin_plan()))
    d.lives.count = 3
    fixed, notes = repair_game_design(d, npc_plan=_goblin_plan())
    assert fixed.player_vitality.has_health
    assert fixed.screen(EndScreenKind.game_over) is not None
    assert EndButton.try_again in fixed.screen(EndScreenKind.game_over).buttons
    assert any(f.kind == ConditionKind.lives_exhausted for f in fixed.failure)
    assert game_design_violations(fixed, prompt=GOBLIN_PROMPT, npc_plan=_goblin_plan()) == []
    assert notes


def test_book_reader_gets_no_health_and_still_has_a_way_out() -> None:
    reader = GameDesign.model_validate({
        "prompt_id": "p", "experience": "media", "free_play": True,
        "pause_menu": {"enabled": False, "buttons": []},
        "omitted": ["health: nothing can hurt the reader"],
    })
    problems = game_design_violations(reader, prompt="A VR library to read a book in.",
                                      npc_plan=NpcPlan(prompt_id="p"))
    assert any("leave_game" in p for p in problems)
    fixed, _ = repair_game_design(reader, npc_plan=NpcPlan(prompt_id="p"))
    assert not fixed.player_vitality.has_health
    assert fixed.lives.count is None
    assert not fixed.end_screens
    assert not fixed.hud.health and not fixed.hud.lives
    assert PauseButton.leave_game in fixed.pause_menu.buttons
    assert game_design_violations(fixed, prompt="A VR library to read a book in.",
                                  npc_plan=NpcPlan(prompt_id="p")) == []


def test_fallback_never_invents_combat() -> None:
    class Spec:
        prompt_id = "p"
        game_type = type("G", (), {"value": "experience"})()

    calm = fallback_design(Spec(), NpcPlan(prompt_id="p"), "a calm garden")
    assert not calm.player_vitality.has_health and calm.free_play
    fight = fallback_design(Spec(), _goblin_plan(), "goblins attack")
    assert fight.experience == ExperienceKind.combat
    assert fight.player_vitality.has_health and fight.lives.count == 3
    assert fight.screen(EndScreenKind.game_over) is not None
    assert fight.screen(EndScreenKind.victory) is not None


# --- the stage itself, with a stub designer --------------------------------

class _Bus(EventBus):
    def __init__(self) -> None:
        super().__init__()
        self.events: list[PipelineEvent] = []
        self._l = threading.Lock()

    def publish(self, event: PipelineEvent) -> None:
        with self._l:
            self.events.append(event)


def _ctx(tmp_path: Path) -> PipelineContext:
    from ptv_orchestrator.settings import Settings
    from ptv_orchestrator.storage import ProjectStore
    from ptv_schemas import AssetManifest, BuildSpec

    settings = Settings(projects_dir=tmp_path / "projects", engine_dir=tmp_path / "engine",
                        samples_dir=tmp_path / "samples", repo_root=tmp_path)
    store = ProjectStore(settings.projects_dir)
    state = store.create("Goblins", GOBLIN_PROMPT)
    (store.dir_for(state.project_id) / "prompt.txt").write_text(GOBLIN_PROMPT, encoding="utf-8")
    ctx = PipelineContext(project_id=state.project_id, settings=settings, store=store, bus=_Bus())
    from types import SimpleNamespace

    dump = lambda **_: "{}"  # noqa: E731
    ctx.build_spec = SimpleNamespace(prompt_id="p", target_device=TargetDevice.quest,
                                     game_type=SimpleNamespace(value="shooter"),
                                     model_dump_json=dump)
    ctx.asset_manifest = SimpleNamespace(prompt_id="p", items=[], model_dump_json=dump)
    ctx.npc_plan = _goblin_plan()
    del BuildSpec, AssetManifest
    return ctx


def test_stage_feeds_problems_back_then_saves(tmp_path: Path) -> None:
    ctx = _ctx(tmp_path)
    broken = _goblin_design()
    broken.end_screens = []
    seen: list[str] = []

    def runner(msg: str) -> GameDesign:
        seen.append(msg)
        return broken if len(seen) == 1 else _goblin_design()

    sgd.run_with_designer(ctx, designer=GameDesigner(instructions="x", runner=runner))
    assert len(seen) == 2 and "problems" in seen[1]
    saved = json.loads((ctx.project_dir / "game_design.json").read_text(encoding="utf-8"))
    assert saved["lives"]["count"] == 3
    assert saved["lives"]["provenance"]["source"] == DecisionSource.explicit.value


def test_stage_falls_back_when_the_designer_is_unavailable(tmp_path: Path) -> None:
    ctx = _ctx(tmp_path)

    def runner(_msg: str) -> GameDesign:
        raise GameDesignAgentError("no key")

    sgd.run_with_designer(ctx, designer=GameDesigner(instructions="x", runner=runner,
                                                     max_retries=0))
    saved = GameDesign.model_validate_json(
        (ctx.project_dir / "game_design.json").read_text(encoding="utf-8"))
    assert saved.player_vitality.has_health
    assert saved.screen(EndScreenKind.game_over) is not None
