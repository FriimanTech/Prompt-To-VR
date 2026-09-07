"""World-generation validation: catch the mistakes that otherwise ship.

Every failure mode here shares one property, which is why validation is worth
the code: the world still LOOKS finished. An interactive whose id matches no
geometry, a nav link gated by a door that was never declared, a duplicate
entity id sharing one save slot -- none of these produce a visible defect at
generation time. They produce a level where the door is not a door, an NPC
walks through a shut gate, or a save restores a state that never existed.

Catching them here turns a shipped bug into a repair round.
"""

from __future__ import annotations

from ptv_orchestrator.pipeline.stage_scene_compile import AFFORDANCE_MECHANICS
from ptv_orchestrator.pipeline.stage_world_generation import (
    _AFFORDANCE_KINDS,
    _validate_entity_ids,
    _validate_interactives,
    _validate_portals,
    validate_environment_graph,
)
from ptv_schemas.build_spec import (
    AssetEntry,
    BuildSpec,
    EnvironmentMode,
    GameType,
    MechanicEntry,
    NestedHost,
    SceneEntry,
    SceneLink,
    TestEntry,
)


def _door_world(**overrides) -> dict:
    doc = {
        "entities": [
            {"id": "ground", "tags": ["ground"]},
            {"id": "front_door"},
            {"id": "lobby_light"},
        ],
        "interactives": [
            {
                "id": "front_door",
                "kind": "door",
                "min_angle_deg": 0.0,
                "max_angle_deg": 90.0,
            },
            {"id": "lobby_light", "kind": "switchable"},
        ],
        "navigation": {
            "regions": [{"id": "outside"}, {"id": "lobby"}],
            "links": [
                {
                    "from": "outside",
                    "to": "lobby",
                    "gated_by": "front_door",
                    "gate_state_key": "open",
                }
            ],
        },
    }
    doc.update(overrides)
    return doc


# --- the two kind lists must agree -----------------------------------


def test_the_validator_and_the_binder_know_the_same_affordance_kinds():
    # Two hand-maintained lists of the same vocabulary drift. If they do, the
    # symptom is a kind that validates fine and then binds to nothing, or one
    # that binds fine and gets rejected -- both confusing.
    assert _AFFORDANCE_KINDS == set(AFFORDANCE_MECHANICS)


# --- entity ids -------------------------------------------------------


def test_duplicate_entity_ids_are_rejected():
    doc = _door_world()
    doc["entities"].append({"id": "front_door"})
    problems = _validate_entity_ids(doc)
    assert problems and "duplicate" in problems[0]
    assert "front_door" in problems[0]


def test_blank_entity_ids_are_rejected():
    doc = _door_world()
    doc["entities"].append({"id": ""})
    problems = _validate_entity_ids(doc)
    assert problems and "no id" in problems[0]


def test_a_clean_world_has_no_id_problems():
    assert _validate_entity_ids(_door_world()) == []


# --- interactives -----------------------------------------------------


def test_a_clean_world_passes_interactive_validation():
    assert _validate_interactives(_door_world()) == []


def test_an_interactive_with_no_geometry_is_rejected():
    # The silent one: the binder has nothing to attach to, so the door simply
    # is not a door, and the world looks perfectly fine.
    doc = _door_world()
    doc["interactives"].append({"id": "ghost_gate", "kind": "door",
                                "max_angle_deg": 90.0})
    problems = _validate_interactives(doc)
    assert any("ghost_gate" in p and "no entity" in p for p in problems)


def test_an_unknown_kind_is_rejected_with_the_valid_list():
    doc = _door_world()
    doc["entities"].append({"id": "odd_thing"})
    doc["interactives"].append({"id": "odd_thing", "kind": "teleporter"})
    problems = _validate_interactives(doc)
    assert any("teleporter" in p for p in problems)
    # The message has to say what WAS acceptable, or the repair round guesses.
    assert any("harvestable" in p for p in problems)


