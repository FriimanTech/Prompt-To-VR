"""The affordance binder connects world declarations to engine mechanics.

The property this exists to guarantee is consistency: if grass can be picked
in the hub it can be picked in a mission, because ONE table is applied to
every environment. That is also why the binder is deterministic rather than a
fourth LLM stage -- an agent reading six environments and all their mechanics
is precisely the thing that wires an affordance in one place and forgets it in
another, and no test can catch a forgetting that only happens sometimes.

Intentional divergence has one explicit home: BuildSpec.interaction_rules.
"""

from __future__ import annotations

import copy

from ptv_orchestrator.pipeline.stage_scene_compile import (
    AFFORDANCE_MECHANICS,
    bind_affordances,
)
from ptv_schemas.build_spec import (
    AssetEntry,
    BuildSpec,
    GameType,
    InteractionRule,
    MechanicEntry,
    SceneEntry,
    TestEntry,
)


def _spec(rules: list[InteractionRule] | None = None) -> BuildSpec:
    return BuildSpec(
        prompt_id="demo",
        title="Demo",
        pitch="A hub and a mission, both with grass.",
        game_type=GameType.adventure,
        scenes=[
            SceneEntry(id="hub", name="Hub", summary="Base of operations."),
            SceneEntry(id="mission", name="Mission", summary="A field."),
        ],
        mechanics=[MechanicEntry(id="grab", name="Grab", summary="Grab.")],
        assets=[
            AssetEntry(id="blade", name="Blade", category="prop", summary="Grass.")
        ],
        acceptance_tests=[TestEntry(id="t1", description="Grass is pickable.")],
        interaction_rules=rules or [],
    )


def _scene_doc() -> dict:
    """A scene with a door, a grass field and a purely declarative movable."""
    return {
        "entities": [
            {"id": "front_door", "name": "Front Door"},
            {"id": "grass_field", "name": "Grass Field"},
            {"id": "glass_bottle", "name": "Glass Bottle"},
        ],
        "interactives": [
            {"id": "front_door", "kind": "door"},
            {
                "id": "grass_field",
                "kind": "harvestable",
                "tags": ["grabbable"],
                "yields_asset_id": "blade",
            },
            {"id": "glass_bottle", "kind": "movable"},
        ],
    }


def _binding_ids(doc: dict, entity_id: str) -> list[str]:
    for e in doc["entities"]:
        if e["id"] == entity_id:
            return [b["id"] for b in e.get("mechanic_bindings", [])]
    return []


# --- the core wiring --------------------------------------------------


def test_declared_affordances_become_real_mechanic_bindings():
    doc = _scene_doc()
    bound, notes = bind_affordances(doc, _spec(), "hub")
    assert bound == 2, notes
    assert _binding_ids(doc, "front_door") == ["builtin.door"]
    assert _binding_ids(doc, "grass_field") == ["builtin.harvestable"]


def test_a_declaration_only_kind_gets_no_mechanic():
    # 'movable' says "persist this object's transform". The interaction is
    # already builtin.grab's job, so binding a mechanic would be a second
    # writer on the same object.
    doc = _scene_doc()
    bind_affordances(doc, _spec(), "hub")
    assert _binding_ids(doc, "glass_bottle") == []


def test_per_instance_params_reach_the_binding():
    # The reason a tag could not have done this job: a tag is a bare string
    # and cannot carry a reach, a hinge axis or a range.
    doc = _scene_doc()
    bind_affordances(doc, _spec(), "hub")
    for e in doc["entities"]:
        if e["id"] == "grass_field":
            assert e["mechanic_bindings"][0]["params"]["reach_m"] == 0.5


def test_every_affordance_kind_maps_to_a_real_builtin():
    from ptv_orchestrator.engine_catalog import all_known_ids

    known = all_known_ids()
    for kind, mechanic_id in AFFORDANCE_MECHANICS.items():
        if not mechanic_id:
            continue
        assert mechanic_id in known, (
            f"affordance '{kind}' maps to '{mechanic_id}', which is not in "
            "the engine catalog, so a scene binding it would do nothing"
        )


