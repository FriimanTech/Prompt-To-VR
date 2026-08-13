"""Orchestrator storage + scene compile tests (no network)."""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from ptv_orchestrator.pipeline.bounds import (
    _FORBIDDEN_STD_HEADERS,
    _HEADER_WHITELIST_EXACT,
    GeneratedBoundsError,
    assert_path_allowed,
    filter_cpp_only,
    repair_generated_cpp,
    validate_includes,
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


def test_list_artifacts_prunes_build_intermediates(tmp_path: Path):
    """Regression for the alien-shooter ``list_artifacts`` PermissionError
    spam: compiler intermediates under ``builds/.../<target>.dir/`` (locked
    ``*.obj`` files) and vcpkg/CMake bookkeeping must NOT be walked or listed
    as project artifacts. Final binaries that live directly under a config
    output dir stay listed."""
    store = ProjectStore(tmp_path)
    state = store.create("X", "prompt here")
    store.save_build_spec(state.project_id, _build_spec())
    root = store.dir_for(state.project_id)

    # Build-intermediate files that previously tripped PermissionError / noise.
    obj_dir = (
        root / "builds" / "cmake-current" / "runtime"
        / "ptv_runtime.dir" / "Release"
    )
    obj_dir.mkdir(parents=True)
    (obj_dir / "game_over_and_restart_runtime.obj").write_bytes(b"\x00obj")
    (root / "builds" / "cmake-current" / "CMakeFiles").mkdir(parents=True)
    (root / "builds" / "cmake-current" / "CMakeFiles" / "x.stamp").write_text("s")
    (root / "builds" / "cmake-current" / "vcpkg_installed").mkdir(parents=True)
    (root / "builds" / "cmake-current" / "vcpkg_installed" / "y.h").write_text("h")

    # A real final binary that SHOULD still be surfaced.
    bin_dir = root / "builds" / "cmake-current" / "samples" / "starter-room" / "Release"
    bin_dir.mkdir(parents=True)
    (bin_dir / "ptv_starter_room.exe").write_bytes(b"MZ")

    arts = store.list_artifacts(state.project_id)
    names = {a.path.rsplit("/", 1)[-1] for a in arts}

    assert "build_spec.json" in names
    assert "ptv_starter_room.exe" in names
    # Pruned intermediates must be absent.
    assert "game_over_and_restart_runtime.obj" not in names
    assert "x.stamp" not in names
    assert "y.h" not in names
    # And no path should reference a *.dir intermediate directory.
    assert not any(".dir/" in a.path for a in arts)


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


def test_scene_compile_materializes_controller_binding_into_params():
    """A resolved ControllerBinding on a generated mechanic must be written
    into the entity's mechanic_bindings params (control + hand) so the
    running mechanic reads its control at on_attach. Per-entity params win."""
    from ptv_schemas import (
        ControllerBinding,
        ControllerControl,
        ControllerHand,
    )
    from ptv_schemas.code_task import CodeTask, CodeTaskKind

    bs = BuildSpec(
        prompt_id="demo",
        title="Demo",
        pitch="jump game",
        game_type=GameType.arcade,
        scenes=[SceneEntry(id="room", name="Room", summary="one room.", entry_point=True)],
        mechanics=[MechanicEntry(id="jump", name="Jump", summary="press a to jump")],
        assets=[AssetEntry(id="cube", name="Cube", category="prop", summary="wood cube")],
        acceptance_tests=[TestEntry(id="t1", description="jump works")],
    )
    plan = MechanicPlan(
        prompt_id="demo",
        items=[
            MechanicPlanItem(
                id="jump",
                classification=MechanicClassification.generate,
                binding=ControllerBinding(
                    control=ControllerControl.a, hand=ControllerHand.right
                ),
                code_task=CodeTask(
                    id="jump",
                    kind=CodeTaskKind.mechanic,
                    title="Jump",
                    description="x" * 40,
                    interface="IMechanic",
                ),
            )
        ],
    )
    scene = SceneSpec(
        prompt_id="demo",
        scene_id="room",
        name="Room",
        entities=[
            EntitySpec(
                id="player",
                name="Player",
                transform=Transform(position=Vec3(x=0.0, y=0.0, z=0.0)),
                mechanic_bindings=[
                    MechanicBinding(mechanic_id="generated.jump", params={})
                ],
            )
        ],
    )
    doc = compile_scene_json(bs, _manifest(), plan, scene)
    binding = doc["entities"][0]["mechanic_bindings"][0]
    assert binding["id"] == "generated.jump"
    assert binding["params"]["control"] == "a"
    assert binding["params"]["hand"] == "right"


def test_scene_compile_auto_instantiates_unreferenced_assets():
    """Part 3 of apps/adjustments.md: every Tripo-produced asset must end
    up inside the running VR game. If the LLM-composed SceneSpec drops one,
    the deterministic compiler auto-instantiates a category-appropriate
    default entity for it so the .glb still ships."""

    # Manifest: one asset per category. Style notes deliberately cue the
    # role-detection heuristics for the character / prop categories.
    manifest = AssetManifest(
        prompt_id="demo",
        items=[
            AssetItem(
                id="metropolis_city",
                name="Metropolis City",
                category=AssetCategory.environment,
                style_notes="dense skyscraper city as the world environment",
                reference=ReferenceImageRequest(prompt="a sci-fi city skyline"),
                target_scale_meters=24.0,
                collision_shape="mesh",
            ),
            AssetItem(
                id="superman_avatar",
                name="Superman",
                category=AssetCategory.character,
                style_notes="Player avatar: Superman, full body in flight pose",
                reference=ReferenceImageRequest(prompt="Superman, full body, hero pose"),
                target_scale_meters=1.9,
            ),
            AssetItem(
                id="villain_drone",
                name="Hostile Drone",
                category=AssetCategory.character,
                style_notes="Hostile enemy training drone",
                reference=ReferenceImageRequest(prompt="hostile combat drone"),
                target_scale_meters=0.4,
            ),
            AssetItem(
                id="merchant_npc",
                name="Merchant",
                category=AssetCategory.character,
                style_notes="friendly merchant NPC standing behind a counter",
                reference=ReferenceImageRequest(prompt="merchant NPC, idle pose"),
                target_scale_meters=1.8,
            ),
            AssetItem(
                id="power_ring",
                name="Power Ring",
                category=AssetCategory.prop,
                style_notes="grabbable, wearable power ring artifact",
                reference=ReferenceImageRequest(prompt="glowing alien power ring"),
                target_scale_meters=0.08,
            ),
            AssetItem(
                id="lamp_dressing",
                name="Street Lamp",
                category=AssetCategory.prop,
                style_notes="static dressing prop, decorative street lamp",
                reference=ReferenceImageRequest(prompt="ornate street lamp"),
                target_scale_meters=2.4,
            ),
            AssetItem(
                id="blast_beam_vfx",
                name="Blast Beam",
                category=AssetCategory.fx,
                style_notes="energy beam vfx impostor",
                reference=ReferenceImageRequest(prompt="green energy beam vfx"),
                target_scale_meters=1.0,
            ),
            AssetItem(
                id="ring_hud_panel",
                name="Ring HUD",
                category=AssetCategory.ui,
                style_notes="world-anchored hud panel showing ring energy",
                reference=ReferenceImageRequest(prompt="circular HUD panel mockup"),
                target_scale_meters=0.5,
            ),
        ],
    )

    # Empty SceneSpec: the planner forgot to instantiate ANYTHING.
    empty_scene = SceneSpec(
        prompt_id="demo",
        scene_id="room",
        name="Room",
        entities=[],
    )

    # Synthetic asset-normalization report: pretend Tripo succeeded for
    # every asset and dropped a .glb on disk. Without this the safety net
    # refuses to fabricate a mesh entity (correctly).
    normalization = {
        a.id: {"glb_path": f"projects/demo/assets/{a.id}/model_pbr.glb"}
        for a in manifest.items
    }

    doc = compile_scene_json(
        _build_spec(), manifest, _plan(), empty_scene, normalization
    )

    ents = {e["id"]: e for e in doc["entities"]}
    # Every asset became at least one entity.
    for asset in manifest.items:
        auto_id = f"auto_{asset.id}"
        assert auto_id in ents, f"{asset.id} was silently dropped from the scene"

    # Environment is the world the player spawns in: origin + static + mesh col.
    env = ents["auto_metropolis_city"]
    assert env["transform"]["position"] == [0.0, 0.0, 0.0]
    assert env["physics"]["kind"] == "static"
    assert env["physics"]["collision_shape"] == "mesh"
    assert "environment" in env["tags"]
    assert env["mesh"]["glb_path"].endswith("metropolis_city/model_pbr.glb")

    # Player avatar -> tagged player_avatar, no physics body.
    avatar = ents["auto_superman_avatar"]
    assert avatar["physics"] is None
    assert "player_avatar" in avatar["tags"]

    # Enemy character -> dynamic capsule, tagged enemy.
    enemy = ents["auto_villain_drone"]
    assert enemy["physics"]["kind"] == "dynamic"
    assert enemy["physics"]["collision_shape"] == "capsule"
    assert "enemy" in enemy["tags"]

    # Plain character -> NPC default, static capsule.
    npc = ents["auto_merchant_npc"]
    assert npc["physics"]["kind"] == "static"
    assert npc["physics"]["collision_shape"] == "capsule"
    assert "npc" in npc["tags"]

    # Grabbable prop -> dynamic, interactable.
    ring = ents["auto_power_ring"]
    assert ring["physics"]["kind"] == "dynamic"
    assert "interactable" in ring["tags"] and "grabbable" in ring["tags"]

    # Static dressing prop -> static AABB, tagged static.
    lamp = ents["auto_lamp_dressing"]
    assert lamp["physics"]["kind"] == "static"
    assert "static" in lamp["tags"]

    # FX -> no physics, tagged vfx.
    fx = ents["auto_blast_beam_vfx"]
    assert fx["physics"] is None
    assert "vfx" in fx["tags"]

    # UI -> no physics, tagged hud.
    hud = ents["auto_ring_hud_panel"]
    assert hud["physics"] is None
    assert "hud" in hud["tags"]


def test_scene_compile_does_not_duplicate_referenced_assets():
    """If the SceneSpec already places an asset, the safety net must not
    add a second 'auto_*' copy."""

    doc = compile_scene_json(_build_spec(), _manifest(), _plan(), _scene_spec())
    ids = [e["id"] for e in doc["entities"]]
    assert "auto_cube" not in ids
    # Original entity is preserved untouched.
    assert "cube1" in ids


def test_scene_compile_skips_unreferenced_assets_without_glb():
    """If the asset has no .glb on disk (Tripo failed for it), the safety
    net must NOT emit a dangling mesh entity that would crash the loader."""

    manifest = AssetManifest(
        prompt_id="demo",
        items=[
            AssetItem(
                id="missing_city",
                name="Missing City",
                category=AssetCategory.environment,
                style_notes="city environment but Tripo never produced it",
                reference=ReferenceImageRequest(prompt="a city"),
                target_scale_meters=20.0,
            )
        ],
    )
    empty_scene = SceneSpec(
        prompt_id="demo", scene_id="room", name="Room", entities=[]
    )
    doc = compile_scene_json(
        _build_spec(), manifest, _plan(), empty_scene, normalization_report={}
    )
    assert doc["entities"] == []


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


def test_validate_includes_accepts_random_and_cctype():
    """Regression for the alien-shooter codegen run: <random> (enemy fire
    spread/jitter) and <cctype> (HUD label formatting) are legitimate,
    deterministic, side-effect-free standard headers and must NOT be
    flagged as violations."""
    src = (
        '#include "ptv/api/IMechanic.h"\n'
        "#include <random>\n"
        "#include <cctype>\n"
        "#include <sstream>\n"
        "#include <charconv>\n"
        "void foo() {}\n"
    )
    assert validate_includes(src, "enemy_projectile_firing_shared.h") == []


@pytest.mark.parametrize(
    "header",
    ["random", "cctype", "cwctype", "sstream", "iomanip", "charconv",
     "cstdlib", "climits", "limits", "bit", "bitset", "cassert",
     "stdexcept", "deque", "list"],
)
def test_validate_includes_accepts_expanded_stdlib(header):
    src = f"#include <{header}>\nvoid foo() {{}}\n"
    assert validate_includes(src, "x.cpp") == [], header


@pytest.mark.parametrize(
    "header",
    ["iostream", "fstream", "thread", "mutex", "atomic", "filesystem",
     "cstdio", "regex", "ctime"],
)
def test_validate_includes_rejects_forbidden_stdlib(header):
    """Non-deterministic / I/O / threading headers stay rejected (fail-loud)."""
    src = f"#include <{header}>\nvoid foo() {{}}\n"
    violations = validate_includes(src, "x.cpp")
    assert len(violations) == 1
    assert "disallowed include" in violations[0]


def test_forbidden_and_allowed_stdlib_sets_are_disjoint():
    """A header can never be both allowed and documented-forbidden."""
    assert _HEADER_WHITELIST_EXACT.isdisjoint(_FORBIDDEN_STD_HEADERS)


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

        def get_balance(self):
            # Plenty of credits so the preflight passes; each individual
            # generation call then blows up instead.
            return {"balance": 10000.0, "frozen": 0.0}

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

    # The stage's error string now includes "NEW" to distinguish "no
    # work produced this run" from "no GLBs on disk at all" -- both
    # halt the pipeline, but the second can be a legitimate Resume
    # after a clean prior run.
    with pytest.raises(RuntimeError, match=r"produced 0 (NEW )?models"):
        stage_tripo_models.run(ctx)


def test_tripo_stage_preflight_fails_on_insufficient_balance(tmp_path: Path, monkeypatch):
    """Regression for 'user wastes 9 image-to-3D requests on a 0-balance
    account': the preflight balance call must halt the stage BEFORE any
    image_to_model / text_to_model request is fired."""

    from ptv_orchestrator.events import EventBus
    from ptv_orchestrator.pipeline import stage_tripo_models
    from ptv_orchestrator.pipeline.context import PipelineContext
    from ptv_orchestrator.settings import Settings

    calls: list[str] = []

    class _ZeroBalanceTripo:
        def __init__(self, *a, **kw):
            pass

        def get_balance(self):
            return {"balance": 0.0, "frozen": 0.0}

        def image_to_model(self, **kwargs):
            calls.append("image_to_model")
            raise RuntimeError("should never be called; preflight must halt first")

        def text_to_model(self, **kwargs):
            calls.append("text_to_model")
            raise RuntimeError("should never be called; preflight must halt first")

    monkeypatch.setattr(stage_tripo_models, "TripoClient", _ZeroBalanceTripo)

    settings = Settings(repo_root=tmp_path, projects_dir=tmp_path / "projects")
    store = ProjectStore(settings.projects_dir)
    state = store.create("Zero Balance", "a tiny prompt for the test")
    store.save_asset_manifest(state.project_id, _manifest())

    ctx = PipelineContext(
        project_id=state.project_id,
        settings=settings,
        store=store,
        bus=EventBus(),
    )

    with pytest.raises(RuntimeError, match="Insufficient Tripo credits"):
        stage_tripo_models.run(ctx)

    assert calls == [], f"Preflight failed but generation was still attempted: {calls}"


# ---------------------------------------------------------------------------
# Auto-rigging stage
# ---------------------------------------------------------------------------


def _rig_manifest(rigged: bool):
    return AssetManifest(
        prompt_id="demo",
        items=[
            AssetItem(
                id="hero",
                name="Hero",
                category=AssetCategory.character,
                style_notes="a humanoid",
                reference=ReferenceImageRequest(prompt="a humanoid character"),
                rigged=rigged,
            )
        ],
    )


def _rig_ctx(tmp_path: Path, *, rigged: bool):
    from ptv_orchestrator.events import EventBus
    from ptv_orchestrator.pipeline.context import PipelineContext
    from ptv_orchestrator.settings import Settings

    settings = Settings(repo_root=tmp_path, projects_dir=tmp_path / "projects")
    store = ProjectStore(settings.projects_dir)
    state = store.create("Rig Test", "a tiny prompt for the test")
    store.save_asset_manifest(state.project_id, _rig_manifest(rigged))

    item_dir = store.dir_for(state.project_id) / "assets" / "hero"
    item_dir.mkdir(parents=True, exist_ok=True)
    (item_dir / "model.glb").write_bytes(b"glTF\x02\x00\x00\x00")

    ctx = PipelineContext(
        project_id=state.project_id,
        settings=settings,
        store=store,
        bus=EventBus(),
    )
    return ctx, item_dir


class _FakeRigCheck:
    def __init__(self, riggable: bool, rig_type: str = "biped"):
        self.task_id = "rigchk_1"
        self.riggable = riggable
        self.rig_type = rig_type

    @property
    def rig_model(self):
        from ptv_providers import rig_model_for_type

        return rig_model_for_type(self.rig_type)


def test_rigging_stage_skips_assets_that_are_not_marked_rigged(
    tmp_path: Path, monkeypatch
):
    """An un-marked asset must never reach Tripo. Each rig is ~30 credits,
    so rigging every prop in a scene would be a large silent bill."""

    from ptv_orchestrator.pipeline import stage_asset_rigging

    calls: list[str] = []

    class _Tripo:
        def __init__(self, *a, **kw):
            pass

        def get_balance(self):
            calls.append("balance")
            return {"balance": 10000.0, "frozen": 0.0}

        def rig_check(self, **kw):
            calls.append("rig_check")
            raise AssertionError("must not be called")

    monkeypatch.setattr(stage_asset_rigging, "TripoClient", _Tripo)

    ctx, _ = _rig_ctx(tmp_path, rigged=False)
    result = stage_asset_rigging.run(ctx)

    assert result.artifacts == []
    # Not even the free balance probe: there is nothing to price.
    assert calls == []


def test_rigging_stage_does_not_pay_for_a_rig_that_cannot_happen(
    tmp_path: Path, monkeypatch
):
    """rig-check is free and rig is ~30 credits, so a `riggable: false`
    answer must stop the sequence rather than optimistically rigging."""

    from ptv_orchestrator.pipeline import stage_asset_rigging

    calls: list[str] = []

    class _Tripo:
        def __init__(self, *a, **kw):
            pass

        def get_balance(self):
            return {"balance": 10000.0, "frozen": 0.0}

        def rig_check(self, **kw):
            calls.append("rig_check")
            return _FakeRigCheck(riggable=False)

        def rig(self, **kw):
            calls.append("rig")
            raise AssertionError("must not rig a non-riggable asset")

    monkeypatch.setattr(stage_asset_rigging, "TripoClient", _Tripo)

    ctx, item_dir = _rig_ctx(tmp_path, rigged=True)
    stage_asset_rigging.run(ctx)

    assert calls == ["rig_check"]
    assert not (item_dir / "model_rigged.glb").exists()

    report = json.loads(
        (ctx.project_dir / "asset_rig.json").read_text(encoding="utf-8")
    )
    assert report["hero"]["status"] == "not_riggable"


def test_rigging_stage_rigs_and_then_reuses_the_recorded_result(
    tmp_path: Path, monkeypatch
):
    """The rig must land as model_rigged.glb (so the un-rigged source
    survives a re-run) and a second pass must not re-bill."""

    from ptv_orchestrator.pipeline import stage_asset_rigging

    calls: list[str] = []

    class _Tripo:
        def __init__(self, *a, **kw):
            pass

        def get_balance(self):
            return {"balance": 10000.0, "frozen": 0.0}

        def rig_check(self, **kw):
            calls.append("rig_check")
            return _FakeRigCheck(riggable=True, rig_type="biped")

        def rig(self, *, out_dir, **kw):
            calls.append("rig")
            assert kw["spec"] == "tripo"
            assert kw["out_format"] == "glb"
            assert kw["model"] == "v1.0-20240301"
            # The real client always names its download ``model.glb``, which
            # collides with the un-rigged source; reproduce that here so the
            # staging-directory handling stays covered.
            out_path = Path(out_dir)
            out_path.mkdir(parents=True, exist_ok=True)
            out = out_path / "model.glb"
            out.write_bytes(b"glTF-rigged")

            class _R:
                task_id = "rig_1"
                primary_glb = out

            return _R()

    monkeypatch.setattr(stage_asset_rigging, "TripoClient", _Tripo)

    ctx, item_dir = _rig_ctx(tmp_path, rigged=True)
    stage_asset_rigging.run(ctx)

    rigged_glb = item_dir / "model_rigged.glb"
    assert rigged_glb.read_bytes() == b"glTF-rigged"
    # The original Tripo output has to survive, or a re-run would rig an
    # already-rigged mesh.
    assert (item_dir / "model.glb").read_bytes().startswith(b"glTF")
    assert calls == ["rig_check", "rig"]

    # Second pass: recorded result is reused, no further Tripo calls.
    stage_asset_rigging.run(ctx)
    assert calls == ["rig_check", "rig"]


def test_rigged_glb_wins_the_primary_glb_pick(tmp_path: Path):
    """Both pickers must prefer the rigged file. On the *model* key alone
    'model.glb' sorts before 'model_rigged.glb' and the skeleton would
    silently never reach the scene."""

    from ptv_orchestrator.pipeline.stage_asset_normalize import _pick_primary_glb
    from ptv_orchestrator.pipeline.stage_tripo_models import _existing_glb

    d = tmp_path / "asset"
    d.mkdir()
    (d / "model.glb").write_bytes(b"glTF\x02\x00\x00\x00")
    (d / "model_rigged.glb").write_bytes(b"glTF\x02\x00\x00\x00")

    assert _pick_primary_glb(d).name == "model_rigged.glb"
    assert _existing_glb(d).name == "model_rigged.glb"


def test_animated_glb_outranks_the_rigged_one(tmp_path: Path):
    """Each stage writes beside its input so a re-run can start from the
    original, which means the pickers carry the whole precedence chain."""

    from ptv_orchestrator.pipeline.stage_asset_normalize import _pick_primary_glb
    from ptv_orchestrator.pipeline.stage_asset_rigging import _source_glb
    from ptv_orchestrator.pipeline.stage_tripo_models import _existing_glb

    d = tmp_path / "asset"
    d.mkdir()
    for name in ("model.glb", "model_rigged.glb", "model_animated.glb"):
        (d / name).write_bytes(b"glTF\x02\x00\x00\x00")

    assert _pick_primary_glb(d).name == "model_animated.glb"
    assert _existing_glb(d).name == "model_animated.glb"
    # Rigging must still start from the untouched Tripo output, or a re-run
    # would rig a mesh that already has a skeleton and clips.
    assert _source_glb(d).name == "model.glb"


# ---------------------------------------------------------------------------
# Animation stage
# ---------------------------------------------------------------------------


def _animated_glb_bytes(clip_names: list[str]) -> bytes:
    """A minimal GLB whose JSON chunk advertises the given animations."""
    import struct

    doc = {
        "asset": {"version": "2.0"},
        "animations": [{"name": n} for n in clip_names],
    }
    payload = json.dumps(doc).encode("utf-8")
    payload += b" " * ((4 - len(payload) % 4) % 4)
    out = struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(payload))
    out += struct.pack("<II", len(payload), 0x4E4F534A)
    out += payload
    return out


