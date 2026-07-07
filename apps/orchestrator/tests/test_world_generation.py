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