# --- the consistency guarantee ---------------------------------------


def test_the_same_declaration_binds_identically_in_every_environment():
    # THE property. Two environments, the same grass, the same result --
    # produced by the algorithm rather than by anyone remembering.
    spec = _spec()
    hub = _scene_doc()
    mission = _scene_doc()
    bind_affordances(hub, spec, "hub")
    bind_affordances(mission, spec, "mission")
    assert _binding_ids(hub, "grass_field") == _binding_ids(
        mission, "grass_field"
    )
    assert hub["entities"] == mission["entities"]


def test_a_rule_denies_interaction_without_removing_the_mechanic():
    # The prompt's hub-forbids-grass case. The player performs the identical
    # gesture in both environments; only permission differs. So the mechanic
    # must STILL be bound -- removing it would change the interaction rather
    # than its outcome.
    spec = _spec(
        [
            InteractionRule(
                scene_id="hub", affordance="harvestable", allowed=False
            )
        ]
    )
    hub = _scene_doc()
    mission = _scene_doc()
    bind_affordances(hub, spec, "hub")
    bind_affordances(mission, spec, "mission")

    assert _binding_ids(hub, "grass_field") == ["builtin.harvestable"]
    assert _binding_ids(mission, "grass_field") == ["builtin.harvestable"]

    hub_grass = next(i for i in hub["interactives"] if i["id"] == "grass_field")
    mission_grass = next(
        i for i in mission["interactives"] if i["id"] == "grass_field"
    )
    assert hub_grass["interaction_allowed"] is False
    assert "interaction_allowed" not in mission_grass


def test_a_rule_can_be_written_against_a_tag_instead_of_a_kind():
    spec = _spec(
        [InteractionRule(scene_id="hub", affordance="grabbable", allowed=False)]
    )
    doc = _scene_doc()
    bind_affordances(doc, spec, "hub")
    grass = next(i for i in doc["interactives"] if i["id"] == "grass_field")
    assert grass["interaction_allowed"] is False
    # The door has neither that kind nor that tag, so it is untouched.
    door = next(i for i in doc["interactives"] if i["id"] == "front_door")
    assert "interaction_allowed" not in door


def test_a_rule_for_another_scene_does_not_leak():
    spec = _spec(
        [
            InteractionRule(
                scene_id="mission", affordance="harvestable", allowed=False
            )
        ]
    )
    doc = _scene_doc()
    bind_affordances(doc, spec, "hub")
    grass = next(i for i in doc["interactives"] if i["id"] == "grass_field")
    assert "interaction_allowed" not in grass


# --- degrading safely -------------------------------------------------


def test_an_existing_binding_is_not_duplicated():
    # A SceneSpec may already have bound the mechanic explicitly. Two copies
    # would run the mechanic twice on one object.
    doc = _scene_doc()
    for e in doc["entities"]:
        if e["id"] == "front_door":
            e["mechanic_bindings"] = [
                {"id": "builtin.door", "params": {"motion_tau_s": 0.4}}
            ]
    bind_affordances(doc, _spec(), "hub")
    assert _binding_ids(doc, "front_door") == ["builtin.door"]


def test_an_interactive_with_no_entity_is_reported_not_invented():
    doc = _scene_doc()
    doc["interactives"].append({"id": "ghost_door", "kind": "door"})
    bound, notes = bind_affordances(doc, _spec(), "hub")
    assert bound == 2
    assert any("ghost_door" in n for n in notes)


def test_an_unknown_kind_is_reported_rather_than_guessed():
    doc = _scene_doc()
    doc["interactives"].append({"id": "front_door", "kind": "teleporter"})
    _, notes = bind_affordances(doc, _spec(), "hub")
    assert any("teleporter" in n for n in notes)


def test_a_scene_with_no_interactives_is_a_no_op():
    doc = {"entities": [{"id": "wall"}]}
    before = copy.deepcopy(doc)
    assert bind_affordances(doc, _spec(), "hub") == (0, [])
    assert doc == before