def _anim_ctx(tmp_path: Path, *, intents, rig_entry):
    from ptv_orchestrator.events import EventBus
    from ptv_orchestrator.pipeline.context import PipelineContext
    from ptv_orchestrator.settings import Settings
    from ptv_schemas import AnimationIntent

    settings = Settings(repo_root=tmp_path, projects_dir=tmp_path / "projects")
    store = ProjectStore(settings.projects_dir)
    state = store.create("Anim Test", "a tiny prompt for the test")
    store.save_asset_manifest(
        state.project_id,
        AssetManifest(
            prompt_id="demo",
            items=[
                AssetItem(
                    id="hero",
                    name="Hero",
                    category=AssetCategory.character,
                    style_notes="a humanoid",
                    reference=ReferenceImageRequest(prompt="a humanoid"),
                    rigged=True,
                    animations=[AnimationIntent(i) for i in intents],
                )
            ],
        ),
    )

    project_dir = store.dir_for(state.project_id)
    (project_dir / "assets" / "hero").mkdir(parents=True, exist_ok=True)
    if rig_entry is not None:
        (project_dir / "asset_rig.json").write_text(
            json.dumps({"hero": rig_entry}), encoding="utf-8"
        )

    ctx = PipelineContext(
        project_id=state.project_id,
        settings=settings,
        store=store,
        bus=EventBus(),
    )
    return ctx, project_dir / "assets" / "hero"


