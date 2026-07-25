"""Integration tests for the multi-agent code-generation stage.

These tests exercise ``stage_code_generation.run_with_runner`` with an
injected stub agent runner so the real OpenAI Agents SDK is never
called. They verify:

* multi-file ``files[]`` output is written under
  ``engine/generated/<kind>/<project_id>/`` with bounds applied
* ``depends_on`` integration: B sees A's ledger exports
* the agent's input bundle carries metadata, deps, ledger snapshot,
  and scene_spec excerpts (four-step process wiring)
* single-file fallback when the agent returns only one file
* failure handling stamps the board and other tasks continue
* prompt regression: ``CODE_TASK_SYSTEM`` no longer caps single file
  / line count
"""

from __future__ import annotations

import json
import threading
from pathlib import Path
from typing import Any

import pytest

from ptv_orchestrator.events import EventBus
from ptv_orchestrator.pipeline import stage_code_generation
from ptv_orchestrator.pipeline.codegen_agent import (
    CodegenAgentInput,
    CodegenAgentResult,
    GeneratedFile,
)
from ptv_orchestrator.pipeline.context import PipelineContext
from ptv_orchestrator.pipeline.prompts import CODE_TASK_SYSTEM
from ptv_orchestrator.settings import Settings
from ptv_orchestrator.storage import ProjectStore
from ptv_schemas import (
    AssetEntry,
    AssetItem,
    AssetManifest,
    BuildSpec,
    CodeTask,
    CodeTaskKind,
    MechanicBinding,
    MechanicClassification,
    MechanicEntry,
    MechanicPlan,
    MechanicPlanItem,
    ReferenceImageRequest,
    SceneEntry,
    SceneSpec,
    TestEntry,
    Transform,
)
from ptv_schemas.asset_manifest import AssetCategory
from ptv_schemas.build_spec import GameType
from ptv_schemas.common import Vec3
from ptv_schemas.scene_spec import EntitySpec, MeshRef, PhysicsProps


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------


def _seed(
    tmp_path: Path, plan: MechanicPlan, *, verify_compile: bool = False
) -> tuple[ProjectStore, str, Settings]:
    settings = Settings(
        projects_dir=tmp_path / "projects",
        engine_dir=tmp_path / "engine",
        samples_dir=tmp_path / "samples",
        repo_root=tmp_path,
        codegen_max_parallel_agents=3,
        # Compile verification runs a real cmake build; disable it by default
        # in tests and exercise it explicitly with an injected stub verifier.
        codegen_verify_compile=verify_compile,
    )
    store = ProjectStore(settings.projects_dir)
    state = store.create("Demo", "prompt here")

    bs = BuildSpec(
        prompt_id="demo",
        title="Demo",
        pitch="A tiny demo.",
        game_type=GameType.arcade,
        scenes=[SceneEntry(id="room", name="Room", summary="one room.", entry_point=True)],
        mechanics=[
            MechanicEntry(id="shooting_hub", name="Shooting Hub", summary="route trigger to weapon profile"),
            MechanicEntry(id="repulsor_blast", name="Repulsor", summary="palm beam"),
        ],
        assets=[AssetEntry(id="gauntlet", name="Gauntlet", category="prop", summary="iron-man gauntlet")],
        acceptance_tests=[TestEntry(id="t1", description="shooting_hub fires correctly")],
    )
    store.save_build_spec(state.project_id, bs)

    manifest = AssetManifest(
        prompt_id="demo",
        items=[
            AssetItem(
                id="gauntlet",
                name="Gauntlet",
                category=AssetCategory.prop,
                style_notes="gold and red gauntlet",
                reference=ReferenceImageRequest(prompt="iron man gauntlet"),
                target_scale_meters=0.3,
            )
        ],
    )
    store.save_asset_manifest(state.project_id, manifest)
    store.save_mechanic_plan(state.project_id, plan)

    scene = SceneSpec(
        prompt_id="demo",
        scene_id="room",
        name="Room",
        entities=[
            EntitySpec(
                id="player_rig",
                name="Player Rig",
                transform=Transform(position=Vec3(x=0.0, y=0.0, z=0.0)),
                tags=["player_avatar"],
            ),
            EntitySpec(
                id="gauntlet_right",
                name="Gauntlet Right",
                transform=Transform(position=Vec3(x=0.6, y=1.0, z=-0.5)),
                mesh=MeshRef(asset_id="gauntlet", glb_path="projects/demo/assets/gauntlet/model.glb"),
                physics=PhysicsProps(kind="dynamic"),
                tags=["interactable", "grabbable", "wearable", "weapon"],
                mechanic_bindings=[
                    MechanicBinding(mechanic_id="generated.shooting_hub"),
                    MechanicBinding(
                        mechanic_id="generated.repulsor_blast",
                        params={"side": "right"},
                    ),
                ],
            ),
        ],
    )
    store.save_scene_spec(state.project_id, scene, filename="scene_spec.room.json")
    store.save_scene_spec(state.project_id, scene, filename="scene_spec.json")

    return store, state.project_id, settings


