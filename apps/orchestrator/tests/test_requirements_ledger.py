"""The requirements ledger keeps only the prompt's own words, records who
claims each requirement, and reports exactly what a stage left unclaimed."""

from __future__ import annotations

from pathlib import Path
from types import SimpleNamespace

from ptv_schemas import RequirementKind, RequirementOutcome

from ptv_orchestrator.pipeline import requirements_ledger as rl

PROMPT = (
    "There should be 2 tables with a Glock pistol and a grenade on them. "
    "The first lane should have regular shooting range targets that the VR Player "
    "can shoot or throw grenades at to explode and destroy the targets. "
    "Lane 3 should be wider than the other lanes."
)


def _raw(*items):
    return rl._RawLedger.model_validate({"requirements": list(items)})


def _req(rid, kind, statement, quote, subjects=()):
    return {"id": rid, "kind": kind, "statement": statement, "quote": quote,
            "subjects": list(subjects)}


def _ledger():
    return rl.validate(_raw(
        _req("req_glock", "asset", "A Glock pistol exists", "a Glock pistol", ["Glock pistol"]),
        _req("req_on_table", "placement", "The Glock rests on a table",
             "2 tables with a Glock pistol", ["Glock pistol", "table"]),
        _req("req_destroy", "mechanic", "Lane-1 targets are destroyed by shots or grenades",
             "shoot or throw grenades at to explode and destroy the targets"),
        _req("req_lane3", "layout", "Lane 3 is wider", "Lane 3 should be wider than the other lanes"),
    ), PROMPT, "p")


def test_quotes_must_be_verbatim():
    ledger = rl.validate(_raw(
        _req("glock", "asset", "A Glock exists", "a Glock pistol"),
        _req("invented", "asset", "A rocket launcher exists", "a rocket launcher"),
        _req("bad_kind", "sound", "Gunshots are loud", "a Glock pistol"),
    ), PROMPT, "p")
    assert [r.id for r in ledger.requirements] == ["req_glock"]
    assert any("not in the prompt" in n for n in ledger.notes)
    assert any("unknown kind" in n for n in ledger.notes)


def test_gate_reports_only_what_the_stage_owns():
    ledger = _ledger()
    known = {r.id for r in ledger.requirements}
    plan = SimpleNamespace(items=[SimpleNamespace(id="lane_targets", satisfies=[])])
    missing = rl.gate(ledger, "mechanic_plan", rl.claims_from_mechanic_plan(plan, known))
    assert [r.id for r in missing] == ["req_destroy"]
    plan.items[0].satisfies = ["req_destroy", "not_a_requirement"]
    assert rl.gate(ledger, "mechanic_plan", rl.claims_from_mechanic_plan(plan, known)) == []
    assert ledger.claims["req_destroy"][0].item == "lane_targets"
    # A stage's re-run replaces its own claims.
    plan.items[0].satisfies = []
    assert [r.id for r in rl.gate(ledger, "mechanic_plan",
                                  rl.claims_from_mechanic_plan(plan, known))] == ["req_destroy"]


def test_assets_are_claimed_by_name_but_mechanics_are_not():
    ledger = _ledger()
    claims = rl.name_claims(ledger, "asset_manifest",
                            [("glock_pistol", "Glock 19 pistol"),
                             ("explosive_hub", "grenade throwing and explosions")])
    assert "req_glock" in claims and "req_destroy" not in claims
    assert rl.gate(ledger, "asset_manifest", claims) == []


def test_world_claims_and_report(tmp_path: Path):
    ledger = _ledger()
    known = {r.id for r in ledger.requirements}
    claims = rl.claims_from_world([{"builder_id": "range", "requirements": [
        {"id": "req_lane3", "how": "lane 3 is 18 m wide, lanes 1-2 are 14 m"}]}], known)
    assert rl.gate(ledger, "world_generation", claims) == []
    rl.record_result(ledger, "req_lane3", RequirementOutcome.passed, ["scene review: wide"],
                     "scene_review")
    rl.record_result(ledger, "req_lane3", RequirementOutcome.unverified, [], "quest")
    assert ledger.results["req_lane3"].outcome == RequirementOutcome.passed
    report = rl.write_report(tmp_path, ledger)
    text = report.read_text(encoding="utf-8")
    assert '"pass": 1' in text and "req_lane3" in text