def test_a_duplicate_id_is_reported_instead_of_its_kind():
    # A duplicate is the more fundamental problem, so reporting it alone is
    # correct -- but the message must name the duplicate rather than
    # complaining about whatever kind the second copy happened to use.
    doc = _door_world()
    doc["interactives"].append({"id": "front_door", "kind": "teleporter"})
    problems = _validate_interactives(doc)
    assert any("duplicate interactive id 'front_door'" in p for p in problems)


def test_a_door_that_cannot_open_is_rejected():
    doc = _door_world()
    doc["interactives"][0]["max_angle_deg"] = 0.0
    problems = _validate_interactives(doc)
    assert any("cannot open" in p for p in problems)


def test_a_sliding_door_is_accepted_without_a_hinge_range():
    doc = _door_world()
    doc["interactives"][0].update(
        {"max_angle_deg": 0.0, "slide_distance_m": 0.9}
    )
    assert _validate_interactives(doc) == []


def test_a_mover_with_nowhere_to_go_is_rejected():
    doc = _door_world()
    doc["entities"].append({"id": "elevator"})
    doc["interactives"].append(
        {"id": "elevator", "kind": "mover", "positions": ["floor_1"]}
    )
    problems = _validate_interactives(doc)
    assert any("nowhere to travel" in p for p in problems)


def test_a_harvestable_with_no_yield_is_rejected():
    doc = _door_world()
    doc["entities"].append({"id": "grass_field"})
    doc["interactives"].append({"id": "grass_field", "kind": "harvestable"})
    problems = _validate_interactives(doc)
    assert any("yields_asset_id" in p for p in problems)


def test_a_nav_gate_naming_nothing_is_rejected():
    # Without this the route is either permanently open or permanently shut
    # depending on how the runtime reads a missing id, and neither is visible.
    doc = _door_world()
    doc["navigation"]["links"][0]["gated_by"] = "a_door_nobody_declared"
    problems = _validate_interactives(doc)
    assert any("a_door_nobody_declared" in p for p in problems)


def test_an_ungated_link_is_still_fine():
    doc = _door_world()
    del doc["navigation"]["links"][0]["gated_by"]
    assert _validate_interactives(doc) == []


def test_too_many_interactives_is_rejected():
    doc = _door_world()
    doc["entities"] += [{"id": f"prop_{i}"} for i in range(600)]
    doc["interactives"] += [
        {"id": f"prop_{i}", "kind": "movable"} for i in range(600)
    ]
    problems = _validate_interactives(doc)
    assert any("cap" in p for p in problems)


def test_duplicate_interactive_ids_are_rejected():
    doc = _door_world()
    doc["interactives"].append({"id": "front_door", "kind": "movable"})
    problems = _validate_interactives(doc)
    assert any("duplicate interactive id" in p for p in problems)


def test_a_world_with_no_interactives_is_valid():
    # Most environments declare none, and that must stay free.
    assert _validate_interactives({"entities": [{"id": "ground"}]}) == []


# --- portals ----------------------------------------------------------


def test_a_portal_with_no_target_is_rejected():
    doc = _door_world(portals=[{"id": "gate", "to_scene": ""}])
    problems = _validate_portals(doc)
    assert any("names no target scene" in p for p in problems)


def test_duplicate_spawn_point_ids_are_rejected():
    doc = _door_world(
        spawn_points=[{"id": "north_door"}, {"id": "north_door"}]
    )
    problems = _validate_portals(doc)
    assert any("duplicate spawn point" in p for p in problems)


def test_a_valid_portal_passes():
    doc = _door_world(
        portals=[{"id": "gate", "to_scene": "mission", "target_spawn_id": "a"}],
        spawn_points=[{"id": "entrance"}],
    )
    assert _validate_portals(doc) == []


# --- the environment graph -------------------------------------------


