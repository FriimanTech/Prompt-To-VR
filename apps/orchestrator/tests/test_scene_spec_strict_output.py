"""Strict structured output + diagnosable failures for the SceneSpec agent.

The stage used to fail like this, three times in a row, over ten minutes::

    Agents SDK run failed: Invalid JSON when parsing model output

That message is the SDK's redaction of a Pydantic validation error. Because
the repair loop had nothing else to say, it told the agent "your output was
invalid" and the agent made the same mistake again.

Two fixes are pinned here:

* the agent's wire schema is accepted by OpenAI **strict** mode, so
  constrained decoding makes malformed JSON and bad ids impossible;
* when something does go wrong (truncation at the token cap), the failure
  carries field-level detail and the repair turn says something useful.
"""

from __future__ import annotations

import json

import pytest
from pydantic import ValidationError

from ptv_orchestrator.pipeline.agent_output_capture import (
    diagnose_output_failure,
    issues_from_validation_error,
    looks_truncated,
)
from ptv_orchestrator.pipeline.scene_spec_agent import (
    SceneSpecAgentError,
    SceneSpecOutputError,
    build_truncation_message,
    classify_output_failure,
)
from ptv_orchestrator.pipeline.scene_spec_wire import (
    WireSceneSpec,
    decode_param_value,
    to_scene_spec,
)


def _payload(**overrides) -> dict:
    transform = {
        "position": {"x": 0.0, "y": 1.0, "z": -0.4},
        "rotation": {"x": 0.0, "y": 0.0, "z": 0.0, "w": 1.0},
        "scale": 1.0,
    }
    base = {
        "prompt_id": "range-ready",
        "scene_id": "shooting_range",
        "name": "Shooting Range",
        "player_spawn": {
            "transform": transform,
            "face_direction": {"x": 0.0, "y": 0.0, "z": -1.0},
        },
        "entities": [
            {
                "id": "pistol",
                "name": "Pistol",
                "transform": transform,
                "mesh": {"asset_id": "pistol", "glb_path": "assets/pistol.glb"},
                "physics": {
                    "kind": "dynamic",
                    "mass_kg": 1.2,
                    "collision_shape": "aabb",
                    "restitution": 0.2,
                    "friction": 0.5,
                },
                "mechanic_bindings": [
                    {
                        "mechanic_id": "builtin.grab",
                        "param_overrides": [
                            {"key": "reach_m", "value_json": "0.55"},
                            {"key": "two_handed", "value_json": "true"},
                            {"key": "hand", "value_json": '"right"'},
                            {"key": "offset", "value_json": "[0,0,-0.2]"},
                            {"key": "sloppy", "value_json": "left"},
                        ],
                    }
                ],
                "tags": ["interactable", "grabbable"],
                "health": None,
                "faction": None,
                "attached_to": None,
            }
        ],
        "lights": [],
        "sky_color": [0.05, 0.07, 0.11],
        "ambient_intensity": 0.25,
    }
    base.update(overrides)
    return base


# ---------------------------------------------------------------------------
# Strict-mode acceptance
# ---------------------------------------------------------------------------


def test_wire_schema_is_accepted_by_strict_mode() -> None:
    """This is the whole point: strict mode guarantees parseable output.

    ``SceneSpec`` itself cannot be used here -- ``MeshRef.sockets`` and
    ``MechanicBinding.params`` are open-ended dicts, which strict mode
    forbids.
    """
    agents = pytest.importorskip("agents")

    schema = agents.AgentOutputSchema(WireSceneSpec, strict_json_schema=True)
    rendered = schema.json_schema()

    assert schema.is_strict_json_schema() is True

    def walk(node, path="$"):
        if isinstance(node, dict):
            if node.get("type") == "object" and "properties" in node:
                assert node.get("additionalProperties") is False, path
                assert set(node["required"]) == set(node["properties"]), path
            for key, value in node.items():
                walk(value, f"{path}.{key}")
        elif isinstance(node, list):
            for i, value in enumerate(node):
                walk(value, f"{path}[{i}]")

    walk(rendered)

    text = json.dumps(rendered)
    for unsupported in ("patternProperties", '"allOf"', '"oneOf"', '"not"'):
        assert unsupported not in text, unsupported
    # Ids keep their pattern, which strict mode DOES support: constrained
    # decoding then enforces snake_case for us.
    assert '"pattern"' in text


