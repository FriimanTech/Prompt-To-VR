"""The NPC plan's population compiles into the scene: one hidden template per
spawned type, companions beside the spawn, one director with per-type waves
and zones, and the mechanic plan carrying the built-ins it needs."""

from __future__ import annotations

import math

from ptv_schemas import (
    EncounterGroup,
    EncounterPlan,
    EncounterWave,
    MechanicClassification,
    MechanicPlan,
    MechanicPlanItem,
    NpcPlan,
)

from ptv_orchestrator.pipeline.encounters import (
    DIRECTOR_ID,
    FOLLOW_ID,
    MELEE_ID,
    TEMPLATE_PARK_Y,
    apply_npc_population,
    finalize_encounter_zones,
)
from ptv_orchestrator.pipeline.npc_population import reconcile_npc_mechanics


def _npc(asset_id: str, role: str, behaviors: list[str], **kw) -> dict:
    out = {
        "asset_id": asset_id,
        "role": role,
        "faction": "goblins" if role == "enemy" else "heroes",
        "hostile_to": ["player"] if role == "enemy" else [],
        "movement_style": "animated_locomotion",
        "behaviors": behaviors,
    }
    out.update(kw)
    return out


def _plan(waves: bool = True) -> NpcPlan:
    raw = {
        "prompt_id": "p",
        "npcs": [
            _npc("goblin", "enemy", ["chase", "melee_attack"],
                 locomotion={"walk_speed_mps": 1.2, "run_speed_mps": 3.0},
                 combat={"max_health": 40, "damage": 8}),
            _npc("goblin_brute", "enemy", ["chase", "melee_attack"],
                 locomotion={"walk_speed_mps": 0.9, "run_speed_mps": 2.0},
                 combat={"max_health": 120, "damage": 20}),
            _npc("lantern", "ally", ["follow_leader"], persistent_count=1),
        ],
    }
    plan = NpcPlan.model_validate(raw)
    if waves:
        plan = plan.model_copy(update={"encounters": EncounterPlan(waves=[
            EncounterWave(groups=[EncounterGroup(asset_id="goblin", count=4)]),
            EncounterWave(groups=[EncounterGroup(asset_id="goblin", count=3),
                                  EncounterGroup(asset_id="goblin_brute", count=1)]),
        ])})
    return plan


def _ent(eid: str, asset: str, pos, tags=()) -> dict:
    return {"id": eid, "mesh": {"asset_id": asset},
            "transform": {"position": list(pos), "rotation": [0, 0, 0, 1], "scale": 1.0},
            "physics": {"kind": "dynamic", "collision_shape": "capsule"},
            "mechanic_bindings": [], "tags": list(tags)}


def _doc() -> dict:
    return {
        "player": {"spawn": {"position": [0.0, 0.0, 0.0], "rotation": [0, 0, 0, 1]}},
        "entities": [
            _ent("goblin", "goblin", (0, 0, -6), ["enemy", "pooled"]),
            _ent("goblin_b", "goblin", (2, 0, -6), ["enemy", "pooled"]),
            _ent("goblin_brute", "goblin_brute", (0, 0, -9), ["enemy"]),
            _ent("lantern", "lantern", (20, 0, 20), ["npc", "ally"]),
        ],
    }