def _task(task_id: str, *, depends_on: list[str] | None = None) -> CodeTask:
    return CodeTask(
        id=task_id,
        kind=CodeTaskKind.mechanic,
        title=f"Task {task_id}",
        description=f"Implement {task_id}.",
        interface="IMechanic",
        depends_on=list(depends_on or []),
    )


def _generate_item(task_id: str, *, depends_on: list[str] | None = None) -> MechanicPlanItem:
    return MechanicPlanItem(
        id=task_id,
        classification=MechanicClassification.generate,
        code_task=_task(task_id, depends_on=depends_on),
    )


# ---------------------------------------------------------------------------
# Stub runners
# ---------------------------------------------------------------------------


def _make_multi_file_runner(captured: list[CodegenAgentInput]):
    """Stub runner that returns 3 files per task and records every input
    bundle it received."""

    def runner(inp: CodegenAgentInput, model: str) -> CodegenAgentResult:
        captured.append(inp)
        return CodegenAgentResult(
            files=[
                GeneratedFile(
                    path=f"engine/generated/mechanics/{inp.project_id}/{inp.task_id}_register.cpp",
                    contents=_minimal_cpp(inp.task_id, "register"),
                ),
                GeneratedFile(
                    path=f"engine/generated/mechanics/{inp.project_id}/{inp.task_id}_simulation.cpp",
                    contents=_minimal_cpp(inp.task_id, "sim"),
                ),
                GeneratedFile(
                    path=f"engine/generated/mechanics/{inp.project_id}/{inp.task_id}_helpers.cpp",
                    contents=_minimal_cpp(inp.task_id, "helpers"),
                ),
            ],
            exports={
                "registered_mechanic_id": f"generated.{inp.task_id}",
                "weapon_profile_macro": "PTV_REGISTER_WEAPON_PROFILE",
            },
            notes=f"stub run for {inp.task_id}",
        )

    return runner


def _minimal_cpp(task_id: str, suffix: str) -> str:
    return (
        '#include "ptv/api/IMechanic.h"\n'
        '#include "ptv/ecs/World.h"\n'
        "#include <nlohmann/json.hpp>\n"
        "#include <string>\n"
        "\n"
        "namespace ptv {\n"
        f"// {suffix} portion of {task_id}\n"
        f"class {_pascal(task_id)}_{suffix} : public IMechanic {{\n"
        " public:\n"
        "  void on_attach(World&, std::uint32_t, const nlohmann::json&) override {}\n"
        "  void on_update(World&, std::uint32_t, float) override {}\n"
        "  void on_detach(World&, std::uint32_t) override {}\n"
        f'  std::string id() const override {{ return "generated.{task_id}"; }}\n'
        "};\n"
        "}\n"
    )


def _pascal(s: str) -> str:
    return "".join(p.title() for p in s.replace("-", "_").split("_"))


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------


def test_multi_file_output_written_to_disk(tmp_path: Path) -> None:
    plan = MechanicPlan(
        prompt_id="demo",
        items=[_generate_item("shooting_hub")],
    )
    store, pid, settings = _seed(tmp_path, plan)
    ctx = PipelineContext(
        project_id=pid, settings=settings, store=store, bus=EventBus()
    )

    captured: list[CodegenAgentInput] = []
    runner = _make_multi_file_runner(captured)

    result = stage_code_generation.run_with_runner(ctx, agent_runner=runner)

    assert len(captured) == 1

    base = tmp_path / "engine" / "generated" / "mechanics" / pid
    for suffix in ("register", "simulation", "helpers"):
        p = base / f"shooting_hub_{suffix}.cpp"
        assert p.exists(), f"expected {p} on disk"
        text = p.read_text(encoding="utf-8")
        assert "PTV" not in text or "ptv::IMechanic" not in text or "ptv/api/IMechanic.h" in text

    # Board reflects done state with files_written + exports.
    board_path = store.dir_for(pid) / "codegen_board.json"
    board = json.loads(board_path.read_text(encoding="utf-8"))
    row = board["tasks"]["shooting_hub"]
    assert row["status"] == "done"
    assert len(row["files_written"]) == 3
    assert row["exports"]["registered_mechanic_id"] == "generated.shooting_hub"

    # StageResult artifacts include the 3 cpp files + sidecar.
    assert any(p.endswith("shooting_hub_register.cpp") for p in result.artifacts)
    assert any(p.endswith(".codetask.json") for p in result.artifacts)


