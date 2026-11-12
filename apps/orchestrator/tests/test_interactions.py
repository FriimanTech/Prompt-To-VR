"""Buttons, displays, challenges, destructibles and explosives run on their
built-ins: the plan is reconciled onto them and the scene binds them from
the design, wired together through signals and vars."""

from __future__ import annotations

from ptv_schemas import (
    DestructibleSpec,
    ExplosiveSpec,
    InteractionDesign,
    ItemInteraction,
    ItemUsage,
    MechanicClassification,
    MechanicPlan,
    MechanicPlanItem,
    PressSpec,
)
from ptv_schemas.code_task import CodeTask

from ptv_orchestrator.pipeline.interaction_plan import (
    DESTRUCTIBLE_ID,
    PRESS_BUTTON_ID,
    TARGET_CHALLENGE_ID,
    WORLD_DISPLAY_ID,
    reconcile_interaction_items,
)
from ptv_orchestrator.pipeline.interaction_scene import apply_interactions
from ptv_orchestrator.pipeline.stage_scene_compile import bind_affordances
from ptv_orchestrator.pipeline.weapon_plan import reconcile_weapon_items


def _design() -> InteractionDesign:
    return InteractionDesign(prompt_id="p", items=[
        ItemInteraction(asset_id="red_button", usage=ItemUsage.pressable,
                        press=PressSpec(signal="lane2.start")),
        ItemInteraction(asset_id="challenge_hud", usage=ItemUsage.display),
        ItemInteraction(asset_id="range_target", usage=ItemUsage.static_prop,
                        destructible=DestructibleSpec(max_health=40, respawn_after_s=5)),
        ItemInteraction(asset_id="frag_grenade", usage=ItemUsage.thrown,
                        explosive=ExplosiveSpec(fuse_s=3.5)),
    ])


def _task(tid: str, title: str, desc: str) -> CodeTask:
    return CodeTask.model_validate({
        "id": tid, "title": title, "description": desc, "kind": "mechanic",
        "interface": "IMechanic", "depends_on": []})


def test_plan_reconcile_maps_onto_the_built_ins():
    plan = MechanicPlan(prompt_id="p", items=[
        MechanicPlanItem(id="timed_target_challenge", classification=MechanicClassification.generate,
                         code_task=_task("timed_target_challenge", "Timed target challenge",
                                         "A 20-second challenge that spawns targets and counts hits")),
        MechanicPlanItem(id="timed_challenge_hud", classification=MechanicClassification.builtin_config,
                         builtin_id="builtin.game_hud", config={"note": "challenge_hud panel"}),
    ])
    out, notes = reconcile_interaction_items(plan, _design())
    by_id = {i.id: i for i in out.items}
    assert by_id["timed_target_challenge"].builtin_id == TARGET_CHALLENGE_ID
    assert by_id["timed_target_challenge"].config["duration_s"] == 20.0
    assert by_id["timed_target_challenge"].code_task is None
    builtins = {i.builtin_id for i in out.items}
    assert {PRESS_BUTTON_ID, WORLD_DISPLAY_ID, DESTRUCTIBLE_ID} <= builtins
    assert notes
    again, notes2 = reconcile_interaction_items(out, _design())
    assert notes2 == [] and len(again.items) == len(out.items)


def _ent(eid, asset, tags=(), placement=None, physics=None):
    e = {"id": eid, "mesh": {"asset_id": asset}, "transform": {"position": [0, 0, 0]},
         "mechanic_bindings": [], "tags": list(tags)}
    if placement:
        e["placement"] = placement
    if physics:
        e["physics"] = physics
    return e