def test_animation_stage_maps_intents_and_records_real_clip_names(
    tmp_path: Path, monkeypatch
):
    """Tripo names the clips, so the stage must read the names actually in
    the returned GLB rather than assuming they match the presets."""

    from ptv_orchestrator.pipeline import stage_asset_animation

    sent: dict = {}

    class _Tripo:
        def __init__(self, *a, **kw):
            pass

        def get_balance(self):
            return {"balance": 10000.0, "frozen": 0.0}

        def retarget(self, *, out_dir, rig_task_id, animations, **kw):
            sent["rig_task_id"] = rig_task_id
            sent["animations"] = list(animations)
            sent.update(kw)
            out = Path(out_dir)
            out.mkdir(parents=True, exist_ok=True)
            glb = out / "model.glb"
            # Tripo's own naming, deliberately unlike the preset ids.
            glb.write_bytes(_animated_glb_bytes(["Idle", "Walk", "Run"]))

            class _R:
                task_id = "task_anim"
                primary_glb = glb

            return _R()

    monkeypatch.setattr(stage_asset_animation, "TripoClient", _Tripo)

    ctx, item_dir = _anim_ctx(
        tmp_path,
        intents=["idle", "walk", "run"],
        rig_entry={
            "status": "rigged",
            "rig_task_id": "task_rig",
            "rig_type": "biped",
            "model": "v1.0-20240301",
        },
    )
    stage_asset_animation.run(ctx)

    assert sent["rig_task_id"] == "task_rig"
    assert sent["animations"] == [
        "preset:biped:idle",
        "preset:biped:walk",
        "preset:biped:run",
    ]
    assert sent["animate_in_place"] is True
    assert sent["export_with_geometry"] is True

    assert (item_dir / "model_animated.glb").is_file()
    report = json.loads(
        (ctx.project_dir / "asset_animation.json").read_text(encoding="utf-8")
    )
    entry = report["hero"]
    assert entry["status"] == "animated"
    # The contract generated C++ relies on: semantic intent -> real clip name.
    assert entry["animations"] == {
        "idle": "Idle",
        "walk": "Walk",
        "run": "Run",
    }


