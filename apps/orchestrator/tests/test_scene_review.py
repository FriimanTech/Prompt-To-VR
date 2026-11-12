"""scene_review: requirement entities found by claim or by head noun, cameras
on walkable floor, and the deterministic checks (claimed asset missing,
entities nobody can see, a spawn facing a wall)."""

from __future__ import annotations

from ptv_schemas.requirements import Requirement, RequirementClaim, RequirementsLedger

from ptv_orchestrator.pipeline import stage_scene_review as sr


def _ent(eid, asset=None, pos=(0, 0, 0), tags=(), prim=None):
    e = {"id": eid, "transform": {"position": list(pos)}, "tags": list(tags),
         "mechanic_bindings": []}
    if asset:
        e["mesh"] = {"asset_id": asset, "target_scale_meters": 1.0}
    if prim:
        e["primitive"] = {"kind": "box", "size": prim}
    return e


DOC = {
    "player": {"spawn": {"position": [0, 0, 10], "rotation": [0, 0, 0, 1]}, "height_meters": 1.7},
    "navigation": {"regions": [{"id": "hall", "min": [-10, 0, -20], "max": [10, 0, 12]}]},
    "entities": [
        _ent("ground", prim=[40, 0.2, 40], tags=["ground"]),
        _ent("weapons_table", "range_table", (-4, 0, 2)),
        _ent("explosives_table", "range_table", (4, 0, 2)),
        _ent("weapons_table_pad", pos=(-4, 0, 2), prim=[1, 0.01, 1]),
        _ent("lane2_start_button", "red_button", (-6, 0, -4)),
        _ent("red_halo", pos=(-6, 0.01, -4), prim=[0.6, 0.01, 0.6]),
        _ent("m4a1_red_dot", "m4a1_red_dot", (-4, 0.8, 2)),
        _ent("goblin_template", "goblin", (0, -50, 0), tags=["template"]),
    ],
}

LEDGER = RequirementsLedger(prompt_id="p", requirements=[
    Requirement(id="req_tables", kind="placement", statement="two tables", quote="2 tables",
                subjects=["tables"]),
    Requirement(id="req_button", kind="interaction", statement="a red button", quote="red button",
                subjects=["red button"]),
    Requirement(id="req_flashbang", kind="asset", statement="a flashbang", quote="A Flashbang",
                subjects=["flashbang"]),
], claims={"req_flashbang": [RequirementClaim(stage="asset_manifest", item="flashbang")]})


def test_entities_by_claim_and_head_noun():
    groups = {r.id: [e["id"] for e in sr.entities_for(r, DOC, LEDGER)] for r in LEDGER.requirements}
    assert groups["req_tables"] == ["weapons_table", "explosives_table"]
    assert groups["req_button"] == ["lane2_start_button"]   # not the red-dot sight, not the halo
    assert groups["req_flashbang"] == []


def test_cameras_stand_on_walkable_floor():
    groups = {r.id: sr.entities_for(r, DOC, LEDGER) for r in LEDGER.requirements}
    cams = {c["id"]: c for c in sr.plan_cameras(DOC, groups)}
    assert {"spawn", "overview", "req_tables__eye", "req_tables__above"} <= set(cams)
    assert cams["spawn"]["eye"][1] == 1.6 and cams["spawn"]["target"][2] < 10
    eye = cams["req_tables__eye"]["eye"]
    assert -10 < eye[0] < 10 and -20 < eye[2] < 12   # inside the hall, not behind a wall
    assert "max_y" in cams["overview"] and "max_y" in cams["req_tables__above"]