def test_scene_binding_wires_button_challenge_and_display():
    doc = {"entities": [
        _ent("lane2_button", "red_button"),
        _ent("lane2_hud", "challenge_hud"),
        _ent("lane1_target_01", "range_target"),
        _ent("frag_1", "frag_grenade"),
        _ent("lane2_target_template", "range_target", tags=["template"],
             placement={"relation": "hang_from", "anchor_set": "lane2_hang_points"},
             physics={"kind": "dynamic"}),
    ]}
    plan = MechanicPlan(prompt_id="p", items=[MechanicPlanItem(
        id="lane2_challenge", classification=MechanicClassification.builtin_config,
        builtin_id=TARGET_CHALLENGE_ID, config={"duration_s": 20})])
    summary = apply_interactions(doc, _design(), plan)
    ents = {e["id"]: e for e in doc["entities"]}
    press = next(b for b in ents["lane2_button"]["mechanic_bindings"] if b["id"] == PRESS_BUTTON_ID)
    assert press["params"]["signal"] == "lane2.start"
    host = next(e for e in doc["entities"] if e["id"].startswith("ptv_challenge_"))
    ch = host["mechanic_bindings"][0]["params"]
    assert ch["start_signal"] == "lane2.start"
    assert ch["anchor_set"] == "lane2_hang_points"
    assert ch["templates"] == ["lane2_target_template"]
    assert ents["lane2_target_template"]["physics"]["kind"] == "kinematic"
    disp = next(b for b in ents["lane2_hud"]["mechanic_bindings"] if b["id"] == WORLD_DISPLAY_ID)
    assert "{lane2_challenge.time_left:0.0}" in disp["params"]["lines"][0]
    assert ents["lane1_target_01"]["health"]["max"] == 40
    grenade = ents["frag_1"]
    assert {"builtin.explosive", "builtin.grab", "builtin.throw"} <= {
        b["id"] for b in grenade["mechanic_bindings"]}
    assert grenade["physics"]["kind"] == "dynamic" and "grabbable" in grenade["tags"]
    assert summary["challenges"] and summary["press_buttons"]


def test_generated_grenade_hub_becomes_builtin_explosive():
    plan = MechanicPlan(prompt_id="p", items=[
        MechanicPlanItem(id="grenade_activation_throw", classification=MechanicClassification.generate,
                         code_task=_task("grenade_activation_throw", "Grenade activation and throwing",
                                         "Pull the pin with the trigger, run a 3 s fuse")),
        MechanicPlanItem(id="grenade_score_hud", classification=MechanicClassification.generate,
                         code_task=_task("grenade_score_hud", "Grenade score HUD",
                                         "Counts targets broken by grenade_activation_throw")),
    ])
    out, notes = reconcile_weapon_items(plan, _design())
    by_id = {i.id: i for i in out.items}
    assert "grenade_activation_throw" not in by_id
    ex = by_id["explosive_frag_grenade"]
    assert ex.builtin_id == "builtin.explosive" and ex.config["fuse_s"] == 3.5
    assert ex.binding is not None and ex.binding.control.value == "trigger"
    hud = by_id["grenade_score_hud"].code_task
    assert hud.depends_on == [] and "explosive.detonated" in hud.description
    assert any("grenade_activation_throw" in n for n in notes)
    again, notes2 = reconcile_weapon_items(out, _design())
    assert notes2 == [] and again is out
    # The interaction reconcile appending after it does not make it noisy.
    final, _ = reconcile_interaction_items(out, _design())
    third, notes3 = reconcile_weapon_items(final, _design())
    assert notes3 == [] and third is final


def test_world_breakable_target_reads_destroyed_and_takes_damage():
    doc = {"entities": [{"id": "lane1_destructible_target_2", "mechanic_bindings": []},
                        {"id": "storefront_glass", "mechanic_bindings": []}],
           "interactives": [
               {"id": "lane1_destructible_target_2", "kind": "breakable",
                "initial_state": {"broken": {"type": "bool", "value": False}}},
               {"id": "storefront_glass", "kind": "breakable", "initial_state": {}}]}
    bound, notes = bind_affordances(doc, None, "range")
    target, glass = doc["entities"]
    state = doc["interactives"][0]["initial_state"]
    assert "broken" not in state and state["destroyed"]["value"] is False
    ids = [b["id"] for b in target["mechanic_bindings"]]
    assert ids == ["builtin.destructible", "builtin.breakable"]
    assert target["mechanic_bindings"][1]["params"]["breaks_by_hand"] is False
    assert target["health"]["max"] == 30.0
    assert [b["id"] for b in glass["mechanic_bindings"]] == ["builtin.breakable"]
    assert "breaks_by_hand" not in glass["mechanic_bindings"][0]["params"]
    assert any("'broken' -> 'destroyed'" in n for n in notes)