def test_animation_stage_skips_an_asset_with_no_recorded_rig_task(
    tmp_path: Path, monkeypatch
):
    """Retarget takes a rig task id and nothing else, so an asset rigged
    before the pipeline recorded one cannot be animated -- and must say so
    rather than burning a call."""

    from ptv_orchestrator.pipeline import stage_asset_animation

    calls: list[str] = []

    class _Tripo:
        def __init__(self, *a, **kw):
            pass

        def get_balance(self):
            calls.append("balance")
            return {"balance": 10000.0, "frozen": 0.0}

        def retarget(self, **kw):
            calls.append("retarget")
            raise AssertionError("must not be called")

    monkeypatch.setattr(stage_asset_animation, "TripoClient", _Tripo)

    ctx, _ = _anim_ctx(
        tmp_path,
        intents=["idle", "walk"],
        rig_entry={"status": "rigged", "rig_type": "biped"},
    )
    stage_asset_animation.run(ctx)

    assert calls == []
    report = json.loads(
        (ctx.project_dir / "asset_animation.json").read_text(encoding="utf-8")
    )
    assert report["hero"]["status"] == "missing_rig_task"


def test_animation_map_reaches_scene_json_through_normalization():
    """The clip contract has to survive two hops: the animation stage records
    it, normalization carries it, and scene_compile emits it. A break anywhere
    leaves generated C++ guessing clip names."""

    from ptv_orchestrator.pipeline.stage_scene_compile import _mesh_render_fields

    norm = {
        "skinned": True,
        "joint_count": 65,
        "animations": {"idle": "Idle", "walk": "Walk_01"},
    }
    out = _mesh_render_fields(norm, 1.6, None)

    assert out["skinned"] is True
    assert out["animations"] == {"idle": "Idle", "walk": "Walk_01"}

    # An un-animated asset must not gain an empty map -- the runtime treats
    # absence as "no contract" and falls back to exact-name matching.
    assert "animations" not in _mesh_render_fields({"skinned": True}, 1.0, None)