def test_scene_spec_itself_is_not_strict_compatible() -> None:
    """Documents why the wire model exists at all."""
    agents = pytest.importorskip("agents")
    from ptv_schemas import SceneSpec

    with pytest.raises(Exception):
        agents.AgentOutputSchema(SceneSpec, strict_json_schema=True)


# ---------------------------------------------------------------------------
# Wire -> artifact mapping
# ---------------------------------------------------------------------------


def test_param_overrides_decode_into_real_params() -> None:
    spec = to_scene_spec(WireSceneSpec.model_validate(_payload()))

    params = spec.entities[0].mechanic_bindings[0].params
    assert params == {
        "reach_m": 0.55,
        "two_handed": True,
        "hand": "right",
        "offset": [0, 0, -0.2],
        # An unquoted string is the one mistake worth forgiving.
        "sloppy": "left",
    }


def test_decode_param_value_prefers_json_but_tolerates_bare_strings() -> None:
    assert decode_param_value("0.5") == 0.5
    assert decode_param_value("null") is None
    assert decode_param_value('{"a":1}') == {"a": 1}
    assert decode_param_value("right_hand") == "right_hand"


def test_sockets_are_left_to_the_scene_compiler() -> None:
    spec = to_scene_spec(WireSceneSpec.model_validate(_payload()))
    mesh = spec.entities[0].mesh
    assert mesh is not None
    assert mesh.sockets == {}


def test_sky_color_is_clamped_and_defaulted() -> None:
    spec = to_scene_spec(
        WireSceneSpec.model_validate(_payload(sky_color=[2.0, -1.0, 0.5]))
    )
    assert spec.sky_color == [1.0, 0.0, 0.5]


def test_bad_ids_are_rejected_by_the_wire_schema() -> None:
    """Belt and braces: strict decoding should never produce this, but the
    schema still refuses it if it somehow does."""
    payload = _payload()
    payload["entities"][0]["id"] = "PaperTarget03"
    with pytest.raises(ValidationError):
        WireSceneSpec.model_validate(payload)


# ---------------------------------------------------------------------------
# Diagnosable failures
# ---------------------------------------------------------------------------


def test_validation_failure_names_the_offending_fields() -> None:
    payload = _payload()
    payload["entities"][0]["name"] = 17  # wrong type
    del payload["entities"][0]["tags"]
    raw = json.dumps(payload)

    error = classify_output_failure(raw, RuntimeError("Invalid JSON when parsing model output"))

    assert isinstance(error, SceneSpecOutputError)
    assert error.truncated is False
    joined = " | ".join(error.issues)
    assert "entities.0.name" in joined, joined
    # A repair turn built from this actually says something.
    from ptv_orchestrator.pipeline.scene_spec_agent import build_repair_message

    assert "entities.0.name" in build_repair_message(error.issues)


def test_repeated_field_errors_collapse_to_one_issue() -> None:
    payload = _payload()
    entity = payload["entities"][0]
    payload["entities"] = []
    for i in range(30):
        clone = json.loads(json.dumps(entity))
        clone["id"] = f"target_{i:02d}"
        clone["name"] = 17
        payload["entities"].append(clone)

    error = classify_output_failure(json.dumps(payload), RuntimeError("boom"))

    assert isinstance(error, SceneSpecOutputError)
    name_issues = [i for i in error.issues if i.endswith("Input should be a valid string")]
    assert len(name_issues) == 1, error.issues


