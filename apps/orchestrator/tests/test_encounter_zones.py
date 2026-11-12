"""Groups appear where the prompt put them: zone tags on encounter groups,
checked against the prompt, briefed to the world, required of it, compiled
into the director -- and "they all come back" is a repeating encounter."""

from __future__ import annotations

from ptv_schemas import (
    EncounterGroup,
    EncounterPlan,
    EncounterWave,
    GameDesign,
    NpcPlan,
    ZoneVisibility,
)
from ptv_schemas.game_design import ExperienceKind, Objective, ObjectiveKind

from ptv_orchestrator.pipeline.encounters import (
    apply_npc_population,
    finalize_encounter_zones,
)
from ptv_orchestrator.pipeline.game_design_rules import game_design_violations, repair_game_design
from ptv_orchestrator.pipeline.npc_population import (
    npc_plan_violations,
    placement_phrases,
    repair_npc_plan,
    zone_tags_required,
)
from ptv_orchestrator.pipeline.stage_world_generation import _zone_tag_problems, _zoned_groups

PROMPT = (
    "The third lane should be a much wider lane where, at the front of the lane, there "
    "should be regular-sized Goblins, in the middle of the lane there should be taller, "
    "lengthy Goblins, and at the back of the lane there should be huge, bulky Goblins. "
    "Once all the Goblins in the third lane have been killed, all of them should "
    "automatically respawn so the player can continue shooting them."
)


def _npc(asset_id: str) -> dict:
    return {"asset_id": asset_id, "role": "enemy", "faction": "goblins",
            "hostile_to": ["player"], "movement_style": "stationary", "behaviors": []}


def _plan(tagged: bool) -> NpcPlan:
    plan = NpcPlan.model_validate({"prompt_id": "p", "npcs": [
        _npc("goblin_regular"), _npc("goblin_tall"), _npc("goblin_bulky")]})

    def g(aid: str, tag: str, quote: str) -> EncounterGroup:
        if not tagged:
            return EncounterGroup(asset_id=aid, count=3)
        return EncounterGroup(asset_id=aid, count=3, zone_tags=[tag],
                              zone_visibility=ZoneVisibility.any, zone_quote=quote)

    return plan.model_copy(update={"encounters": EncounterPlan(waves=[EncounterWave(groups=[
        g("goblin_regular", "Front Band", "at the front of the lane"),
        g("goblin_tall", "middle_band", "in the middle of the lane"),
        g("goblin_bulky", "back_band", "at the back of the lane"),
    ])])})


def test_prompt_that_places_groups_requires_zone_tags_and_repeat():
    assert "front of the lane" in placement_phrases(PROMPT)
    problems = npc_plan_violations(_plan(tagged=False), None, PROMPT)
    assert any("zone_tags" in p for p in problems)
    assert any("repeat" in p for p in problems)
    tagged = _plan(tagged=True)
    assert tagged.encounters.waves[0].groups[0].zone_tags == ["front_band"]
    assert [p for p in npc_plan_violations(tagged, None, PROMPT) if "zone_tags" in p] == []
    repaired, notes = repair_npc_plan(tagged, None, prompt=PROMPT)
    assert repaired.encounters.repeat is True and notes
    assert zone_tags_required(repaired) == ["front_band", "middle_band", "back_band"]


def test_invented_zone_quote_is_flagged_and_dropped():
    plan = _plan(tagged=True)
    bad = plan.encounters.waves[0].groups[0].model_copy(update={"zone_quote": "near the door"})
    plan = plan.model_copy(update={"encounters": plan.encounters.model_copy(update={
        "waves": [EncounterWave(groups=[bad, *plan.encounters.waves[0].groups[1:]])]})})
    assert any("not verbatim" in p for p in npc_plan_violations(plan, None, PROMPT))
    repaired, _ = repair_npc_plan(plan, None, prompt=PROMPT)
    assert repaired.encounters.waves[0].groups[0].zone_quote == ""


def test_world_is_told_and_held_to_the_zone_tags():
    plan = _plan(tagged=True)
    zoned = _zoned_groups(plan)
    assert list(zoned) == ["front_band", "middle_band", "back_band"]
    assert zoned["front_band"] == (["goblin_regular"], "at the front of the lane")
    world = {"npc_spawn_zones": [{"id": "lane3_front", "tags": ["front_band"]},
                                 {"id": "middle_band", "tags": []}]}
    assert _zone_tag_problems(world, list(zoned)) == [
        "no npc_spawn_zone carries the tag 'back_band' the encounter spawns a group in"]


def test_director_params_carry_zone_tags_and_repeat():
    plan, _ = repair_npc_plan(_plan(tagged=True), None, prompt=PROMPT)
    doc = {"player": {"spawn": {"position": [0, 0, 0]}}, "entities": [
        {"id": a, "mesh": {"asset_id": a}, "transform": {"position": [0, 0, -8]},
         "physics": {"kind": "kinematic", "collision_shape": "capsule"},
         "mechanic_bindings": [], "tags": ["enemy"]}
        for a in ("goblin_regular", "goblin_tall", "goblin_bulky")]}
    out = apply_npc_population(doc, plan)
    params = out["director"]
    assert params["repeat"] is True
    groups = params["waves"][0]["groups"]
    assert groups[0]["zone_tags"] == ["front_band"] and groups[0]["zone_visibility"] == "any"
    doc["npc_spawn_zones"] = [{"id": "z1", "position": [0, 0, -8], "radius": 2,
                               "tags": ["front_band", "middle_band"]}]
    warnings: list[str] = []
    finalize_encounter_zones(doc, warnings.append)
    assert doc["encounters"]["missing_zone_tags"] == ["back_band"]
    assert any("back_band" in w for w in warnings)


def test_repeating_encounter_has_no_clear_waves_victory():
    plan, _ = repair_npc_plan(_plan(tagged=True), None, prompt=PROMPT)
    design = GameDesign(prompt_id="p", experience=ExperienceKind.combat, objectives=[Objective(
        id="clear_waves", kind=ObjectiveKind.clear_waves, description="Defeat every wave")])
    assert any("never complete" in p
               for p in game_design_violations(design, prompt=PROMPT, npc_plan=plan))
    fixed, notes = repair_game_design(design, npc_plan=plan)
    assert not fixed.objectives and any("repeats" in n for n in notes)