def test_animated_agents_get_their_own_budget_per_tier():
    """Skinning and AI are CPU work competing with the render thread, so a
    scene can be well inside its triangle budget and still miss frame rate on
    skeletons alone. Characters therefore need a separate ceiling."""

    from ptv_schemas import PerformanceTier, TargetDevice, derive_profile

    quest = derive_profile(
        asset_count=10, mesh_entity_count=10, target_device=TargetDevice.quest
    )
    assert quest.max_animated_agents > 0
    assert len(quest.animation_lod_distances_m) == 3
    assert len(quest.ai_tick_rates_hz) == 3
    # Bands must increase, or "nearer than the second" is meaningless.
    assert (
        quest.animation_lod_distances_m[0]
        < quest.animation_lod_distances_m[1]
        < quest.animation_lod_distances_m[2]
    )
    # Distant NPCs think less often, never more.
    assert quest.ai_tick_rates_hz[0] > quest.ai_tick_rates_hz[2]

    # A bigger scene has less headroom for characters.
    massive = derive_profile(
        asset_count=200,
        mesh_entity_count=400,
        target_device=TargetDevice.quest,
        tier_override=PerformanceTier.massive,
    )
    assert massive.max_animated_agents < quest.max_animated_agents

    # A PC has CPU headroom the headset does not.
    pcvr = derive_profile(
        asset_count=10, mesh_entity_count=10, target_device=TargetDevice.pcvr
    )
    assert pcvr.max_animated_agents > quest.max_animated_agents
    assert pcvr.animation_lod_distances_m[0] > quest.animation_lod_distances_m[0]


