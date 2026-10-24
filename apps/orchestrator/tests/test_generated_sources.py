"""Builds compile one game's generated C++, plus promoted mechanics it reuses.

Compiling every game together made two games' same-named tasks (both
shooters had a ``shooting_hub``) collide at link time with LNK2005.
"""

from __future__ import annotations

import json
from pathlib import Path

from ptv_orchestrator.engine_catalog import promoted_sources_for_plan
from ptv_schemas import MechanicClassification, MechanicPlan, MechanicPlanItem

REPO = Path(__file__).resolve().parents[3]


def _registry(repo: Path, items: list[dict]) -> None:
    path = repo / "engine" / "promoted" / "registry.json"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps({"version": 1, "items": items}))


def _plan(*builtin_ids: str) -> MechanicPlan:
    return MechanicPlan(prompt_id="p", items=[
        MechanicPlanItem(id=f"m{i}", classification=MechanicClassification.builtin,
                         builtin_id=b)
        for i, b in enumerate(builtin_ids)
    ])


def test_promoted_sources_only_for_reused_mechanics_from_other_games(tmp_path):
    other = tmp_path / "engine" / "generated" / "mechanics" / "old_game"
    other.mkdir(parents=True)
    for name in ("hover_register.cpp", "hover_simulation.cpp", "unrelated.cpp"):
        (other / name).write_text("//")
    _registry(tmp_path, [
        {"id": "promoted.hover", "code_task_id": "hover", "project_id": "old_game",
         "source_path": "engine/generated/mechanics/old_game/hover_register.cpp"},
        {"id": "promoted.mine", "code_task_id": "mine", "project_id": "this_game",
         "source_path": "engine/generated/mechanics/this_game/mine.cpp"},
    ])

    assert promoted_sources_for_plan(tmp_path, _plan("builtin.grab"), "this_game") == []
    got = promoted_sources_for_plan(
        tmp_path, _plan("promoted.hover", "promoted.mine"), "this_game")
    assert [Path(p).name for p in got] == ["hover_register.cpp", "hover_simulation.cpp"]


def test_desktop_cmake_compiles_one_project():
    text = (REPO / "engine" / "generated" / "CMakeLists.txt").read_text(encoding="utf-8")
    assert "${PTV_GENERATED_PROJECT_ID}/*.cpp" in text
    assert "PTV_GENERATED_EXTRA_SOURCES" in text
    android = (REPO / "engine" / "android" / "app" / "src" / "main" / "cpp"
               / "CMakeLists.txt").read_text(encoding="utf-8")
    assert "PTV_GENERATED_EXTRA_SOURCES" in android
    gradle = (REPO / "engine" / "android" / "app" / "build.gradle").read_text(encoding="utf-8")
    assert "@PTV_GENERATED_EXTRA_SOURCES@" in gradle