def test_population_templates_companion_and_director():
    doc = _doc()
    out = apply_npc_population(doc, _plan(), spec_yaw=0.0)
    ents = {e["id"]: e for e in doc["entities"]}

    # One template per spawned type, parked, never pooled; the extra
    # pre-placed goblin is gone (the director spawns them now).
    assert out["templates"] == {"goblin": "goblin", "goblin_brute": "goblin_brute"}
    assert "goblin_b" not in ents
    for tid in ("goblin", "goblin_brute"):
        t = ents[tid]
        assert "template" in t["tags"] and "pooled" not in t["tags"]
        assert t["transform"]["position"][1] == TEMPLATE_PARK_Y
        assert t["physics"]["kind"] == "kinematic"

    # Per-type stats reach the behaviour params.
    melee = {tid: next(b for b in ents[tid]["mechanic_bindings"] if b["id"] == MELEE_ID)
             for tid in ("goblin", "goblin_brute")}
    assert melee["goblin"]["params"]["damage"] == 8
    assert melee["goblin_brute"]["params"]["damage"] == 20
    assert melee["goblin"]["params"]["chase_speed_mps"] > melee["goblin_brute"]["params"]["chase_speed_mps"]
    assert ents["goblin_brute"]["health"]["max"] == 120

    # The companion is placed once, beside the spawn, and follows.
    lantern = ents["lantern"]
    assert math.dist(lantern["transform"]["position"], [0, 0, 0]) < 3.0
    assert any(b["id"] == FOLLOW_ID for b in lantern["mechanic_bindings"])
    assert "template" not in lantern["tags"]

    # One director, waves referencing template entity ids.
    directors = [e for e in doc["entities"]
                 if any(b.get("builtin_id") == DIRECTOR_ID for b in e["mechanic_bindings"])]
    assert len(directors) == 1
    params = directors[0]["mechanic_bindings"][0]["params"]
    assert [[g["template"] for g in w["groups"]] for w in params["waves"]] == [
        ["goblin"], ["goblin", "goblin_brute"]]


def test_generated_ai_keeps_its_npc():
    doc = _doc()
    doc["entities"][0]["mechanic_bindings"].append(
        {"id": "generated.goblin_ai", "builtin_id": None, "params": {}})
    apply_npc_population(doc, _plan(), generated_ai_ids={"generated.goblin_ai"})
    goblin = next(e for e in doc["entities"] if e["id"] == "goblin")
    assert not any(b["id"] == MELEE_ID for b in goblin["mechanic_bindings"])


def test_no_waves_means_no_director_or_templates():
    doc = _doc()
    out = apply_npc_population(doc, _plan(waves=False))
    assert out["templates"] == {}
    assert not any(e["id"] == "game_director" for e in doc["entities"])


def test_scene_bound_director_is_reused():
    doc = _doc()
    doc["entities"].append({"id": "flow", "transform": {}, "tags": [], "mechanic_bindings": [
        {"id": "encounter_director", "builtin_id": DIRECTOR_ID, "params": {}}]})
    apply_npc_population(doc, _plan())
    holders = [e["id"] for e in doc["entities"]
               if any(b.get("builtin_id") == DIRECTOR_ID for b in e["mechanic_bindings"])]
    assert holders == ["flow"]


def test_zones_from_world_or_nav_regions():
    doc = _doc()
    apply_npc_population(doc, _plan())
    doc["npc_spawn_zones"] = [{"id": "gate", "position": [0, 0, 20], "radius": 2, "tags": []}]
    assert finalize_encounter_zones(doc) == 1
    assert doc["encounters"]["zones_source"] == "world"

    doc2 = _doc()
    apply_npc_population(doc2, _plan())
    doc2["navigation"] = {"regions": [{"id": "yard", "min": [-30, 0, -30], "max": [30, 0, 30]}]}
    warnings: list[str] = []
    n = finalize_encounter_zones(doc2, warnings.append)
    assert n >= 2 and warnings
    director = next(e for e in doc2["entities"] if e["id"] == "game_director")
    for z in director["mechanic_bindings"][0]["params"]["zones"]:
        assert math.hypot(z["position"][0], z["position"][2]) >= 12.0


def test_mechanic_plan_gains_npc_builtins_once():
    plan = MechanicPlan(prompt_id="p", items=[MechanicPlanItem(
        id="npc_follow", classification=MechanicClassification.builtin,
        builtin_id="builtin.npc.follow")])
    out, notes = reconcile_npc_mechanics(plan, _plan())
    builtins = sorted(i.builtin_id for i in out.items)
    assert builtins == sorted([DIRECTOR_ID, FOLLOW_ID, MELEE_ID])
    assert len(notes) == 2
    again, notes2 = reconcile_npc_mechanics(out, _plan())
    assert len(again.items) == 3 and notes2 == []