def test_scene_compile_warns_when_too_many_characters_are_placed_at_load():
    from ptv_orchestrator.pipeline.stage_scene_compile import (
        check_animated_agent_budget,
        count_animated_agents,
    )

    def entity(eid: str, skinned: bool, tags=()):
        return {
            "id": eid,
            "tags": list(tags),
            "mesh": {"asset_id": eid, "skinned": skinned},
        }

    entities = [entity(f"e{i}", True) for i in range(5)]
    # Static props do not cost pose evaluation...
    entities += [entity("crate", False)]
    # ...and neither does a hidden template, which is never posed because it
    # is never in the scene as an actor.
    entities += [entity("spawner_src", True, tags=["template"])]

    assert count_animated_agents(entities) == 5

    assert check_animated_agent_budget(entities, {"max_animated_agents": 8}) == ""
    over = check_animated_agent_budget(
        entities, {"max_animated_agents": 3, "tier": "standard"}
    )
    assert "5 animated characters" in over and "allows 3" in over
    # It has to say what to do instead, or the warning is just noise.
    assert "template" in over

    # No performance block (older projects) means no opinion.
    assert check_animated_agent_budget(entities, None) == ""


def test_locomotion_clips_get_a_reference_speed_scaled_by_height():
    """Tripo retargets in place, so a walk clip carries no root motion to
    measure. Without a reference speed the runtime cannot tell whether the
    stride matches the ground covered, and the character skates."""

    from ptv_orchestrator.pipeline.stage_scene_compile import (
        _mesh_render_fields,
    )

    norm = {
        "skinned": True,
        "animations": {
            "idle": "Idle",
            "walk": "Walk_01",
            "run": "Run_Fast",
            "attack": "Slash",
        },
    }
    out = _mesh_render_fields(norm, 1.8, None)
    speeds = out["animation_speeds"]

    # 1.8 m biped: ~1.4 m/s walking, ~4.0 m/s running.
    assert speeds["walk"] == pytest.approx(1.404, abs=1e-3)
    assert speeds["run"] == pytest.approx(3.96, abs=1e-3)

    # Only locomotion gets a speed. Rate-scaling an attack or an idle to
    # "match" a ground speed would be meaningless.
    assert "idle" not in speeds
    assert "attack" not in speeds

    # Half-height creature, half the stride.
    small = _mesh_render_fields(norm, 0.9, None)["animation_speeds"]
    assert small["walk"] == pytest.approx(speeds["walk"] / 2.0, abs=1e-3)

    # No height means no honest estimate, so emit nothing rather than a wrong
    # divisor.
    assert "animation_speeds" not in _mesh_render_fields(norm, 0.0, None)