def test_depends_on_blocks_until_dep_done(tmp_path: Path) -> None:
    plan = MechanicPlan(
        prompt_id="demo",
        items=[
            _generate_item("shooting_hub"),
            _generate_item("repulsor_blast", depends_on=["shooting_hub"]),
        ],
    )
    store, pid, settings = _seed(tmp_path, plan)
    ctx = PipelineContext(
        project_id=pid, settings=settings, store=store, bus=EventBus()
    )

    captured: list[CodegenAgentInput] = []

    # Use a runner that asserts ordering: when repulsor_blast runs, the
    # shooting_hub row must already be ``done`` in the ledger snapshot
    # the worker handed to the agent.
    ordering_lock = threading.Lock()
    order: list[str] = []

    def runner(inp: CodegenAgentInput, model: str) -> CodegenAgentResult:
        captured.append(inp)
        with ordering_lock:
            order.append(inp.task_id)
        snapshot = json.loads(inp.ledger_snapshot_json)
        if inp.task_id == "repulsor_blast":
            assert snapshot["tasks"]["shooting_hub"]["status"] == "done", (
                f"expected shooting_hub=done at repulsor_blast claim time, got "
                f"{snapshot['tasks']['shooting_hub']['status']}"
            )
            dep_exports = json.loads(inp.dep_exports_json)
            assert dep_exports["shooting_hub"]["exports"]["registered_mechanic_id"] == "generated.shooting_hub"
        return CodegenAgentResult(
            files=[
                GeneratedFile(
                    path=(
                        f"engine/generated/mechanics/{inp.project_id}/{inp.task_id}.cpp"
                    ),
                    contents=_minimal_cpp(inp.task_id, "all"),
                ),
            ],
            exports={"registered_mechanic_id": f"generated.{inp.task_id}"},
        )

    stage_code_generation.run_with_runner(ctx, agent_runner=runner)

    # shooting_hub must run before repulsor_blast even with 3 parallel workers.
    assert order.index("shooting_hub") < order.index("repulsor_blast")


def test_dependent_task_may_include_dependency_header(tmp_path: Path) -> None:
    """A task that declares ``depends_on: ['shooting_hub']`` may
    ``#include "shooting_hub_shared.h"`` even though it does NOT emit that
    header itself -- the dependency owns it and is built first. This is the
    regression guard for the pistol_fire_profile include-violation bug."""
    plan = MechanicPlan(
        prompt_id="demo",
        items=[
            _generate_item("shooting_hub"),
            _generate_item("pistol_fire_profile", depends_on=["shooting_hub"]),
        ],
    )
    store, pid, settings = _seed(tmp_path, plan)
    ctx = PipelineContext(
        project_id=pid, settings=settings, store=store, bus=EventBus()
    )

    def _hub_header() -> str:
        return (
            "#pragma once\n"
            "#include <nlohmann/json.hpp>\n"
            "namespace ptv {\n"
            "struct WeaponFireProfile { float fire_rate_rps{3.0f}; };\n"
            "}\n"
        )

    def runner(inp: CodegenAgentInput, model: str) -> CodegenAgentResult:
        if inp.task_id == "shooting_hub":
            return CodegenAgentResult(
                files=[
                    GeneratedFile(
                        path=f"engine/generated/mechanics/{inp.project_id}/shooting_hub_register.cpp",
                        contents=(
                            '#include "ptv/api/IMechanic.h"\n'
                            '#include "ptv/ecs/World.h"\n'
                            '#include "shooting_hub_shared.h"\n'
                            "namespace ptv {\n"
                            "class ShootingHub : public IMechanic {\n"
                            " public:\n"
                            "  void on_attach(World&, std::uint32_t, const nlohmann::json&) override {}\n"
                            "  void on_update(World&, std::uint32_t, float, const InputSnapshot&) override {}\n"
                            "  void on_detach(World&, std::uint32_t) override {}\n"
                            '  std::string id() const override { return "generated.shooting_hub"; }\n'
                            "};\n"
                            'PTV_REGISTER_MECHANIC("generated.shooting_hub", ShootingHub)\n'
                            "}\n"
                        ),
                    ),
                    GeneratedFile(
                        path=f"engine/generated/mechanics/{inp.project_id}/shooting_hub_shared.h",
                        contents=_hub_header(),
                    ),
                ],
                exports={"registered_mechanic_id": "generated.shooting_hub"},
            )
        # pistol_fire_profile: include the DEPENDENCY's header (not emitted here).
        return CodegenAgentResult(
            files=[
                GeneratedFile(
                    path=f"engine/generated/mechanics/{inp.project_id}/pistol_fire_profile_register.cpp",
                    contents=(
                        '#include "ptv/api/IMechanic.h"\n'
                        '#include "ptv/ecs/World.h"\n'
                        '#include "shooting_hub_shared.h"\n'
                        "namespace ptv {\n"
                        "class PistolFireProfile : public IMechanic {\n"
                        " public:\n"
                        "  void on_attach(World&, std::uint32_t, const nlohmann::json&) override {}\n"
                        "  void on_update(World&, std::uint32_t, float, const InputSnapshot&) override {}\n"
                        "  void on_detach(World&, std::uint32_t) override {}\n"
                        '  std::string id() const override { return "generated.pistol_fire_profile"; }\n'
                        "};\n"
                        'PTV_REGISTER_MECHANIC("generated.pistol_fire_profile", PistolFireProfile)\n'
                        "}\n"
                    ),
                ),
            ],
            exports={"registered_mechanic_id": "generated.pistol_fire_profile"},
        )

    stage_code_generation.run_with_runner(ctx, agent_runner=runner)

    board = json.loads(
        (store.dir_for(pid) / "codegen_board.json").read_text(encoding="utf-8")
    )
    # Both tasks succeeded (the profile was NOT rejected for including the
    # dependency's header).
    assert board["tasks"]["shooting_hub"]["status"] == "done"
    assert board["tasks"]["pistol_fire_profile"]["status"] == "done"

    # The profile file is on disk and kept its dependency include.
    profile = (
        tmp_path
        / "engine" / "generated" / "mechanics" / pid
        / "pistol_fire_profile_register.cpp"
    )
    assert profile.exists()
    assert '#include "shooting_hub_shared.h"' in profile.read_text(encoding="utf-8")


