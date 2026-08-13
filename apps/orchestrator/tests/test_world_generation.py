"""Tests for the agentic world-generation feature.

These tests never touch the OpenAI Agents SDK, CMake, or the network. They
cover the pure pieces:

* world include-bounds (whitelist + header-content + path rules)
* the world-architect message construction + a stubbed WorldArchitect
* the scene-compile merge of a world.scene.json into a gameplay scene
"""

from __future__ import annotations

from pathlib import Path

from ptv_orchestrator.pipeline import bounds
from ptv_orchestrator.pipeline.stage_scene_compile import _merge_world_into_scene
from ptv_orchestrator.pipeline.world_agent import (
    WorldAgentInput,
    WorldAgentResult,
    WorldArchitect,
    WorldGeneratedFile,
    build_initial_message,
)


# ---------------------------------------------------------------------------
# Bounds
# ---------------------------------------------------------------------------


def test_world_includes_allow_interface_std_glm():
    code = (
        '#include "ptv/api/IWorldBuilder.h"\n'
        "#include <vector>\n"
        "#include <glm/glm.hpp>\n"
        '#include "world_shared.h"\n'
    )
    violations = bounds.validate_world_includes(
        code, "world.cpp", local_headers=frozenset({"world_shared.h"})
    )
    assert violations == []


def test_world_includes_reject_engine_internals():
    code = (
        '#include "ptv/api/IWorldBuilder.h"\n'
        "#include <entt/entt.hpp>\n"
        '#include "ptv/ecs/World.h"\n'
        "#include <nlohmann/json.hpp>\n"
    )
    violations = bounds.validate_world_includes(code, "world.cpp")
    # entt, ptv/ecs, and nlohmann are all disallowed for world code.
    assert len(violations) == 3


def test_world_header_rejects_registration_macro():
    hdr = "#pragma once\nPTV_REGISTER_WORLD_BUILDER(\"x\", Y)\n"
    violations = bounds.validate_world_header_content(hdr, "world_shared.h")
    assert len(violations) == 1


def test_world_path_allowed(tmp_path: Path):
    gen_root = tmp_path / "engine" / "generated"
    ok = gen_root / "world" / "proj" / "world.cpp"
    ok.parent.mkdir(parents=True)
    ok.write_text("// x", encoding="utf-8")
    bounds.assert_world_path_allowed(ok, gen_root)  # no raise

    bad = gen_root / "mechanics" / "proj" / "world.cpp"
    bad.parent.mkdir(parents=True)
    bad.write_text("// x", encoding="utf-8")
    try:
        bounds.assert_world_path_allowed(bad, gen_root)
        assert False, "expected GeneratedBoundsError"
    except bounds.GeneratedBoundsError:
        pass


# ---------------------------------------------------------------------------
# Agent wiring (stubbed runner -- no SDK)
# ---------------------------------------------------------------------------


def test_initial_message_contains_brief_and_guide():
    inp = WorldAgentInput(
        project_id="proj",
        output_subdir="engine/generated/world/proj",
        builder_id="generated.world.proj",
        engine_guide="GUIDE-TEXT",
        build_spec_excerpt="BRIEF-TEXT",
        object_assets_json="[]",
    )
    msg = build_initial_message(inp)
    assert "GUIDE-TEXT" in msg
    assert "BRIEF-TEXT" in msg
    assert "generated.world.proj" in msg
    assert "ptv/api/IWorldBuilder.h" in msg


def test_world_architect_uses_injected_runner():
    captured = {}

    def stub(message: str) -> WorldAgentResult:
        captured["message"] = message
        return WorldAgentResult(
            files=[WorldGeneratedFile(path="world.cpp", contents="// c++")],
            builder_id="generated.world.proj",
        )

    arch = WorldArchitect(instructions="SYS", runner=stub)
    result = arch.run("hello")
    assert captured["message"] == "hello"
    assert result.files[0].path == "world.cpp"


# ---------------------------------------------------------------------------
# Scene merge
# ---------------------------------------------------------------------------