def test_normalization_picks_up_the_animation_report(tmp_path: Path):
    from ptv_orchestrator.pipeline.stage_asset_normalize import (
        _animation_clip_names,
    )

    (tmp_path / "asset_animation.json").write_text(
        json.dumps(
            {
                "hero": {
                    "status": "animated",
                    "animations": {"walk": "Walk_01"},
                },
                # Failed and skipped assets must not contribute a contract.
                "rock": {"status": "not_rigged"},
                "ogre": {"status": "failed", "error": "boom"},
            }
        ),
        encoding="utf-8",
    )

    got = _animation_clip_names(tmp_path)
    assert got == {"hero": {"walk": "Walk_01"}}
    # Missing file is the normal case for a project with no animated assets.
    assert _animation_clip_names(tmp_path / "nope") == {}


def test_animation_stage_does_not_rebill_on_a_second_pass(
    tmp_path: Path, monkeypatch
):
    from ptv_orchestrator.pipeline import stage_asset_animation

    calls: list[str] = []

    class _Tripo:
        def __init__(self, *a, **kw):
            pass

        def get_balance(self):
            return {"balance": 10000.0, "frozen": 0.0}

        def retarget(self, *, out_dir, **kw):
            calls.append("retarget")
            out = Path(out_dir)
            out.mkdir(parents=True, exist_ok=True)
            glb = out / "model.glb"
            glb.write_bytes(_animated_glb_bytes(["Walk"]))

            class _R:
                task_id = "task_anim"
                primary_glb = glb

            return _R()

    monkeypatch.setattr(stage_asset_animation, "TripoClient", _Tripo)

    ctx, _ = _anim_ctx(
        tmp_path,
        intents=["walk"],
        rig_entry={
            "status": "rigged",
            "rig_task_id": "task_rig",
            "rig_type": "biped",
            "model": "v1.0-20240301",
        },
    )
    stage_asset_animation.run(ctx)
    stage_asset_animation.run(ctx)

    assert calls == ["retarget"]