def test_undeclared_cross_task_header_still_rejected(tmp_path: Path) -> None:
    """Including another task's header WITHOUT declaring depends_on is still
    a violation (preserves the DAG-ordering guarantee)."""
    plan = MechanicPlan(
        prompt_id="demo",
        items=[
            _generate_item("shooting_hub"),
            _generate_item("pistol_fire_profile"),  # NO depends_on
        ],
    )
    store, pid, settings = _seed(tmp_path, plan)
    ctx = PipelineContext(
        project_id=pid, settings=settings, store=store, bus=EventBus()
    )

    def runner(inp: CodegenAgentInput, model: str) -> CodegenAgentResult:
        if inp.task_id == "shooting_hub":
            return CodegenAgentResult(
                files=[
                    GeneratedFile(
                        path=f"engine/generated/mechanics/{inp.project_id}/shooting_hub_shared.h",
                        contents="#pragma once\nnamespace ptv { struct X {}; }\n",
                    ),
                    GeneratedFile(
                        path=f"engine/generated/mechanics/{inp.project_id}/shooting_hub.cpp",
                        contents=_minimal_cpp("shooting_hub", "all"),
                    ),
                ],
                exports={"registered_mechanic_id": "generated.shooting_hub"},
            )
        return CodegenAgentResult(
            files=[
                GeneratedFile(
                    path=f"engine/generated/mechanics/{inp.project_id}/pistol_fire_profile.cpp",
                    contents=(
                        '#include "ptv/api/IMechanic.h"\n'
                        '#include "shooting_hub_shared.h"\n'
                        "namespace ptv {}\n"
                    ),
                ),
            ],
            exports={},
        )

    # No compile verification here; we only care about include validation.
    settings.codegen_verify_compile = False
    with pytest.raises(RuntimeError, match="failed task"):
        stage_code_generation.run_with_runner(ctx, agent_runner=runner)

    board = json.loads(
        (store.dir_for(pid) / "codegen_board.json").read_text(encoding="utf-8")
    )
    assert board["tasks"]["pistol_fire_profile"]["status"] == "failed"
    assert "shooting_hub_shared.h" in (
        board["tasks"]["pistol_fire_profile"]["error"] or ""
    )


