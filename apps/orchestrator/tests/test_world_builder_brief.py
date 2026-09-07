"""The world architect's brief has to keep up with the engine.

Every affordance kind, tag and API the engine gained is inert until the agent
is told it exists. That makes the prompt and the author guide load-bearing
code, not documentation: a kind the engine understands but the brief never
mentions will simply never appear in a generated world, and nothing will fail
to tell you so.

These tests pin the brief against the SAME table the compiler binds from, so
adding a seventh affordance cannot silently skip the prompt.
"""

from __future__ import annotations

from pathlib import Path

from ptv_orchestrator.pipeline.prompts import (
    ASSET_MANIFEST_SYSTEM,
    BUILD_SPEC_SYSTEM,
    WORLD_BUILDER_SYSTEM,
)
from ptv_orchestrator.pipeline.stage_scene_compile import AFFORDANCE_MECHANICS

GUIDE = (
    Path(__file__).resolve().parents[3]
    / "engine"
    / "world_author_guide.md"
)


def test_the_guide_exists_and_is_the_agents_ground_truth():
    assert GUIDE.exists(), "world_author_guide.md is passed to the agent as "
    "ground truth; it cannot be missing"


def test_every_affordance_kind_is_documented_for_the_agent():
    prompt = WORLD_BUILDER_SYSTEM
    guide = GUIDE.read_text(encoding="utf-8")
    for kind in AFFORDANCE_MECHANICS:
        assert kind in prompt, (
            f"affordance kind '{kind}' is bound by the scene compiler but "
            "never mentioned in WORLD_BUILDER_SYSTEM, so no generated world "
            "will ever declare one"
        )
        assert kind in guide, (
            f"affordance kind '{kind}' is missing from the author guide"
        )


def test_the_prompt_requires_the_interactive_call_and_honest_initial_state():
    prompt = WORLD_BUILDER_SYSTEM
    assert "interactive(" in prompt
    assert "initial_state" in prompt
    # The specific failure this guards: assuming a uniform default. A hospital
    # with some doors already open must declare them that way, because a save
    # can only record a change from what was declared.
    assert "honest" in prompt.lower()


def test_the_prompt_requires_names_on_geometry():
    prompt = WORLD_BUILDER_SYSTEM
    assert "NAME EVERYTHING" in prompt
    # The reason has to be in the brief, not just the rule: an agent told
    # "set a name" without knowing why will skip it under pressure.
    assert "positional" in prompt


def test_the_prompt_forbids_per_instance_foliage():
    # Without this a world with a grass field ships at under 10 FPS.
    prompt = WORLD_BUILDER_SYSTEM
    assert "harvestable" in prompt
    assert "per-blade" in prompt or "entity per blade" in prompt
    assert "grass" in prompt.lower()


def test_the_prompt_requires_gating_nav_links_on_doors():
    prompt = WORLD_BUILDER_SYSTEM
    assert "gated_by" in prompt
    # And says what goes wrong without it.
    assert "walk straight through" in prompt or "just shut" in prompt


def test_the_prompt_advertises_the_tags_the_engine_already_reads():
    prompt = WORLD_BUILDER_SYSTEM
    for tag in ("grabbable", "teleport_target", "mirror_visible"):
        assert tag in prompt, (
            f"the engine reads the '{tag}' tag, but the brief never tells the "
            "agent it exists, so the wiring goes unused"
        )


def test_the_prompt_still_carries_its_hard_boundaries():
    # Guard against the additions above having displaced the include
    # whitelist, which is what keeps generated world code compiling.
    prompt = WORLD_BUILDER_SYSTEM
    assert "ptv/api/IWorldBuilder.h" in prompt
    assert "PTV_REGISTER_WORLD_BUILDER" in prompt
    assert "nav_region" in prompt and "nav_link" in prompt


def test_the_buildspec_brief_covers_environments_and_perspective():
    prompt = BUILD_SPEC_SYSTEM
    for field in (
        "environment_mode",
        "reuse_environment_from",
        "player_perspective",
        "interaction_rules",
    ):
        assert field in prompt, f"BUILD_SPEC_SYSTEM never mentions {field}"


def _collapsed(text: str) -> str:
    """Whitespace-collapsed, so an assertion about wording is not really an
    assertion about where the prompt happens to line-wrap."""
    return " ".join(text.split())


def test_the_manifest_brief_covers_role_and_pose():
    prompt = ASSET_MANIFEST_SYSTEM
    assert "character_role" in prompt
    assert "t_pose" in prompt
    assert "neutral_quadruped" in prompt
    # The default that matters: always generate a head unless asked otherwise,
    # because visibility is a runtime decision and generation is not.
    assert "never generated can never be shown" in _collapsed(prompt)