def _graph_spec(scenes: list[SceneEntry]) -> BuildSpec:
    return BuildSpec(
        prompt_id="demo",
        title="Demo",
        pitch="A hub and missions.",
        game_type=GameType.shooter,
        scenes=scenes,
        mechanics=[MechanicEntry(id="m", name="M", summary="s")],
        assets=[AssetEntry(id="a", name="A", category="prop", summary="s")],
        acceptance_tests=[TestEntry(id="t", description="d")],
    )


def test_a_hub_and_spoke_graph_is_valid():
    spec = _graph_spec(
        [
            SceneEntry(
                id="hub",
                name="Hub",
                summary="Base.",
                entry_point=True,
                links=[
                    SceneLink(to_scene="mission_a"),
                    SceneLink(to_scene="mission_b"),
                ],
            ),
            SceneEntry(id="mission_a", name="A", summary="A."),
            SceneEntry(id="mission_b", name="B", summary="B."),
        ]
    )
    assert validate_environment_graph(spec) == []


def test_a_link_to_a_missing_scene_is_rejected():
    spec = _graph_spec(
        [
            SceneEntry(
                id="hub",
                name="Hub",
                summary="Base.",
                entry_point=True,
                links=[SceneLink(to_scene="a_mission_that_was_never_planned")],
            )
        ]
    )
    problems = validate_environment_graph(spec)
    assert any("never_planned" in p for p in problems)


def test_an_unreachable_scene_is_rejected():
    # Content that was generated, paid for, and can never be played.
    spec = _graph_spec(
        [
            SceneEntry(id="hub", name="Hub", summary="Base.", entry_point=True),
            SceneEntry(id="orphan", name="Orphan", summary="Nowhere."),
        ]
    )
    problems = validate_environment_graph(spec)
    assert any("orphan" in p for p in problems)


def test_reuse_without_naming_a_source_is_rejected():
    # The ambiguity the flag had on its own: it says a scene borrows an
    # environment but never which one.
    spec = _graph_spec(
        [
            SceneEntry(
                id="hub",
                name="Hub",
                summary="Base.",
                entry_point=True,
                links=[SceneLink(to_scene="twin")],
            ),
            SceneEntry(
                id="twin",
                name="Twin",
                summary="Same place.",
                needs_unique_environment=False,
            ),
        ]
    )
    problems = validate_environment_graph(spec)
    assert any("ambiguous" in p for p in problems)


def test_reuse_naming_a_real_sibling_is_valid():
    spec = _graph_spec(
        [
            SceneEntry(
                id="hub",
                name="Hub",
                summary="Base.",
                entry_point=True,
                links=[SceneLink(to_scene="twin")],
            ),
            SceneEntry(
                id="twin",
                name="Twin",
                summary="Same place.",
                needs_unique_environment=False,
                reuse_environment_from="hub",
            ),
        ]
    )
    assert validate_environment_graph(spec) == []


def test_a_nested_scene_needs_a_host_but_not_a_link():
    # A head interior is entered by its host, not by walking through a portal,
    # so it must not be reported unreachable -- but it does need a host.
    hostless = _graph_spec(
        [
            SceneEntry(id="room", name="Room", summary="A.", entry_point=True),
            SceneEntry(
                id="inside_head",
                name="Interior",
                summary="Skull.",
                environment_mode=EnvironmentMode.nested,
            ),
        ]
    )
    problems = validate_environment_graph(hostless)
    assert any("no host" in p for p in problems)
    assert not any("no route" in p for p in problems)

    hosted = _graph_spec(
        [
            SceneEntry(id="room", name="Room", summary="A.", entry_point=True),
            SceneEntry(
                id="inside_head",
                name="Interior",
                summary="Skull.",
                environment_mode=EnvironmentMode.nested,
                host=NestedHost(asset_id="student"),
            ),
        ]
    )
    assert validate_environment_graph(hosted) == []


def test_a_single_scene_game_is_valid():
    spec = _graph_spec(
        [SceneEntry(id="room", name="Room", summary="One room.")]
    )
    assert validate_environment_graph(spec) == []