def test_single_file_fallback(tmp_path: Path) -> None:
    plan = MechanicPlan(
        prompt_id="demo", items=[_generate_item("solo")]
    )
    store, pid, settings = _seed(tmp_path, plan)
    ctx = PipelineContext(
        project_id=pid, settings=settings, store=store, bus=EventBus()
    )

    def runner(inp: CodegenAgentInput, model: str) -> CodegenAgentResult:
        return CodegenAgentResult(
            files=[
                GeneratedFile(
                    path=(
                        f"engine/generated/mechanics/{inp.project_id}/solo.cpp"
                    ),
                    contents=_minimal_cpp("solo", "only"),
                )
            ],
            exports={"registered_mechanic_id": "generated.solo"},
        )

    stage_code_generation.run_with_runner(ctx, agent_runner=runner)
    base = tmp_path / "engine" / "generated" / "mechanics" / pid
    files = sorted(p.name for p in base.glob("*.cpp"))
    assert files == ["solo.cpp"]


def test_failure_stamps_board_and_propagates(tmp_path: Path) -> None:
    plan = MechanicPlan(
        prompt_id="demo",
        items=[_generate_item("good"), _generate_item("bad")],
    )
    store, pid, settings = _seed(tmp_path, plan)
    ctx = PipelineContext(
        project_id=pid, settings=settings, store=store, bus=EventBus()
    )

    def runner(inp: CodegenAgentInput, model: str) -> CodegenAgentResult:
        if inp.task_id == "bad":
            raise RuntimeError("synthetic agent failure")
        return CodegenAgentResult(
            files=[
                GeneratedFile(
                    path=(
                        f"engine/generated/mechanics/{inp.project_id}/{inp.task_id}.cpp"
                    ),
                    contents=_minimal_cpp(inp.task_id, "only"),
                )
            ],
            exports={"registered_mechanic_id": f"generated.{inp.task_id}"},
        )

    # The stage raises after recording the failure to bubble into the
    # top-level pipeline runner.
    with pytest.raises(RuntimeError, match="failed task"):
        stage_code_generation.run_with_runner(ctx, agent_runner=runner)

    board = json.loads(
        (store.dir_for(pid) / "codegen_board.json").read_text(encoding="utf-8")
    )
    assert board["tasks"]["good"]["status"] == "done"
    assert board["tasks"]["bad"]["status"] == "failed"
    assert "synthetic agent failure" in board["tasks"]["bad"]["error"]


def test_four_step_agent_context_is_wired(tmp_path: Path) -> None:
    """The agent input bundle must include: full CodeTask, ledger
    snapshot, dep_exports, scene_spec excerpts, and CODE_TASK_SYSTEM
    instructions -- everything the four-step process needs."""

    plan = MechanicPlan(
        prompt_id="demo",
        items=[
            _generate_item("shooting_hub"),
            _generate_item("repulsor_blast", depends_on=["shooting_hub"]),
        ],
    )
    store, pid, settings = _seed(tmp_path, plan)
    ctx = PipelineContext(
        project_id=pid, settings=settings, store=store, bus=EventBus()
    )

    captured: dict[str, CodegenAgentInput] = {}

    def runner(inp: CodegenAgentInput, model: str) -> CodegenAgentResult:
        captured[inp.task_id] = inp
        return CodegenAgentResult(
            files=[
                GeneratedFile(
                    path=(
                        f"engine/generated/mechanics/{inp.project_id}/{inp.task_id}.cpp"
                    ),
                    contents=_minimal_cpp(inp.task_id, "all"),
                )
            ],
            exports={"registered_mechanic_id": f"generated.{inp.task_id}"},
        )

    stage_code_generation.run_with_runner(ctx, agent_runner=runner)

    # Both tasks were observed.
    assert set(captured.keys()) == {"shooting_hub", "repulsor_blast"}

    rep = captured["repulsor_blast"]

    # 1) Task metadata is the full JSON.
    task = json.loads(rep.task_json)
    assert task["id"] == "repulsor_blast"
    assert task["depends_on"] == ["shooting_hub"]
    assert task["interface"] == "IMechanic"

    # 2) Ledger snapshot lists every task.
    snapshot = json.loads(rep.ledger_snapshot_json)
    assert set(snapshot["tasks"].keys()) == {"shooting_hub", "repulsor_blast"}

    # 3) Dep exports populated.
    dep_exports = json.loads(rep.dep_exports_json)
    assert dep_exports["shooting_hub"]["exports"]["registered_mechanic_id"] == "generated.shooting_hub"

    # 4) Scene context references both player_rig and gauntlet_right.
    scene_ctx = json.loads(rep.scene_context_json)
    assert isinstance(scene_ctx, list) and len(scene_ctx) >= 1
    found_rel = False
    for excerpt in scene_ctx:
        for ent in excerpt.get("relevant_entities", []):
            if ent["id"] == "gauntlet_right":
                found_rel = True
    assert found_rel, "gauntlet_right with generated.repulsor_blast binding must appear in scene context"

    # 5) Instructions are CODE_TASK_SYSTEM (the four-step contract).
    assert rep.instructions == CODE_TASK_SYSTEM

    # 6) output_subdir is the resolved kind directory plus project id.
    assert rep.output_subdir == f"engine/generated/mechanics/{pid}"


