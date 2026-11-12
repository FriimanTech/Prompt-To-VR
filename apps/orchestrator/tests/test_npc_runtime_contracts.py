"""Checks that keep generated NPCs alive at runtime: the codegen contract
lint, the built-in coverage reconcile, persistent placement against the
world, and binding resolution against what the build registers."""

from __future__ import annotations

from pathlib import Path

from ptv_schemas import (
    CodeTask,
    MechanicClassification,
    MechanicPlan,
    MechanicPlanItem,
    NpcPlan,
)

from ptv_orchestrator.pipeline.binding_resolution import (
    drop_unhosted_npc_ai,
    registered_ids,
    unresolved_bindings,
)
from ptv_orchestrator.pipeline.codegen_contracts import (
    find_dead_contracts,
    lint_task_sources,
)
from ptv_orchestrator.pipeline.encounters import place_persistent_npcs
from ptv_orchestrator.pipeline.npc_population import reconcile_npc_ai_items

REPO = Path(__file__).resolve().parents[3]


def _npc_plan(behaviours: list[str], style: str = "animated_locomotion") -> NpcPlan:
    return NpcPlan.model_validate({
        "prompt_id": "p",
        "npcs": [{
            "asset_id": "green_ally", "role": "ally", "faction": "ally",
            "persistent_count": 1, "movement_style": style,
            "behaviors": behaviours,
        }],
    })


def _ai_item(base: str = "builtin.npc.follow") -> MechanicPlanItem:
    return MechanicPlanItem(
        id="ally_follow",
        classification=MechanicClassification.builtin_config_code_edit,
        base_builtin_id=base,
        config={"catch_up": False, "min_distance_m": 1.5, "avoid_weapon_line": True},
        code_task=CodeTask.model_validate({
            "id": "ally_follow", "kind": "ai", "title": "Follow",
            "description": "Copy builtin.npc.follow for green_ally.",
            "interface": "IBehaviour",
        }),
    )


# --- codegen contract lint ---------------------------------------------------

LEGACY_AI = """
class Follow : public IBehaviour {
  void on_tick(World& world, std::uint32_t e, float dt) override;
};
PTV_REGISTER_BEHAVIOUR("generated.follow", Follow)
"""

CTX_AI = """
class Follow : public IBehaviour {
  void on_tick(World& world, std::uint32_t e, float dt,
               const InputSnapshot& input, MechanicContext& ctx) override;
};
PTV_REGISTER_BEHAVIOUR("generated.follow", Follow)
"""


def test_lint_rejects_context_free_ai_and_accepts_the_context_form() -> None:
    assert any("context-free" in p for p in lint_task_sources("ai", {"a.cpp": LEGACY_AI}))
    assert lint_task_sources("ai", {"a.cpp": CTX_AI}) == []


def test_lint_rejects_player_lookup_by_name_and_private_damage_inbox() -> None:
    probs = lint_task_sources("mechanic", {
        "a.cpp": 'auto p = world.find("player_rig");',
        "a.h": "struct MeleeHitInbox { int n; };",
    })
    assert any("player_rig" in p for p in probs)
    assert any("MeleeHitInbox" in p for p in probs)


def test_lint_rejects_a_private_health_pool_that_ignores_combat() -> None:
    private = {"h.h": "struct HealthState { float current_health{100}; };",
               "d.cpp": "s.current_health = std::max(0.0f, before - damage);"}
    assert any("health pool" in p for p in lint_task_sources("mechanic", private))
    mirrored = dict(private, **{"s.cpp": "s.current_health = ctx.combat->health(p);"})
    assert lint_task_sources("mechanic", mirrored) == []
    display_copy = {"hud.h": "struct Hud { float current_health{0}; };"}
    assert lint_task_sources("mechanic", display_copy) == []


def test_dead_contract_is_an_offered_entry_point_nobody_calls() -> None:
    sources = {
        "hud": {"hud.h": "inline void push_panel_event(int x) {}"},
        "flow": {"flow.cpp": "int main() { return 0; }"},
    }
    assert any("push_panel_event" in w for w in find_dead_contracts(sources))
    sources["flow"]["flow.cpp"] = "void f() { push_panel_event(1); }"
    assert find_dead_contracts(sources) == []


# --- built-in coverage reconcile ----------------------------------------------

def test_follow_only_npc_ai_becomes_the_builtin() -> None:
    plan = MechanicPlan(prompt_id="p", items=[_ai_item()])
    out, notes = reconcile_npc_ai_items(plan, _npc_plan(["follow_leader"]))
    item = out.items[0]
    assert item.classification == MechanicClassification.builtin_config
    assert item.builtin_id == "builtin.npc.follow"
    assert item.code_task is None
    assert item.config == {"catch_up": False, "min_distance_m": 1.5}
    assert notes and "avoid_weapon_line" in notes[0]