def test_artifact_id_accepts_vcpkg_leading_underscore_filenames(tmp_path: Path):
    """Regression for ValidationError on ``_features.hpp`` and friends that
    vcpkg drops into the build tree. ``list_artifacts`` must produce a valid
    ``IdStr`` for every file under the project without raising."""

    from ptv_orchestrator.storage import _artifact_id

    assert _artifact_id("_features.hpp") == "features.hpp"
    assert _artifact_id(".ninja_deps") == "ninja_deps"
    assert _artifact_id("---weird").startswith("weird")
    assert _artifact_id("OK_Name.JSON") == "ok_name.json"
    # First char must be [a-z0-9] to satisfy the IdStr regex.
    import re

    pat = re.compile(r"^[a-z0-9][a-z0-9_.\-]{0,63}$")
    for raw in ("_features.hpp", ".gitkeep", "---", "CMakeCache.txt", "build_spec.json"):
        assert pat.match(_artifact_id(raw)), f"bad id for {raw!r}: {_artifact_id(raw)!r}"


def test_repair_rewrites_transform_scale_vec3_to_float():
    """Regression for the Green Lantern construct_quick_select_mechanic
    build break: Claude wrote ``ct.t.scale = Vec3{1.0f, 1.0f, 1.0f};`` and
    MSVC died with C2440. The repair pass MUST rewrite this to a scalar
    float assignment before the file is ever written to disk."""

    src = (
        "void f(World& w, std::uint32_t e) {\n"
        "    auto& tr = w.get<CTransform>(e).t;\n"
        "    ct.t.scale = Vec3{1.0f, 1.0f, 1.0f};\n"
        "    tr.scale = Vec3{2.5f, 1.0f, 1.0f};\n"
        "    tr.scale = Vec3(1.0f + 0.1f * std::sin(t), 1.0f, 1.0f);\n"
        "}\n"
    )
    repaired, notes = repair_generated_cpp(src)

    # Vec3 forms are gone.
    assert "Vec3{" not in repaired
    assert "Vec3(" not in repaired
    # First numeric / expression argument is preserved (so animated scales
    # like ``1.0f + 0.1f * std::sin(t)`` keep their behaviour).
    assert "ct.t.scale = 1.0f;" in repaired
    assert "tr.scale = 2.5f;" in repaired
    assert "tr.scale = 1.0f + 0.1f * std::sin(t);" in repaired
    # Each rewrite leaves a marker comment so reviewers can spot it.
    assert repaired.count("[ptv-repair]") == 3
    # Stage emits exactly one human-readable note when any repair fires.
    assert len(notes) == 1
    assert "Transform::scale" in notes[0]


def test_repair_is_noop_on_clean_code():
    """If the LLM gets it right the first time, the repair pass must not
    mangle the source. No notes, no replacements."""

    clean = (
        "void f(World& w, std::uint32_t e) {\n"
        "    auto& tr = w.get<CTransform>(e).t;\n"
        "    tr.scale = 1.0f;\n"
        "    tr.scale = 1.0f + std::sin(t) * 0.1f;\n"
        "}\n"
    )
    repaired, notes = repair_generated_cpp(clean)
    assert repaired == clean
    assert notes == []


def test_repair_preserves_unrelated_vec3_uses():
    """The repair must NOT touch legitimate Vec3 uses (positions, custom
    structs that happen to have a 'scale' field of type Vec3, etc.). It
    only rewrites assignments where 'scale' is the field being written."""

    src = (
        "Vec3 dir = Vec3{0.0f, 0.0f, -1.0f};\n"
        "tr.position = Vec3{1.0f, 2.0f, 3.0f};\n"
        "Vec3 v = Vec3(0.5f, 0.5f, 0.5f);\n"
    )
    repaired, notes = repair_generated_cpp(src)
    assert repaired == src
    assert notes == []


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
