"""Nothing draws over the player's own body: scene_compile names meshless
logic entities, and quest verification fails on foreign draws at the hands
or feet and on placeholder boxes in a generated game. The player's finger,
palm, forearm and shin colliders once drew as 0.3 m white cubes."""

from __future__ import annotations

from ptv_orchestrator.pipeline.stage_quest_verify import evaluate_body_draws
from ptv_orchestrator.pipeline.stage_scene_compile import tag_meshless_logic


def test_meshless_logic_entities_are_tagged_mechanic_host():
    doc = {"entities": [
        {"id": "mission_controller", "tags": ["mission_controller"]},
        {"id": "tactical_hud_anchor", "tags": ["hud"]},
        {"id": "player_rig", "tags": ["player_rig", "player_system"]},
        {"id": "floor", "tags": ["ground"]},
        {"id": "wall", "primitive": {"kind": "box", "size": [1, 1, 1]}, "tags": []},
        {"id": "pistol", "mesh": {"asset_id": "glock"}, "tags": ["grabbable"]},
        {"id": "bare"},
    ]}
    assert tag_meshless_logic(doc) == ["mission_controller", "tactical_hud_anchor", "bare"]
    tags = {e["id"]: e.get("tags") for e in doc["entities"]}
    assert "mechanic_host" in tags["mission_controller"] and "mechanic_host" in tags["bare"]
    assert "mechanic_host" not in tags["player_rig"] and "mechanic_host" not in tags["floor"]
    assert "mechanic_host" not in tags["wall"] and "mechanic_host" not in tags["pistol"]
    assert tag_meshless_logic(doc) == []          # idempotent


def _log(*bodies: str) -> str:
    return "\n".join(f"11-11 10:00:00.000 I/ptv (123): [info] render_body: {b}" for b in bodies)


SCENE = {"prompt_id": "p", "player": {"hands": {"source": "default"}}}


def test_clean_body_passes():
    checks = {c.name: c for c in evaluate_body_draws(_log(
        "placeholders=0 meshless_hidden=4 player_draws=0 names=-"), SCENE)}
    assert checks["nothing draws over the player's hands / body"].outcome == "pass"
    assert checks["no placeholder boxes in a generated game"].outcome == "pass"


def test_boxes_over_the_hands_fail():
    checks = {c.name: c for c in evaluate_body_draws(_log(
        "placeholders=0 meshless_hidden=4 player_draws=0 names=-",
        "placeholders=12 meshless_hidden=4 player_draws=12 names=__player_part,__player_part",
    ), SCENE)}
    body = checks["nothing draws over the player's hands / body"]
    assert body.outcome == "fail" and "__player_part" in body.detail
    assert checks["no placeholder boxes in a generated game"].outcome == "fail"


def test_no_telemetry_is_not_applicable_and_samples_skip_the_box_check():
    checks = evaluate_body_draws("nothing here", SCENE)
    assert checks[0].outcome == "not_applicable"
    sample = {c.name for c in evaluate_body_draws(
        _log("placeholders=19 meshless_hidden=0 player_draws=0 names=-"), {})}
    assert "no placeholder boxes in a generated game" not in sample
