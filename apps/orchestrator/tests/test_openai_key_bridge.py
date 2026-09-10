"""Every Agents-SDK stage must bridge OPENAI_API_KEY into os.environ.

Pydantic ``BaseSettings`` reads ``.env`` into ``settings.openai_api_key`` but
does NOT export it to ``os.environ``. The OpenAI Agents SDK builds its own
internal client and reads ``os.environ`` directly, so each stage that uses the
SDK has to bridge the gap itself.

``stage_npc_plan`` was missing it. That stage is the FIRST Agents-SDK stage in
a run, so on a cold orchestrator nothing had populated ``os.environ`` yet and
the NPC director failed with "Missing credentials" despite a perfectly good
key in ``.env``. It then appeared to start working, because the next stage set
the variable for the lifetime of the process -- so a deterministic bug looked
intermittent, and the only visible symptom was a warning the stage is designed
to swallow.

Five copies of one line is exactly the kind of thing that goes stale silently,
hence this test rather than care.
"""

from __future__ import annotations

import ast
from pathlib import Path

import pytest

_PIPELINE = (
    Path(__file__).resolve().parents[1] / "ptv_orchestrator" / "pipeline"
)

# Stages that construct an Agents-SDK agent. Each has a matching
# ``*_agent.py`` wrapper that imports from ``agents`` lazily.
_SDK_STAGES = [
    "stage_npc_plan.py",
    "stage_mechanic_plan.py",
    "stage_scene_spec.py",
    "stage_world_generation.py",
    "stage_code_generation.py",
]


def _source(name: str) -> str:
    path = _PIPELINE / name
    assert path.exists(), f"missing {path}"
    return path.read_text(encoding="utf-8")


@pytest.mark.parametrize("stage", _SDK_STAGES)
def test_the_stage_bridges_the_key(stage: str):
    src = _source(stage)
    assert 'os.environ["OPENAI_API_KEY"]' in src, (
        f"{stage} runs an Agents-SDK agent but never writes "
        "os.environ['OPENAI_API_KEY']. The SDK reads the environment "
        "directly, so a key that lives only in .env is invisible to it and "
        "the stage fails with 'Missing credentials'."
    )
    # Guarded on the setting, so a stage cannot blank an environment key that
    # was already exported by the shell.
    assert "ctx.settings.openai_api_key" in src, (
        f"{stage} writes the environment variable without reading the "
        "setting it is supposed to come from"
    )
    assert 'not os.environ.get("OPENAI_API_KEY")' in src, (
        f"{stage} overwrites OPENAI_API_KEY unconditionally; an existing "
        "value from the shell must win"
    )


@pytest.mark.parametrize("stage", _SDK_STAGES)
def test_the_stage_imports_os(stage: str):
    # The bridge cannot work without it, and a missing import is an
    # ImportError at module load rather than a stage failure.
    tree = ast.parse(_source(stage))
    imports_os = any(
        isinstance(node, ast.Import)
        and any(alias.name == "os" for alias in node.names)
        for node in ast.walk(tree)
    )
    assert imports_os, f"{stage} uses os.environ but does not import os"


def test_npc_plan_bridges_before_it_builds_the_director():
    # Order matters: bridging after the agent has been constructed and run
    # would be a no-op for this run, which is the failure mode being fixed.
    src = _source("stage_npc_plan.py")
    bridge = src.index('os.environ["OPENAI_API_KEY"]')
    director = src.index("director = NpcDirector(")
    assert bridge < director, (
        "the key is bridged after the NPC director is constructed, so the "
        "first run still sees no credentials"
    )