def test_uncovered_behaviour_keeps_its_generated_ai() -> None:
    plan = MechanicPlan(prompt_id="p", items=[_ai_item("builtin.npc.melee_chase")])
    out, notes = reconcile_npc_ai_items(
        plan, _npc_plan(["chase", "melee_attack", "reposition"]))
    assert out.items[0].code_task is not None and notes == []


def test_stationary_npc_ai_is_left_alone() -> None:
    plan = MechanicPlan(prompt_id="p", items=[_ai_item()])
    out, _ = reconcile_npc_ai_items(plan, _npc_plan(["follow_leader"], "stationary"))
    assert out.items[0].code_task is not None


# --- placement against the merged world ---------------------------------------

def _doc(pos: list[float], anchors: list[dict] | None = None) -> dict:
    return {
        "player": {"spawn": {"position": [0.0, 0.05, 0.0]}},
        "navigation": {"regions": [
            {"id": "yard", "min": [-10, 0, -10], "max": [10, 0, 10], "floor_y": 0.25}]},
        "npc_anchors": anchors or [],
        "entities": [{
            "id": "ally_1", "transform": {"position": pos},
            "mesh": {"asset_id": "green_ally", "target_scale_meters": 1.72,
                     "foot_offset_m": 0.86},
        }],
    }


def test_persistent_npc_stands_on_the_floor_inside_the_walkable_area() -> None:
    doc = _doc([25.0, 0.0, 1.0])
    notes = place_persistent_npcs(doc, {"green_ally": ["ally_1"]}, _npc_plan(["follow_leader"]))
    x, y, z = doc["entities"][0]["transform"]["position"]
    assert -10 <= x <= 10 and abs(y - (0.25 + 0.86)) < 1e-6
    assert any("walkable" in n for n in notes)


def test_two_npcs_given_one_spot_stand_apart() -> None:
    doc = _doc([2.0, 0.0, 1.0])
    doc["entities"].append({
        "id": "ally_2", "transform": {"position": [2.0, 0.0, 1.0]},
        "mesh": {"asset_id": "green_ally", "target_scale_meters": 1.72, "foot_offset_m": 0.86}})
    notes = place_persistent_npcs(doc, {"green_ally": ["ally_1", "ally_2"]},
                                  _npc_plan(["follow_leader"]))
    a, b = (e["transform"]["position"] for e in doc["entities"])
    assert ((a[0] - b[0]) ** 2 + (a[2] - b[2]) ** 2) ** 0.5 >= 0.9 - 1e-6
    assert -10 <= b[0] <= 10 and -10 <= b[2] <= 10
    assert any("shared its spot" in n for n in notes)


def test_world_anchor_naming_the_npc_wins() -> None:
    doc = _doc([0.0, 0.0, 1.0], anchors=[
        {"id": "porch", "position": [4.0, 0.25, -3.0], "facing": [1, 0, 0],
         "tags": ["ally"]}])
    place_persistent_npcs(doc, {"green_ally": ["ally_1"]}, _npc_plan(["follow_leader"]))
    e = doc["entities"][0]
    assert e["transform"]["position"][0] == 4.0 and e["transform"]["position"][2] == -3.0
    assert abs(e["transform"]["rotation"][1]) > 0.5  # turned to face +X


# --- binding resolution -------------------------------------------------------

def test_registered_ids_find_builtins_in_engine_source() -> None:
    reg = registered_ids(REPO, "no-such-project")
    assert reg.get("builtin.npc.follow") == "mechanic"
    assert reg.get("builtin.encounter_director") == "mechanic"


def test_unresolved_and_unhosted_bindings() -> None:
    reg = {"builtin.npc.follow": "mechanic", "generated.chase": "behaviour"}
    ents = [
        {"id": "a", "mechanic_bindings": [{"id": "generated.chase"}]},
        {"id": "b", "mechanic_bindings": [{"id": "generated.gone"},
                                          {"id": "builtin.npc.follow"}]},
    ]
    assert unresolved_bindings(ents, reg) == [
        "b: generated.gone is registered nowhere in the engine or this "
        "project's generated code"]
    hosted, notes = drop_unhosted_npc_ai(ents, reg, {"generated.chase", "generated.gone"})
    assert hosted == {"generated.chase"} and notes
    assert ents[1]["mechanic_bindings"] == [{"id": "builtin.npc.follow"}]