def test_deterministic_issues():
    groups = {r.id: sr.entities_for(r, DOC, LEDGER) for r in LEDGER.requirements}
    views = {
        "spawn": {"visible": {}, "centre_entity": "wall", "centre_depth_m": 0.3},
        "req_tables__eye": {"visible": {"weapons_table": 900, "explosives_table": 700}},
        "req_button__eye": {"visible": {"lane2_start_button": 12}},
        "req_button__above": {"visible": {"lane2_start_button": 20}},
    }
    issues = {(i["requirement"], i["check"]) for i in sr.deterministic_issues(DOC, LEDGER, groups, views)}
    assert ("req_flashbang", "asset_missing") in issues
    assert ("req_button", "not_visible") in issues
    assert (None, "spawn_blocked") in issues
    assert not any(r == "req_tables" for r, _ in issues)


def test_an_extra_claimed_effect_does_not_fail_a_present_weapon():
    """The rerun's "a Laser Blaster is available" failed because name
    matching also claimed the beam fx the weapon draws at runtime."""
    doc = {**DOC, "entities": DOC["entities"] + [_ent("blue_laser_blaster", "blue_laser_blaster",
                                                      (-3, 0.9, 2))]}
    ledger = RequirementsLedger(prompt_id="p", requirements=[
        Requirement(id="req_blaster", kind="asset", statement="a laser blaster",
                    quote="Laser Blaster", subjects=["Laser Blaster"]),
    ], claims={"req_blaster": [RequirementClaim(stage="asset_manifest", item="blue_laser_blaster"),
                               RequirementClaim(stage="asset_manifest", item="solid_laser_beam_fx")]})
    groups = {r.id: sr.entities_for(r, doc, ledger) for r in ledger.requirements}
    issues = {(i["requirement"], i["check"], i["severity"])
              for i in sr.deterministic_issues(doc, ledger, groups, {})}
    assert ("req_blaster", "asset_missing", "fail") not in issues
    assert ("req_blaster", "asset_not_placed", "warn") in issues


def test_runtime_spawned_subjects_get_no_cameras():
    """Goblins spawned by the encounter director exist only as hidden
    templates parked off-map; cameras aimed there showed the vision model an
    empty canvas, which it called a missing goblin."""
    groups = {"req_goblins": [e for e in DOC["entities"] if e["id"] == "goblin_template"]}
    visible = {k: [e for e in v if sr._visible_entity(e)] for k, v in groups.items()}
    ids = {c["id"] for c in sr.plan_cameras(DOC, visible)}
    assert "req_goblins__eye" not in ids


def test_placement_subjects_are_the_thing_not_the_place():
    """"Regular-sized goblins at the front of the third lane": the lane's
    floor strips are not the goblins."""
    doc = {**DOC, "entities": DOC["entities"] + [_ent("lane3_front_strip", pos=(0, 0, -8),
                                                      prim=[6, 0.01, 2])]}
    ledger = RequirementsLedger(prompt_id="p", requirements=[
        Requirement(id="req_front", kind="placement",
                    statement="The regular-sized Goblins are positioned at the front of the third lane.",
                    quote="at the front of the lane, there should be regular-sized Goblins",
                    subjects=["front of the lane", "regular-sized Goblins", "third lane"]),
    ], claims={"req_front": [RequirementClaim(stage="npc_plan", item="goblin")]})
    ids = [e["id"] for e in sr.entities_for(ledger.requirements[0], doc, ledger)]
    assert ids == ["goblin_template"]


def test_headset_viewpoints_are_one_per_place_and_walkable():
    cams = [{"id": "spawn", "eye": [0, 1.6, 10], "target": [0, 1, 0]},
            {"id": "overview", "eye": [0, 30, 0], "target": [0, 0, 0]},
            {"id": "req_glock_asset__eye", "eye": [-4, 1.6, 6], "target": [-4, 1, 2]},
            {"id": "req_glock_on_table__eye", "eye": [-4.2, 1.6, 6.1], "target": [-4, 1, 2]},
            {"id": "req_avatar__eye", "eye": [30, 19, -1], "target": [0, 1, 0]},
            {"id": "req_roof__eye", "eye": [2, 9, 0], "target": [0, 1, 0]}]
    ids = [v["id"] for v in sr.verify_viewpoints(cams, DOC)]
    assert ids == ["spawn", "req_glock_asset__eye"]
