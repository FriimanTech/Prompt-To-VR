"""The game loop reaches the scene once, completely, and never as generated
code."""

from __future__ import annotations

from ptv_schemas import GameDesign, MechanicPlan

from ptv_orchestrator.pipeline.game_flow_plan import reconcile_game_flow_items
from ptv_orchestrator.pipeline.game_flow_scene import (
    DIRECTOR_ENTITY_ID,
    apply_game_flow,
    game_flow_violations,
)


def _design(**over) -> GameDesign:
    base = {
        "prompt_id": "p", "experience": "combat",
        "player_vitality": {"has_health": True},
        "lives": {"count": 3},
        "objectives": [{"id": "survive", "kind": "survive_time",
                        "description": "Survive 3 minutes", "duration_s": 180}],
        "victory": {"kind": "all_objectives", "objective_ids": ["survive"]},
        "failure": [{"kind": "lives_exhausted"}],
        "end_screens": [
            {"kind": "game_over", "title": "Game Over", "buttons": ["try_again", "leave_game"]},
            {"kind": "victory", "title": "You Win!", "buttons": ["play_again", "leave_game"]}],
        "hud": {"health": True, "lives": True, "timer": True},
    }
    base.update(over)
    return GameDesign.model_validate(base)


def _doc() -> dict:
    return {
        "player": {"spawn": {"position": [1, 0, 2], "rotation": [0, 0, 0, 1]}},
        "spawn_points": [{"id": "exit_gate", "position": [10, 0, 10]}],
        "entities": [{"id": "run_flow", "mechanic_bindings": [
            {"id": "survival_run_flow", "builtin_id": "builtin.game_flow", "params": {}}]}],
    }


def test_one_director_carries_the_flow_panel_and_hud() -> None:
    doc = _doc()
    params = apply_game_flow(doc, _design())
    director = next(e for e in doc["entities"] if e["id"] == DIRECTOR_ENTITY_ID)
    ids = [b["builtin_id"] for b in director["mechanic_bindings"]]
    assert ids == ["builtin.ui_panel", "builtin.game_flow", "builtin.game_hud"]
    # The stray binding the SceneSpec put elsewhere is gone: two flows would
    # each spend a life.
    other = next(e for e in doc["entities"] if e["id"] == "run_flow")
    assert not other["mechanic_bindings"]
    assert params["lives"] == 3 and params["spawn"]["position"] == [1.0, 0.0, 2.0]
    assert params["screens"]["game_over"]["buttons"] == ["try_again", "leave_game"]
    assert game_flow_violations(doc, params) == []


def test_flow_that_cannot_complete_here_is_reported() -> None:
    doc = _doc()
    design = _design(objectives=[{"id": "escape", "kind": "reach_location",
                                  "description": "Escape", "target_ids": ["helipad"]}],
                     victory={"kind": "all_objectives", "objective_ids": ["escape"]})
    problems = game_flow_violations(doc, apply_game_flow(doc, design))
    assert any("helipad" in p for p in problems)


def test_a_reader_gets_no_hud_and_no_projects_without_design_change() -> None:
    doc = _doc()
    reader = GameDesign.model_validate({"prompt_id": "p", "experience": "media",
                                        "free_play": True})
    params = apply_game_flow(doc, reader)
    director = next(e for e in doc["entities"] if e["id"] == DIRECTOR_ENTITY_ID)
    assert "builtin.game_hud" not in [b["builtin_id"] for b in director["mechanic_bindings"]]
    assert params["has_health"] is False and params["lives"] == -1
    untouched = _doc()
    assert apply_game_flow(untouched, None) is None
    assert untouched == _doc()


def _plan(items: list[dict]) -> MechanicPlan:
    return MechanicPlan.model_validate({"prompt_id": "p", "items": items})


def _task(tid: str, title: str, deps=(), desc="d") -> dict:
    return {"id": tid, "kind": "mechanic", "title": title, "description": desc,
            "interface": "IMechanic", "depends_on": list(deps)}


def test_generated_loop_items_become_builtins() -> None:
    plan = _plan([
        {"id": "survival_run_flow", "classification": "generate",
         "code_task": _task("survival_run_flow", "Survival run flow")},
        {"id": "player_health_and_damage", "classification": "generate",
         "code_task": _task("player_health_and_damage", "Player health and damage")},
        {"id": "survival_hud", "classification": "generate",
         "code_task": _task("survival_hud", "Survival HUD", ["survival_run_flow"],
                            "Shows health, ammo and the wave")},
        {"id": "blaster", "classification": "generate",
         "code_task": _task("blaster", "Plasma blaster")},
        {"id": "goblin_health_pickup", "classification": "generate",
         "code_task": _task("goblin_health_pickup", "Health pickup")},
    ])
    out, notes = reconcile_game_flow_items(plan, _design())
    by_id = {it.id: it for it in out.items}
    assert by_id["survival_run_flow"].builtin_id == "builtin.game_flow"
    assert by_id["survival_run_flow"].code_task is None
    assert by_id["player_health_and_damage"].builtin_id == "builtin.game_flow"
    # Game-specific HUD stays generated, loses the dead dependency, and is told
    # not to redraw health.
    hud = by_id["survival_hud"].code_task
    assert hud is not None and hud.depends_on == []
    assert "builtin.game_hud" in hud.description and "ctx.game" in hud.description
    assert by_id["blaster"].code_task is not None
    assert by_id["goblin_health_pickup"].code_task is not None
    assert notes
    # Without a design, nothing changes.
    same, none = reconcile_game_flow_items(plan, None)
    assert same is plan and none == []
