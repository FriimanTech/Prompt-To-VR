"""Orchestrator storage + scene compile tests (no network)."""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from ptv_orchestrator.pipeline.bounds import (
    GeneratedBoundsError,
    assert_path_allowed,
    filter_cpp_only,
)
from ptv_orchestrator.pipeline.stage_scene_compile import compile_scene_json
from ptv_orchestrator.storage import ProjectStore
from ptv_schemas import (
    AssetEntry,
    AssetItem,
    AssetManifest,
    BuildSpec,
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


def _build_spec() -> BuildSpec:
    return BuildSpec(
        prompt_id="demo",
        title="Demo room",
        pitch="Grab a cube.",
        game_type=GameType.arcade,
        scenes=[SceneEntry(id="room", name="Room", summary="one room.", entry_point=True)],
        mechanics=[MechanicEntry(id="grab", name="Grab", summary="grab cubes")],
        assets=[AssetEntry(id="cube", name="Cube", category="prop", summary="wood cube")],
        acceptance_tests=[TestEntry(id="t1", description="grab works")],
    )


def _manifest() -> AssetManifest:
    return AssetManifest(
        prompt_id="demo",
        items=[
            AssetItem(
                id="cube",
                name="Cube",
                category=AssetCategory.prop,
                style_notes="wood cube",
                reference=ReferenceImageRequest(prompt="a wooden cube"),
                target_scale_meters=0.2,
            )
        ],
    )


def _plan() -> MechanicPlan:
    return MechanicPlan(
        prompt_id="demo",
        items=[
            MechanicPlanItem(
                id="grab",
                classification=MechanicClassification.builtin,
                builtin_id="builtin.grab",
            )
        ],
    )


def _scene_spec() -> SceneSpec:
    return SceneSpec(
        prompt_id="demo",
        scene_id="room",
        name="Room",
        entities=[
            EntitySpec(
                id="cube1",
                name="Cube1",
                transform=Transform(position=Vec3(x=0.0, y=1.0, z=-0.8)),
                mesh=MeshRef(asset_id="cube", glb_path="projects/demo/assets/cube/model.glb"),
                physics=PhysicsProps(kind="dynamic"),
                mechanic_bindings=[MechanicBinding(mechanic_id="builtin.grab")],
            )
        ],
    )


# ---------------------------------------------------------------------------
# Storage
# ---------------------------------------------------------------------------


def test_project_store_roundtrip(tmp_path: Path):
    store = ProjectStore(tmp_path)
    state = store.create("My VR Game", "A detailed VR prompt." * 5)
    assert (store.dir_for(state.project_id) / "prompt.txt").exists()
    loaded = store.load_state(state.project_id)
    assert loaded.project_id == state.project_id
    assert loaded.title == "My VR Game"


def test_write_and_list_artifacts(tmp_path: Path):
    store = ProjectStore(tmp_path)
    state = store.create("X", "prompt here")
    store.save_build_spec(state.project_id, _build_spec())
    store.save_asset_manifest(state.project_id, _manifest())
    arts = store.list_artifacts(state.project_id)
    names = [a.path.rsplit("/", 1)[-1] for a in arts]
    assert "build_spec.json" in names
    assert "asset_manifest.json" in names


# ---------------------------------------------------------------------------
# Scene compile
# ---------------------------------------------------------------------------


def test_scene_compile_resolves_builtin_and_mesh():
    doc = compile_scene_json(_build_spec(), _manifest(), _plan(), _scene_spec())
    assert doc["version"] == 1
    assert doc["scene_id"] == "room"
    assert doc["player"]["height_meters"] == 1.7
    ents = doc["entities"]
    assert len(ents) == 1
    e = ents[0]
    assert e["mesh"]["asset_id"] == "cube"
    assert e["mesh"]["target_scale_meters"] == 0.2
    assert e["mechanic_bindings"][0]["builtin_id"] == "builtin.grab"
    assert e["physics"]["kind"] == "dynamic"


# ---------------------------------------------------------------------------
# Generated-code bounds
# ---------------------------------------------------------------------------


def test_bounds_accepts_allowed_path(tmp_path: Path):
    gen = tmp_path / "engine" / "generated"
    (gen / "mechanics" / "proj1").mkdir(parents=True)
    path = gen / "mechanics" / "proj1" / "foo.cpp"
    assert_path_allowed(path, gen)


def test_bounds_rejects_outside(tmp_path: Path):
    gen = tmp_path / "engine" / "generated"
    with pytest.raises(GeneratedBoundsError):
        assert_path_allowed(tmp_path / "engine" / "runtime" / "foo.cpp", gen)


def test_bounds_rejects_wrong_subtree(tmp_path: Path):
    gen = tmp_path / "engine" / "generated"
    (gen / "renderer").mkdir(parents=True)
    with pytest.raises(GeneratedBoundsError):
        assert_path_allowed(gen / "renderer" / "foo.cpp", gen)


def test_filter_cpp_only_removes_disallowed_includes():
    src = (
        '#include "ptv/api/IMechanic.h"\n'
        '#include <vector>\n'
        '#include "../runtime/src/evil.h"\n'
        '#include <windows.h>\n'
        "void foo() {}\n"
    )
    out = filter_cpp_only(src)
    assert 'ptv/api/IMechanic.h' in out
    assert "vector" in out
    # The include lines for disallowed headers are replaced with a comment.
    assert '#include "../runtime/src/evil.h"' not in out
    assert '#include <windows.h>' not in out
    assert "[ptv-bounds] removed disallowed include" in out
    assert "void foo" in out


# ---------------------------------------------------------------------------
# Tripo stage: hard-fail when nothing is produced
# ---------------------------------------------------------------------------


def test_tripo_stage_fails_when_zero_models_produced(tmp_path: Path, monkeypatch):
    """If the manifest has items but Tripo returns nothing, the stage must
    raise so the pipeline halts instead of steamrolling into asset
    normalization with an empty asset set."""

    from ptv_orchestrator.events import EventBus
    from ptv_orchestrator.pipeline import stage_tripo_models
    from ptv_orchestrator.pipeline.context import PipelineContext
    from ptv_orchestrator.settings import Settings

    class _FailingTripo:
        def __init__(self, *a, **kw):  # noqa: D401 - test double
            pass

        def image_to_model(self, **kwargs):
            raise RuntimeError("no credits on account")

        def text_to_model(self, **kwargs):
            raise RuntimeError("no credits on account")

    monkeypatch.setattr(stage_tripo_models, "TripoClient", _FailingTripo)

    settings = Settings(
        repo_root=tmp_path,
        projects_dir=tmp_path / "projects",
    )
    store = ProjectStore(settings.projects_dir)
    state = store.create("Zero Tripo", "a tiny prompt for the test")
    store.save_asset_manifest(state.project_id, _manifest())

    bus = EventBus()
    ctx = PipelineContext(
        project_id=state.project_id,
        settings=settings,
        store=store,
        bus=bus,
    )

    with pytest.raises(RuntimeError, match="produced 0 models"):
        stage_tripo_models.run(ctx)


def test_filter_cpp_only_strips_hallucinated_ptv_api_headers():
    """Regression for generated code including non-existent headers like
    ``ptv/api/input.h`` or ``ptv/api/audio_capture.h``. Only the three real
    interface headers are allowed; everything else under ptv/api/ must be
    filtered out so the compiler doesn't choke on missing files."""

    src = (
        '#include "ptv/api/IMechanic.h"\n'
        '#include "ptv/api/input.h"\n'
        '#include "ptv/api/audio_capture.h"\n'
        '#include "ptv/api/haptics.h"\n'
        '#include "ptv/api/ui.h"\n'
        '#include "ptv/api/locomotion.h"\n'
        '#include "ptv/api/save_state.h"\n'
        '#include "ptv/ecs/World.h"\n'
        "void foo() {}\n"
    )
    out = filter_cpp_only(src)
    # Real headers survive.
    assert "ptv/api/IMechanic.h" in out
    assert "ptv/ecs/World.h" in out
    # Hallucinated ptv/api/ headers are stripped.
    for bad in (
        "ptv/api/input.h",
        "ptv/api/audio_capture.h",
        "ptv/api/haptics.h",
        "ptv/api/ui.h",
        "ptv/api/locomotion.h",
        "ptv/api/save_state.h",
    ):
        assert f'#include "{bad}"' not in out, bad
    assert out.count("[ptv-bounds] removed disallowed include") == 6