def test_merge_world_into_scene():
    gameplay = {
        "entities": [{"id": "enemy_1", "tags": ["enemy"]}],
        "lights": [{"id": "scene_light"}],
        "sky_color": [0.0, 0.0, 0.0],
        "player": {"spawn": {"position": [9, 9, 9]}},
    }
    world = {
        "entities": [{"id": "env_0", "tags": ["ground"]}],
        "lights": [{"id": "sun"}],
        "sky": {"top_color": [0.1, 0.1, 0.2]},
        "sky_color": [0.4, 0.5, 0.6],
        "ambient_intensity": 0.35,
        "player": {"spawn": {"position": [0, 0, 2]}},
    }
    merged = _merge_world_into_scene(dict(gameplay), world)
    ids = [e["id"] for e in merged["entities"]]
    # Environment first, gameplay on top.
    assert ids == ["env_0", "enemy_1"]
    assert merged["sky"]["top_color"] == [0.1, 0.1, 0.2]
    assert merged["sky_color"] == [0.4, 0.5, 0.6]
    assert merged["ambient_intensity"] == 0.35
    # World spawn wins so the player starts on the generated ground.
    assert merged["player"]["spawn"]["position"] == [0, 0, 2]
    # World lights precede scene lights.
    assert [l["id"] for l in merged["lights"]] == ["sun", "scene_light"]


# ---------------------------------------------------------------------------
# Navigation the world builder declares as it builds
# ---------------------------------------------------------------------------


def _two_room_world(**overrides):
    doc = {
        "entities": [{"id": "env_0", "tags": ["ground"]}],
        "player": {"spawn": {"position": [-5, 0, 0]}},
        "navigation": {
            "regions": [
                {"id": "a", "min": [-10, 0, -5], "max": [-1, 0, 5],
                 "floor_y": 0},
                {"id": "b", "min": [1, 0, -5], "max": [10, 0, 5],
                 "floor_y": 0},
            ],
            "links": [{"from": "a", "to": "b", "point": [0, 0, 0]}],
        },
    }
    doc.update(overrides)
    return doc


def test_navigation_block_reaches_the_compiled_scene():
    merged = _merge_world_into_scene({"entities": []}, _two_room_world())
    assert len(merged["navigation"]["regions"]) == 2


def test_a_world_with_no_navigation_is_still_valid():
    """An open arena needs no declared topology; NPCs steer locally. Treating
    absence as an error would fail every simple level."""
    from ptv_orchestrator.pipeline.stage_world_generation import (
        _validate_navigation,
    )

    assert _validate_navigation({"entities": []}) == []
    assert _validate_navigation({"navigation": {"regions": []}}) == []


def test_navigation_validator_catches_the_mistakes_that_ship_silently():
    """Each of these produces a level that looks correct and has NPCs
    standing still in it, so they must fail the build rather than warn."""
    from ptv_orchestrator.pipeline.stage_world_generation import (
        _validate_navigation,
    )

    assert _validate_navigation(_two_room_world()) == []

    # A typo'd region id in a link.
    bad = _two_room_world()
    bad["navigation"]["links"] = [{"from": "a", "to": "hall", "point": [0, 0, 0]}]
    assert any("unknown region" in p for p in _validate_navigation(bad))

    # A doorway coordinate that touches neither room: reads fine in JSON,
    # sends every NPC into a wall.
    stray = _two_room_world()
    stray["navigation"]["links"] = [
        {"from": "a", "to": "b", "point": [400, 0, 400]}
    ]
    assert any("outside both regions" in p for p in _validate_navigation(stray))

    # A room nobody can walk to.
    stranded = _two_room_world()
    stranded["navigation"]["links"] = []
    assert any("no route from the spawn" in p
               for p in _validate_navigation(stranded))

    # The player starts off the walkable set entirely.
    outside = _two_room_world(player={"spawn": {"position": [900, 0, 900]}})
    assert any("spawn is not inside" in p for p in _validate_navigation(outside))

    # A region too small to stand in is a units mistake, not a design choice.
    tiny = _two_room_world()
    tiny["navigation"]["regions"][1] = {
        "id": "b", "min": [1, 0, 0], "max": [1.1, 0, 0.1], "floor_y": 0,
    }
    assert any("too small" in p for p in _validate_navigation(tiny))


def test_world_scene_validation_rejects_a_broken_nav_graph(tmp_path: Path):
    import json as _json

    from ptv_orchestrator.pipeline.stage_world_generation import (
        _validate_world_scene,
    )

    good = tmp_path / "good.json"
    good.write_text(_json.dumps(_two_room_world()), encoding="utf-8")
    ok, note = _validate_world_scene(good)
    assert ok, note
    assert "2 nav regions" in note

    broken = _two_room_world()
    broken["navigation"]["links"] = []
    bad = tmp_path / "bad.json"
    bad.write_text(_json.dumps(broken), encoding="utf-8")
    ok, reason = _validate_world_scene(bad)
    assert not ok
    # The message goes straight back to the agent, so it has to name the fix.
    assert "navigation" in reason and "no route" in reason