def test_code_edit_task_injects_base_builtin_source(tmp_path: Path) -> None:
    """A ``builtin+config_code_edit`` item must cause the base built-in's
    source to be injected into the agent's ``base_source_block`` so it can
    copy-and-edit rather than rewrite from scratch."""

    plan = MechanicPlan(
        prompt_id="demo",
        items=[
            MechanicPlanItem(
                id="custom_grab",
                classification=MechanicClassification.builtin_config_code_edit,
                base_builtin_id="builtin.grab",
                code_task=_task("custom_grab"),
            )
        ],
    )
    store, pid, settings = _seed(tmp_path, plan)

    # Provide a fake BuiltinMechanics.cpp so the source resolver finds it.
    builtin_cpp = (
        tmp_path / "engine" / "runtime" / "src" / "mechanics" / "BuiltinMechanics.cpp"
    )
    builtin_cpp.parent.mkdir(parents=True, exist_ok=True)
    builtin_cpp.write_text(
        "// GrabMechanic base source\nclass GrabMechanic {};\n", encoding="utf-8"
    )

    ctx = PipelineContext(
        project_id=pid, settings=settings, store=store, bus=EventBus()
    )

    captured: list[CodegenAgentInput] = []
    runner = _make_multi_file_runner(captured)

    stage_code_generation.run_with_runner(ctx, agent_runner=runner)

    assert len(captured) == 1
    inp = captured[0]
    # Base source was injected (from the fake BuiltinMechanics.cpp) plus the
    # engine-owned-behavior NOTE for grab.
    assert "GrabMechanic base source" in inp.base_source_block
    assert "engine-owned behavior" in inp.base_source_block
    # An extra instruction tells the agent to copy + adapt under a new id.
    assert any(
        "CODE-EDIT TASK" in line and "generated.custom_grab" in line
        for line in inp.extra_instructions
    )


def test_three_parallel_workers_drain_board(tmp_path: Path) -> None:
    """With 3 workers and 6 independent tasks, the board fully drains
    and every task is claimed exactly once."""

    plan = MechanicPlan(
        prompt_id="demo",
        items=[_generate_item(f"task_{i}") for i in range(6)],
    )
    store, pid, settings = _seed(tmp_path, plan)
    # Force exactly 3 parallel agents.
    settings.codegen_max_parallel_agents = 3
    ctx = PipelineContext(
        project_id=pid, settings=settings, store=store, bus=EventBus()
    )

    seen_lock = threading.Lock()
    seen: list[str] = []

    def runner(inp: CodegenAgentInput, model: str) -> CodegenAgentResult:
        with seen_lock:
            seen.append(inp.task_id)
        return CodegenAgentResult(
            files=[
                GeneratedFile(
                    path=(
                        f"engine/generated/mechanics/{inp.project_id}/{inp.task_id}.cpp"
                    ),
                    contents=_minimal_cpp(inp.task_id, "only"),
                )
            ],
            exports={"registered_mechanic_id": f"generated.{inp.task_id}"},
        )

    stage_code_generation.run_with_runner(ctx, agent_runner=runner)

    assert sorted(seen) == [f"task_{i}" for i in range(6)]
    board = json.loads(
        (store.dir_for(pid) / "codegen_board.json").read_text(encoding="utf-8")
    )
    assert board["max_agents"] == 3
    assert all(row["status"] == "done" for row in board["tasks"].values())


# ---------------------------------------------------------------------------
# Compile-verification + repair loop
# ---------------------------------------------------------------------------


def test_verify_compile_repairs_then_succeeds(tmp_path: Path) -> None:
    """A generated file that fails to compile is regenerated with the
    compiler errors as context, then the rebuild succeeds."""
    plan = MechanicPlan(prompt_id="demo", items=[_generate_item("solo")])
    store, pid, settings = _seed(tmp_path, plan, verify_compile=True)
    ctx = PipelineContext(
        project_id=pid, settings=settings, store=store, bus=EventBus()
    )

    calls: list[CodegenAgentInput] = []

    def runner(inp: CodegenAgentInput, model: str) -> CodegenAgentResult:
        calls.append(inp)
        return CodegenAgentResult(
            files=[
                GeneratedFile(
                    path=f"engine/generated/mechanics/{inp.project_id}/solo.cpp",
                    contents=_minimal_cpp("solo", "only"),
                )
            ],
            exports={"registered_mechanic_id": "generated.solo"},
        )

    rel = f"engine/generated/mechanics/{pid}/solo.cpp"
    rounds = {"n": 0}

    def verifier() -> tuple[bool, str]:
        rounds["n"] += 1
        if rounds["n"] == 1:
            return False, f"C:/repo/{rel}(12): error C2065: 'foo': undeclared identifier"
        return True, "build ok"

    stage_code_generation.run_with_runner(
        ctx, agent_runner=runner, compile_verifier=verifier
    )

    # Verifier ran twice (fail -> ok); agent ran twice (initial + repair).
    assert rounds["n"] == 2
    assert len(calls) == 2
    assert calls[0].repair_context == ""
    assert "error C2065" in calls[1].repair_context
    assert "CURRENT FILES ON DISK" in calls[1].repair_context

    board = json.loads(
        (store.dir_for(pid) / "codegen_board.json").read_text(encoding="utf-8")
    )
    assert board["tasks"]["solo"]["status"] == "done"


