"""The interactions pass: what the scripted player did, read from the engine's
``interact:`` lines, judged per prop / button / challenge / explosive, and
folded into the requirements ledger."""

from __future__ import annotations

from ptv_schemas.requirements import Requirement, RequirementClaim, RequirementsLedger

from ptv_orchestrator.pipeline.stage_quest_verify import (
    evaluate_interactions,
    interaction_requirement_results,
    interactions_capture_seconds,
    interactions_wanted,
    parse_interactions,
)


def _ent(eid, *bindings, kind="dynamic", params=None):
    return {"id": eid, "physics": {"kind": kind}, "tags": [],
            "mechanic_bindings": [{"id": b, "builtin_id": b, "params": dict(params or {})}
                                  for b in bindings]}


SCENE = {"entities": [
    _ent("glock_on_table", "builtin.grab", "builtin.weapon"),
    _ent("grenade_1", "builtin.grab", "builtin.explosive", "builtin.throw"),
    _ent("red_button", "builtin.press_button", kind="static", params={"signal": "lane2.start"}),
    _ent("lane2_host", "builtin.target_challenge", kind=None,
         params={"id": "lane2", "start_signal": "lane2.start"}),
    _ent("table", kind="static"),
]}

LOG = "\n".join("11-10 12:00:00.000 I/ptv: interact: " + line for line in [
    "begin props=2 buttons=1 challenges=1 explosives=1 viewpoints=1",
    "settle e=glock_on_table moved_cm=0.10 height_m=0.76",
    "settle e=grenade_1 moved_cm=4.50 height_m=0.76",
    "walk e=glock_on_table moved_cm=0.00 height_m=0.76",
    "walk e=grenade_1 moved_cm=2.00 height_m=0.76",
    "grab e=glock_on_table held=1 pushed_cm=0.20 lifted_cm=24.0 height_m=0.76",
    "grab e=grenade_1 held=0 pushed_cm=9.00 lifted_cm=0.0 height_m=0.76",
    "press e=red_button signal=lane2.start by=poke tip=1",
    "challenge id=lane2 signal=lane2.start pressed=1 state=running active=3 time_left_drop=1.50",
    "explosive e=grenade_1 held=1 detonated=1 destroyed=2",
    "viewpoint id=req_tables__eye index=0",
    "done",
])


def _outcomes():
    checks = evaluate_interactions(parse_interactions(LOG), SCENE)
    return {(c.subject, c.name): c.outcome.value for c in checks}


def test_wanted_and_budget():
    assert interactions_wanted(SCENE) and not interactions_wanted({"entities": [_ent("t")]})
    assert 30 < interactions_capture_seconds(SCENE) < 420


def test_each_kind_is_judged():
    o = _outcomes()
    assert o[("", "interactions pass ran to the end")] == "pass"
    assert o[("glock_on_table", "stays put when left alone")] == "pass"
    assert o[("grenade_1", "stays put when left alone")] == "fail"
    assert o[("grenade_1", "not pushed by the player's body")] == "fail"
    assert o[("glock_on_table", "can be picked up")] == "pass"
    assert o[("grenade_1", "not knocked away by a reaching hand")] == "fail"
    assert o[("red_button", "presses")] == "pass"
    assert o[("lane2", "challenge starts")] == "pass"
    assert o[("lane2", "challenge spawns targets")] == "pass"
    assert o[("lane2", "challenge clock runs")] == "pass"
    assert o[("grenade_1", "explodes when armed and thrown")] == "pass"


def test_low_props_are_not_failed_for_meeting_the_legs():
    log = LOG.replace("walk e=grenade_1 moved_cm=2.00 height_m=0.76",
                      "walk e=grenade_1 moved_cm=6.00 height_m=0.05")
    checks = evaluate_interactions(parse_interactions(log), SCENE)
    walk = next(c for c in checks if c.subject == "grenade_1"
                and c.name == "not pushed by the player's body")
    assert walk.outcome.value == "not_applicable"


def test_zone_band_check():
    from ptv_schemas import EncounterGroup, EncounterPlan, EncounterWave, NpcPlan

    from ptv_orchestrator.pipeline.stage_quest_verify import zone_band_check

    plan = NpcPlan(prompt_id="p", encounters=EncounterPlan(waves=[EncounterWave(groups=[
        EncounterGroup(asset_id="goblin", count=2, zone_tags=["front_band"])])]))
    scene = {"entities": [{"id": "dir", "mechanic_bindings": [{
        "builtin_id": "builtin.encounter_director", "params": {"zones": [
            {"id": "z1", "position": [0, 0, -8], "radius": 2.0, "tags": ["front_band"]},
            {"id": "z2", "position": [0, 0, -24], "radius": 2.0, "tags": ["back_band"]}]}}]}]}
    spawns = [{"e": "1"}, {"e": "2"}]
    status = {"1": [{"_pos": (1.0, 0.0, -8.5)}], "2": [{"_pos": (0.0, 0.0, -24.0)}]}
    c = zone_band_check("goblin", spawns, status, plan, scene)
    assert c.outcome.value == "fail" and "1/2" in c.detail
    status["2"] = [{"_pos": (-1.0, 0.0, -7.0)}]
    assert zone_band_check("goblin", spawns, status, plan, scene).outcome.value == "pass"
    assert zone_band_check("ogre", spawns, status, plan, scene) is None


def test_requirement_results():
    ledger = RequirementsLedger(prompt_id="p", requirements=[
        Requirement(id="req_button", kind="interaction", statement="press a red button",
                    quote="press a red button", subjects=["red button"]),
        Requirement(id="req_grenade", kind="interaction", statement="throw grenades",
                    quote="activated and thrown", subjects=["grenade"]),
        Requirement(id="req_tables", kind="placement", statement="two tables", quote="2 tables",
                    subjects=["tables"]),
    ], claims={"req_grenade": [RequirementClaim(stage="scene_spec", item="grenade_1")]})
    res = interaction_requirement_results(
        parse_interactions(LOG), SCENE, ledger,
        [{"viewpoint": "req_tables__eye", "file": "views/view_01.jpg"}])
    assert res["req_button"][0] == "pass"
    assert res["req_grenade"][0] == "pass"   # grab failed but the throw held it and exploded
    assert res["req_tables"] == ("unverified", ["headset screenshot: views/view_01.jpg"])