def test_brief_marks_the_stage_s_own_requirements():
    text = rl.brief(_ledger(), "scene_spec", all_kinds=True)
    assert "req_on_table [placement, must, YOURS]" in text
    assert "req_glock [asset, must, context]" in text
    assert rl.brief(None, "scene_spec") == ""
    assert RequirementKind.layout in rl.STAGE_OWNS["world_generation"]


def test_placement_claims_from_intents_world_furniture_and_npc_zones():
    """The rerun's composer placed every item with an anchor but filled no
    `satisfies`; the gate must still see the placements."""
    from ptv_schemas import EncounterGroup, EncounterPlan, EncounterWave, NpcPlan, RequirementsLedger
    from ptv_schemas.requirements import Requirement

    def req(rid, statement, quote, subjects):
        return Requirement(id=rid, kind="placement", statement=statement, quote=quote,
                           subjects=subjects)

    ledger = RequirementsLedger(prompt_id="p", requirements=[
        req("req_glock_on_table", "The Glock Pistol is placed on one of the two tables.",
            "grab them off the tables", ["Glock Pistol", "tables"]),
        req("req_scope_on_m4", "The Red Dot Scope is mounted on the M4A1.",
            "An M4A1 with an Red Dot Scope", ["Red Dot Scope", "M4A1"]),
        req("req_tables_in_range", "Both tables are located inside the shooting range.",
            "2 tables in the shooting range", ["tables", "shooting range"]),
        req("req_goblins_front", "The regular-sized Goblins are positioned at the front of the third lane.",
            "at the front of the lane, there should be regular-sized Goblins",
            ["front of the lane", "regular-sized Goblins", "third lane"]),
        req("req_goblins_back", "The huge, bulky Goblins are positioned at the back of the third lane.",
            "at the back of the lane there should be huge, bulky Goblins",
            ["huge, bulky Goblins", "third lane"]),
        req("req_flag", "A flag hangs over the door.", "a flag over the door", ["flag"]),
    ])

    def ent(eid, asset, relation="on_surface", anchor="a"):
        return SimpleNamespace(id=eid, mesh=SimpleNamespace(asset_id=asset),
                               placement=SimpleNamespace(relation=SimpleNamespace(value=relation),
                                                         anchor=anchor, anchor_set=""))

    scene = SimpleNamespace(entities=[ent("glock_pistol", "glock_pistol"),
                                      ent("m4a1", "m4a1_red_dot"),
                                      ent("loose_crate", "crate", relation="free", anchor="")])
    npc = NpcPlan(prompt_id="p", encounters=EncounterPlan(waves=[EncounterWave(groups=[
        EncounterGroup(asset_id="goblin_standard", count=4, zone_tags=["front_band"]),
        EncounterGroup(asset_id="goblin_bulky", count=2, zone_tags=["back_band"],
                       zone_quote="at the back of the lane there should be huge, bulky Goblins"),
    ])]))
    claims = rl.placement_claims(ledger, [scene], {"equipment_table"}, npc)
    assert [c.item for c in claims["req_glock_on_table"]] == ["glock_pistol"]
    assert [c.item for c in claims["req_scope_on_m4"]] == ["m4a1"]
    assert claims["req_tables_in_range"][0].stage == "world_generation"
    assert [c.item for c in claims["req_goblins_front"]] == ["goblin_standard"]
    assert [c.item for c in claims["req_goblins_back"]] == ["goblin_bulky"]
    assert "req_flag" not in claims


def test_a_rerun_replaces_its_own_results():
    """A stage that no longer finds a failure must not leave the old one."""
    from ptv_schemas import RequirementsLedger
    from ptv_schemas.requirements import Requirement

    ledger = RequirementsLedger(prompt_id="p", requirements=[
        Requirement(id="req_a", kind="asset", statement="a grenade", quote="a grenade"),
        Requirement(id="req_b", kind="interaction", statement="press it", quote="press"),
    ])
    rl.record_result(ledger, "req_a", RequirementOutcome("fail"), ["old judge"], "scene_review")
    rl.record_result(ledger, "req_b", RequirementOutcome("pass"), ["pressed"], "quest_runtime_verify")
    rl.clear_results(ledger, "scene_review")
    assert "req_a" not in ledger.results
    assert ledger.results["req_b"].outcome == RequirementOutcome("pass")