def test_verify_compile_gives_up_after_budget(tmp_path: Path) -> None:
    """When the C++ never compiles, the stage exhausts its repair budget,
    marks the task failed, and raises so the build stage never runs."""
    plan = MechanicPlan(prompt_id="demo", items=[_generate_item("solo")])
    store, pid, settings = _seed(tmp_path, plan, verify_compile=True)
    settings.codegen_max_repair_rounds = 1
    ctx = PipelineContext(
        project_id=pid, settings=settings, store=store, bus=EventBus()
    )

    def runner(inp: CodegenAgentInput, model: str) -> CodegenAgentResult:
        return CodegenAgentResult(
            files=[
                GeneratedFile(
                    path=f"engine/generated/mechanics/{inp.project_id}/solo.cpp",
                    contents=_minimal_cpp("solo", "only"),
                )
            ],
            exports={"registered_mechanic_id": "generated.solo"},
        )

    rel = f"engine/generated/mechanics/{pid}/solo.cpp"
    rounds = {"n": 0}

    def verifier() -> tuple[bool, str]:
        rounds["n"] += 1
        return False, f"C:/repo/{rel}(3): error C1004: unexpected end-of-file found"

    with pytest.raises(RuntimeError, match="failed task"):
        stage_code_generation.run_with_runner(
            ctx, agent_runner=runner, compile_verifier=verifier
        )

    # max_repair_rounds=1 -> verifier called on round 0 (repair) and round 1 (give up).
    assert rounds["n"] == 2
    board = json.loads(
        (store.dir_for(pid) / "codegen_board.json").read_text(encoding="utf-8")
    )
    assert board["tasks"]["solo"]["status"] == "failed"
    assert "compile verification failed" in (board["tasks"]["solo"]["error"] or "")


def test_verify_compile_disabled_skips_build(tmp_path: Path) -> None:
    """With verification off (default in tests), no verifier is invoked."""
    plan = MechanicPlan(prompt_id="demo", items=[_generate_item("solo")])
    store, pid, settings = _seed(tmp_path, plan)  # verify_compile=False
    ctx = PipelineContext(
        project_id=pid, settings=settings, store=store, bus=EventBus()
    )

    def runner(inp: CodegenAgentInput, model: str) -> CodegenAgentResult:
        return CodegenAgentResult(
            files=[
                GeneratedFile(
                    path=f"engine/generated/mechanics/{inp.project_id}/solo.cpp",
                    contents=_minimal_cpp("solo", "only"),
                )
            ],
            exports={"registered_mechanic_id": "generated.solo"},
        )

    called = {"n": 0}

    def verifier() -> tuple[bool, str]:
        called["n"] += 1
        return True, ""

    stage_code_generation.run_with_runner(
        ctx, agent_runner=runner, compile_verifier=verifier
    )
    # Verification disabled -> the injected verifier is never called.
    assert called["n"] == 0


# ---------------------------------------------------------------------------
# Prompt regression tests
# ---------------------------------------------------------------------------


def test_code_task_system_does_not_force_single_file() -> None:
    """``CODE_TASK_SYSTEM`` must not require single-file output."""
    bad_phrases = [
        "Return ONLY the contents of one .cpp",
        "Return ONLY the contents of one .cpp file",
        "no multiple files",
        "single self-contained source file",
        "Keep the file under 300 lines",
    ]
    for phrase in bad_phrases:
        assert phrase not in CODE_TASK_SYSTEM, (
            f"CODE_TASK_SYSTEM still contains banned phrase: {phrase!r}"
        )


