"""Gameplay code has to be told the ids the world architects invented.

The stage order used to be codegen-then-world, which meant a mechanic that
opens a door was written before any door existed. The agent could only invent
a plausible id, and an invented id is not a compile error -- it resolves to
nothing at runtime and the mechanic silently does nothing. These tests pin the
order and the context that the order exists to provide.
"""

from __future__ import annotations

import json

from ptv_orchestrator.pipeline.codegen_agent import (
    CodegenAgentInput,
    build_user_message,
)
from ptv_orchestrator.pipeline.orchestrator import _SHARED_STAGES
from ptv_orchestrator.pipeline.stage_code_generation import _load_world_context
from ptv_schemas import ProjectStage


class _Store:
    def __init__(self, project_dir):
        self._dir = project_dir

    def dir_for(self, _project_id):
        return self._dir


class _Ctx:
    def __init__(self, project_dir):
        self.project_id = "demo"
        self.store = _Store(project_dir)


def _write_world(project_dir, scene_id, doc):
    name = f"world.{scene_id}.scene.json" if scene_id else "world.scene.json"
    (project_dir / name).write_text(json.dumps(doc), encoding="utf-8")


# --- stage order ------------------------------------------------------


def test_world_generation_runs_before_code_generation():
    order = [stage for stage, _fn in _SHARED_STAGES]
    assert order.index(ProjectStage.world_generation) < order.index(
        ProjectStage.code_generation
    )


def test_asset_contract_still_precedes_code_generation():
    # The reorder must not disturb the reason asset_contract runs early:
    # the agent needs real socket names before it writes socket_world().
    order = [stage for stage, _fn in _SHARED_STAGES]
    assert order.index(ProjectStage.asset_contract) < order.index(
        ProjectStage.code_generation
    )


# --- the context itself -----------------------------------------------


def test_declared_interactives_reach_the_agent_context(tmp_path):
    _write_world(
        tmp_path,
        "hospital",
        {
            "environment_builder_id": "generated.world.demo.hospital",
            "interactives": [
                {
                    "id": "ward_door",
                    "kind": "door",
                    "initial_state": {"open": False, "angle": 0.0},
                    "locked": True,
                },
                {
                    "id": "corridor_light",
                    "kind": "switchable",
                    "initial_state": {"on": True},
                },
            ],
            "spawn_points": [{"id": "ward_entry"}],
            "portals": [
                {"id": "exit", "to_scene": "hub", "spawn_point_id": "lobby"}
            ],
            "nav_regions": [{"id": "ward"}, {"id": "corridor"}],
        },
    )

    payload = json.loads(_load_world_context(_Ctx(tmp_path)))
    assert len(payload) == 1
    env = payload[0]
    assert env["scene_id"] == "hospital"
    assert env["builder_id"] == "generated.world.demo.hospital"

    door = next(i for i in env["interactives"] if i["id"] == "ward_door")
    assert door["kind"] == "door"
    # The KEYS are what matter: an agent that assumes "is_open" instead of
    # "open" writes a read that always returns the default.
    assert door["state_keys"] == ["angle", "open"]
    assert door["locked"] is True

    assert env["spawn_points"] == ["ward_entry"]
    assert env["portals"][0]["to_scene"] == "hub"
    assert env["nav_regions"] == ["corridor", "ward"]


def test_every_environment_is_included(tmp_path):
    # A mechanic can be bound in more than one scene, so it needs all of them.
    for scene in ("hub", "hospital", "warehouse"):
        _write_world(
            tmp_path,
            scene,
            {"interactives": [{"id": f"{scene}_door", "kind": "door"}]},
        )
    payload = json.loads(_load_world_context(_Ctx(tmp_path)))
    assert sorted(e["scene_id"] for e in payload) == [
        "hospital",
        "hub",
        "warehouse",
    ]


def test_the_single_environment_layout_is_still_read(tmp_path):
    _write_world(tmp_path, "", {"interactives": [{"id": "gate", "kind": "door"}]})
    payload = json.loads(_load_world_context(_Ctx(tmp_path)))
    assert payload[0]["interactives"][0]["id"] == "gate"


def test_no_worlds_yields_no_context(tmp_path):
    # Resuming a project from before this existed must not crash or fabricate.
    assert _load_world_context(_Ctx(tmp_path)) == ""


def test_a_malformed_world_document_is_skipped(tmp_path):
    (tmp_path / "world.broken.scene.json").write_text("{ not json", encoding="utf-8")
    _write_world(tmp_path, "good", {"interactives": [{"id": "d", "kind": "door"}]})
    payload = json.loads(_load_world_context(_Ctx(tmp_path)))
    assert [e["scene_id"] for e in payload] == ["good"]


# --- the brief the agent actually reads -------------------------------


def _input(**over) -> CodegenAgentInput:
    base = dict(
        project_id="demo",
        task_id="open_ward_door",
        output_subdir="engine/generated/mechanics/demo",
        task_json="{}",
        ledger_snapshot_json="{}",
        dep_exports_json="{}",
        scene_context_json="[]",
        build_spec_excerpt="",
        instructions="sys",
    )
    base.update(over)
    return CodegenAgentInput(**base)


def test_the_message_names_the_services_that_own_state():
    msg = build_user_message(
        _input(world_context_json='[{"scene_id": "hospital"}]')
    )
    assert "GENERATED ENVIRONMENTS" in msg
    assert "hospital" in msg
    # Reading and writing object state goes through ctx.interactive...
    assert "ctx.interactive" in msg
    # ...and writing CTransform on it instead would fight the affordance
    # built-in that owns the motion.
    assert "Never write CTransform on a declared interactive" in msg
    # Object state persists on its own; saving it again is the duplicate this
    # warns against.
    assert "ctx.persistence" in msg
    # Transitions request rather than act, because tearing the world down
    # inside a mechanic tick destroys the entities being iterated.
    assert "ctx.transition->go_to" in msg
    assert "only REQUESTS" in msg


def test_the_block_is_absent_when_there_are_no_environments():
    assert "GENERATED ENVIRONMENTS" not in build_user_message(_input())