def test_truncated_output_is_classified_separately() -> None:
    raw = json.dumps(_payload())[:400]  # cut mid-structure
    assert looks_truncated(raw)

    error = classify_output_failure(raw, RuntimeError("boom"))

    assert isinstance(error, SceneSpecOutputError)
    assert error.truncated is True

    message = build_truncation_message(90)
    assert "CUT OFF" in message
    assert "at most 90 entities" in message


def test_failure_before_any_model_text_is_not_treated_as_repairable() -> None:
    error = classify_output_failure(None, RuntimeError("connection reset"))

    assert isinstance(error, SceneSpecAgentError)
    assert not isinstance(error, SceneSpecOutputError)
    assert "connection reset" in str(error)


def test_diagnose_is_shared_across_agents() -> None:
    """The mechanic_plan / npc_plan / world agents use the same helper."""
    from ptv_schemas import MechanicPlan

    diagnosis = diagnose_output_failure('{"prompt_id": 5}', RuntimeError("x"), MechanicPlan)

    assert diagnosis.kind == "validation"
    assert diagnosis.repairable is True
    assert diagnosis.issues


def test_issues_are_capped() -> None:
    from ptv_schemas import SceneSpec

    try:
        SceneSpec.model_validate(
            {"prompt_id": "x", "scene_id": "y", "name": "z", "entities": [
                {"id": f"e{i}", "name": i} for i in range(40)
            ]}
        )
    except ValidationError as err:
        issues = issues_from_validation_error(err)
        assert len(issues) <= 13, issues  # 12 + the "omitted" note
    else:  # pragma: no cover
        pytest.fail("expected a ValidationError")


# ---------------------------------------------------------------------------
# equip_slots must be reachable through the wire schema.
#
# Strict mode permits exactly the declared surface, so a field the wire model
# omits is a field the agent CANNOT emit -- silently, for every future game.
# ---------------------------------------------------------------------------


def _wearer_payload() -> dict:
    transform = {
        "position": {"x": 0.0, "y": 0.0, "z": 0.0},
        "rotation": {"x": 0.0, "y": 0.0, "z": 0.0, "w": 1.0},
        "scale": 1.0,
    }
    payload = _payload()
    payload["entities"].append(
        {
            "id": "player_rig",
            "name": "Player",
            "transform": transform,
            "mesh": None,
            "physics": None,
            "mechanic_bindings": [],
            "tags": ["player"],
            "health": None,
            "faction": None,
            "attached_to": None,
            "equip_slots": [
                {
                    "name": "hip_holster_r",
                    "body_slot": "waist",
                    "host_item_slot": "",
                    "host_socket": "",
                    "local_position": {"x": 0.16, "y": -0.05, "z": 0.02},
                    "local_rotation": {"x": 0.0, "y": 0.0, "z": 0.0, "w": 1.0},
                    "snap_radius_m": 0.2,
                    "exclusive": True,
                    "accepts_tags": ["pistol"],
                }
            ],
        }
    )
    return payload


def test_equip_slots_survive_the_wire_round_trip() -> None:
    spec = to_scene_spec(WireSceneSpec.model_validate(_wearer_payload()))

    wearer = next(e for e in spec.entities if e.id == "player_rig")
    slot = wearer.equip_slots[0]
    assert slot.name == "hip_holster_r"
    assert slot.body_slot == "waist"
    assert (slot.local_position.x, slot.local_position.z) == (0.16, 0.02)
    assert slot.snap_radius_m == 0.2
    assert slot.accepts_tags == ["pistol"]
    # An item entity that declares none stays empty rather than inheriting.
    assert spec.entities[0].equip_slots == []


def test_equip_slots_are_in_the_strict_wire_schema() -> None:
    agents = pytest.importorskip("agents")

    rendered = agents.AgentOutputSchema(
        WireSceneSpec, strict_json_schema=True
    ).json_schema()
    text = json.dumps(rendered)
    assert "equip_slots" in text
    assert "EquipSlotSpec" in text