def test_code_task_system_mentions_four_step_and_multifile() -> None:
    """Positive regression: prompt should advertise the four-step
    process and multi-file output."""
    for needle in (
        "four-step",
        "files[]",
        "web_search",
        "depends_on",
        "Single-file output is a FALLBACK",
    ):
        assert needle in CODE_TASK_SYSTEM, (
            f"CODE_TASK_SYSTEM missing required phrase: {needle!r}"
        )


def test_mechanic_plan_system_mentions_depends_on() -> None:
    from ptv_orchestrator.pipeline.prompts import MECHANIC_PLAN_SYSTEM

    assert "depends_on" in MECHANIC_PLAN_SYSTEM
    assert "TEST FILES ARE UNUSED" in MECHANIC_PLAN_SYSTEM or "test_files" in MECHANIC_PLAN_SYSTEM


def test_build_user_message_uses_resolved_subdir() -> None:
    """``build_user_message`` must inject the resolved
    engine/generated/<kind>/<project_id> path, not a literal
    ``<kind>`` placeholder."""
    from ptv_orchestrator.pipeline.codegen_agent import (
        CodegenAgentInput,
        build_user_message,
    )

    inp = CodegenAgentInput(
        project_id="proj_xyz",
        task_id="shooting_hub",
        output_subdir="engine/generated/mechanics/proj_xyz",
        task_json="{}",
        ledger_snapshot_json="{}",
        dep_exports_json="{}",
        scene_context_json="[]",
        build_spec_excerpt="(none)",
        instructions="(stub)",
    )
    msg = build_user_message(inp)
    assert "engine/generated/mechanics/proj_xyz/" in msg
    assert "<kind>" not in msg
    # Four-step language must be present.
    assert "four-step process" in msg
    assert "web_search" in msg
    assert "1..16 entries" in msg


def test_configure_and_build_clean_first_appends_flag(monkeypatch) -> None:
    """The codegen compile-verify must force a full recompile so a stale
    ``.obj`` cannot mask a compile error (the shooting_hub false-green
    regression). ``clean_first=True`` must append ``--clean-first`` to the
    ``cmake --build`` invocation."""
    from ptv_orchestrator.build import cmake_runner

    captured: list[list[str]] = []

    monkeypatch.setattr(cmake_runner, "find_cmake", lambda: "cmake")

    def fake_run(cmd, *, cwd, on_line):
        captured.append(list(cmd))
        return 0

    monkeypatch.setattr(cmake_runner, "_run", fake_run)

    cmake_runner.configure_and_build(
        source_dir=Path("."),
        build_dir=Path("."),
        generator="Ninja",
        platform="",
        config="Release",
        vcpkg_root=None,
        build_target="ptv_starter_room",
        clean_first=True,
    )

    build_cmds = [c for c in captured if "--build" in c]
    assert build_cmds, "expected a cmake --build invocation"
    assert "--clean-first" in build_cmds[-1]
    assert "--target" in build_cmds[-1]


def test_configure_and_build_without_clean_first_omits_flag(monkeypatch) -> None:
    from ptv_orchestrator.build import cmake_runner

    captured: list[list[str]] = []
    monkeypatch.setattr(cmake_runner, "find_cmake", lambda: "cmake")
    monkeypatch.setattr(
        cmake_runner, "_run", lambda cmd, *, cwd, on_line: captured.append(list(cmd)) or 0
    )

    cmake_runner.configure_and_build(
        source_dir=Path("."),
        build_dir=Path("."),
        generator="Ninja",
        platform="",
        config="Release",
        vcpkg_root=None,
    )

    build_cmds = [c for c in captured if "--build" in c]
    assert build_cmds
    assert "--clean-first" not in build_cmds[-1]


def test_code_task_system_enforces_consistency_and_full_repair() -> None:
    """Regression for the shooting_hub partial-repair break: the prompt must
    require same-task naming consistency and whole-file-set repairs."""
    for needle in (
        "SAME-TASK INTERNAL CONSISTENCY",
        "REPAIR = REGENERATE THE WHOLE FILE SET",
    ):
        assert needle in CODE_TASK_SYSTEM, (
            f"CODE_TASK_SYSTEM missing required rule: {needle!r}"
        )


def test_world_builder_system_documents_meshdesc_flat_floats() -> None:
    """The world prompt must teach the flat std::vector<float> MeshDesc layout
    so the architect does not use std::vector<Vec3> (attempt-1 failure)."""
    from ptv_orchestrator.pipeline.prompts import WORLD_BUILDER_SYSTEM

    assert "MeshDesc" in WORLD_BUILDER_SYSTEM
    assert "std::vector<float>" in WORLD_BUILDER_SYSTEM
    assert "NOT ``std::vector<Vec3>``" in WORLD_BUILDER_SYSTEM
