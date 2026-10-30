"""Tests for the asset_contract stage (socket derivation).

The ``ptv_glb_sockets`` C++ tool is stubbed out: these cover the stage's own
contract -- which assets it runs on, how it shapes the tool's output, and how
it degrades when geometry cannot supply what gameplay asked for.
"""

from __future__ import annotations

import ast
import json
from pathlib import Path

import pytest
from ptv_orchestrator.events import EventBus
from ptv_orchestrator.pipeline import stage_asset_contract
from ptv_orchestrator.pipeline.context import PipelineContext
from ptv_orchestrator.settings import Settings
from ptv_orchestrator.storage import ProjectStore
from ptv_schemas import AssetItem, AssetManifest, ReferenceImageRequest
from ptv_schemas.asset_manifest import AssetCategory
from ptv_schemas.project_state import ProjectStatus

_TOOL_OUTPUT = {
    "aabb_min": [-0.1, -0.05, -0.3],
    "aabb_max": [0.1, 0.05, 0.3],
    "longest_axis": 0.6,
    "principal_axis": [0.0, 0.0, 1.0],
    "sockets": {
        "muzzle": {"position": [0.0, 0.02, 0.5], "rotation": [0, 0, 0, 1]},
        "grip": {"position": [0.0, -0.1, -0.2], "rotation": [0, 0, 0, 1]},
    },
    "socket_confidence": {"muzzle": "heuristic", "grip": "heuristic"},
    "source_vertices": 1234,
    "source_triangles": 2000,
}


def _manifest(
    socket_hints: list[str], category: AssetCategory = AssetCategory.prop
) -> AssetManifest:
    return AssetManifest(
        prompt_id="demo",
        items=[
            AssetItem(
                id="blaster",
                name="Blaster",
                category=category,
                style_notes="alien blaster",
                reference=ReferenceImageRequest(prompt="an alien blaster"),
                target_scale_meters=0.35,
                socket_hints=socket_hints,
            )
        ],
    )


def _ctx(
    tmp_path: Path,
    socket_hints: list[str],
    *,
    category: AssetCategory = AssetCategory.prop,
    relative_paths: bool = False,
    write_glb: bool = True,
) -> PipelineContext:
    settings = Settings(repo_root=tmp_path, projects_dir=tmp_path / "projects")
    store = ProjectStore(settings.projects_dir)
    state = store.create("Sockets", "a prompt long enough to pass validation")
    store.save_asset_manifest(
        state.project_id, _manifest(socket_hints, category)
    )

    ctx = PipelineContext(
        project_id=state.project_id,
        settings=settings,
        store=store,
        bus=EventBus(),
    )
    glb = ctx.project_dir / "assets" / "blaster" / "model.glb"
    if write_glb:
        glb.parent.mkdir(parents=True, exist_ok=True)
        glb.write_bytes(b"glTF\x02\x00\x00\x00")
    recorded = glb.relative_to(tmp_path) if relative_paths else glb
    (ctx.project_dir / "asset_normalization.json").write_text(
        json.dumps(
            {
                "blaster": {
                    "status": "ok",
                    "glb_path": str(recorded).replace("\\", "/"),
                    "target_scale_meters": 0.35,
                }
            }
        ),
        encoding="utf-8",
    )
    return ctx


def _stub_tool(monkeypatch, payload: dict | None = _TOOL_OUTPUT, rc: int = 0):
    monkeypatch.setattr(
        stage_asset_contract, "_ensure_tool", lambda ctx: Path("ptv_glb_sockets")
    )

    class _Proc:
        returncode = rc
        stdout = ""
        stderr = "boom" if rc else ""

    def fake_run(args, **kwargs):
        if payload is not None:
            out = Path(args[args.index("--output") + 1])
            out.write_text(json.dumps(payload), encoding="utf-8")
        return _Proc()

    monkeypatch.setattr(stage_asset_contract.subprocess, "run", fake_run)


def test_derives_hinted_sockets_in_unit_space(tmp_path: Path, monkeypatch):
    ctx = _ctx(tmp_path, ["muzzle", "grip"])
    _stub_tool(monkeypatch)

    stage_asset_contract.run(ctx)

    report = json.loads(
        (ctx.project_dir / "asset_contract.json").read_text(encoding="utf-8")
    )
    entry = report["blaster"]
    assert entry["status"] == "ok"
    # Unit space, NOT metres: the scene compiler multiplies by
    # target_scale_meters, mirroring how bounds_radius_m already works.
    assert entry["space"] == "unit"
    assert entry["sockets"]["muzzle"]["position"] == [0.0, 0.02, 0.5]
    assert entry["socket_confidence"]["muzzle"] == "heuristic"
    assert entry["unsatisfied_hints"] == []


def test_only_hinted_sockets_are_published(tmp_path: Path, monkeypatch):
    """Asking for just a muzzle must not smuggle a grip into the contract --
    the step-5 validator reads this file to decide what a mechanic may use."""
    ctx = _ctx(tmp_path, ["muzzle"])
    _stub_tool(monkeypatch)

    stage_asset_contract.run(ctx)

    entry = json.loads(
        (ctx.project_dir / "asset_contract.json").read_text(encoding="utf-8")
    )["blaster"]
    assert set(entry["sockets"]) == {"muzzle"}


def test_unsatisfiable_hint_is_reported_not_dropped(tmp_path: Path, monkeypatch):
    """Geometry cannot derive a 'scope_mount'. That gap must be visible in the
    artifact rather than surfacing later as a projectile from nowhere."""
    ctx = _ctx(tmp_path, ["muzzle", "scope_mount"])
    _stub_tool(monkeypatch)

    stage_asset_contract.run(ctx)

    entry = json.loads(
        (ctx.project_dir / "asset_contract.json").read_text(encoding="utf-8")
    )["blaster"]
    assert set(entry["sockets"]) == {"muzzle"}
    assert entry["unsatisfied_hints"] == ["scope_mount"]


def test_asset_without_valid_glb_still_gets_an_entry(tmp_path: Path, monkeypatch):
    ctx = _ctx(tmp_path, ["muzzle"])
    (ctx.project_dir / "asset_normalization.json").write_text(
        json.dumps({"blaster": {"status": "invalid", "reason": "bad magic"}}),
        encoding="utf-8",
    )
    _stub_tool(monkeypatch)

    stage_asset_contract.run(ctx)

    entry = json.loads(
        (ctx.project_dir / "asset_contract.json").read_text(encoding="utf-8")
    )["blaster"]
    assert entry["status"] == "skipped"
    assert entry["sockets"] == {}


# ---------------------------------------------------------------------------
# Path resolution. asset_normalization.json records paths relative to the REPO
# ROOT; resolving them against the process working directory instead made
# every GLB "go missing" for a whole pipeline run while the stage reported
# success, and the damage only surfaced at scene_compile.
# ---------------------------------------------------------------------------


def test_repo_relative_glb_resolves_against_repo_root_not_cwd(
    tmp_path: Path, monkeypatch
):
    ctx = _ctx(tmp_path, ["muzzle", "grip"], relative_paths=True)
    _stub_tool(monkeypatch)

    elsewhere = tmp_path / "somewhere" / "else"
    elsewhere.mkdir(parents=True)
    monkeypatch.chdir(elsewhere)

    stage_asset_contract.run(ctx)

    entry = json.loads(
        (ctx.project_dir / "asset_contract.json").read_text(encoding="utf-8")
    )["blaster"]
    assert entry["status"] == "ok"
    assert set(entry["sockets"]) == {"muzzle", "grip"}
    assert entry["source_glb"].startswith(str(tmp_path).replace("\\", "/"))


def test_missing_hinted_glb_fails_the_stage(tmp_path: Path, monkeypatch):
    """Reporting "0 with sockets, 1 skipped" as success is what hid the bug."""
    ctx = _ctx(tmp_path, ["muzzle"], write_glb=False)
    _stub_tool(monkeypatch)

    with pytest.raises(stage_asset_contract.AssetContractError) as excinfo:
        stage_asset_contract.run(ctx)

    assert "not on disk" in str(excinfo.value)
    entry = json.loads(
        (ctx.project_dir / "asset_contract.json").read_text(encoding="utf-8")
    )["blaster"]
    assert entry["status"] == "missing"

    state = ctx.store.load_state(ctx.project_id)
    latest = [r for r in state.stages if r.stage == stage_asset_contract._STAGE][-1]
    assert latest.status == ProjectStatus.failed


def test_tool_failure_on_hinted_asset_fails_the_stage(tmp_path: Path, monkeypatch):
    ctx = _ctx(tmp_path, ["muzzle"])
    _stub_tool(monkeypatch, payload=None, rc=1)

    with pytest.raises(stage_asset_contract.AssetContractError):
        stage_asset_contract.run(ctx)


def test_tool_failure_without_hints_is_not_blocking(tmp_path: Path, monkeypatch):
    """No hints means nothing in the game asked for a socket here."""
    ctx = _ctx(tmp_path, [])
    _stub_tool(monkeypatch, payload=None, rc=1)

    stage_asset_contract.run(ctx)  # does not raise

    entry = json.loads(
        (ctx.project_dir / "asset_contract.json").read_text(encoding="utf-8")
    )["blaster"]
    assert entry["status"] == "failed"


# ---------------------------------------------------------------------------
# Socket policy. Geometry can only ever derive "muzzle" and "grip", and those
# two names only mean something on a held or mounted object. Publishing them
# on a character or a HUD panel is a number nothing reads.
# ---------------------------------------------------------------------------


@pytest.mark.parametrize(
    "category",
    [AssetCategory.character, AssetCategory.ui, AssetCategory.fx],
)
def test_non_prop_without_hints_gets_no_sockets_and_says_why(
    tmp_path: Path, monkeypatch, category: AssetCategory
):
    ctx = _ctx(tmp_path, [], category=category)
    _stub_tool(monkeypatch)

    stage_asset_contract.run(ctx)

    entry = json.loads(
        (ctx.project_dir / "asset_contract.json").read_text(encoding="utf-8")
    )["blaster"]
    assert entry["sockets"] == {}
    assert entry["status"] == "skipped"
    # An empty socket map must explain itself; a bare {} reads exactly like
    # the path bug did.
    assert "no sockets derived" in entry["reason"]


def test_prop_without_hints_keeps_the_implicit_fallback(
    tmp_path: Path, monkeypatch
):
    ctx = _ctx(tmp_path, [])
    _stub_tool(monkeypatch)

    stage_asset_contract.run(ctx)

    entry = json.loads(
        (ctx.project_dir / "asset_contract.json").read_text(encoding="utf-8")
    )["blaster"]
    assert set(entry["sockets"]) == {"muzzle", "grip"}


@pytest.mark.parametrize(
    "category",
    [AssetCategory.character, AssetCategory.ui, AssetCategory.fx],
)
def test_explicit_hints_win_for_any_category(
    tmp_path: Path, monkeypatch, category: AssetCategory
):
    """The manifest asking for a socket is gameplay stating a requirement; the
    category is only a default for what to do when it stays silent."""
    ctx = _ctx(tmp_path, ["muzzle"], category=category)
    _stub_tool(monkeypatch)

    stage_asset_contract.run(ctx)

    entry = json.loads(
        (ctx.project_dir / "asset_contract.json").read_text(encoding="utf-8")
    )["blaster"]
    assert entry["status"] == "ok"
    assert set(entry["sockets"]) == {"muzzle"}


def test_socket_generation_results_are_merged_with_the_hold(tmp_path: Path, monkeypatch):
    ctx = _ctx(tmp_path, ["grip", "muzzle"])
    _stub_tool(monkeypatch)
    generated = {"blaster": {
        "asset_id": "blaster", "status": "ok", "longest": 0.3, "method": "vision",
        "sockets": {
            "grip": {"position": [0.0, -0.2, -0.1], "rotation": [0, 0, 0, 1],
                     "kind": "hand_grip", "confidence": "vision_verified",
                     "handle_thickness_unit": 0.12, "mirror": "same"},
            "muzzle": {"position": [0.0, 0.0, 0.5], "rotation": [0, 0, 0, 1],
                       "kind": "muzzle", "confidence": "vision", "bore_diameter_unit": 0.05},
        },
        "hold": {"kind": "one_hand", "aim_aligned": True, "two_hand": "none",
                 "primary_socket": "grip", "support_socket": "", "forearm_socket": "",
                 "finger_pose": "trigger", "forward": [0, 0, 1], "up": [0, 1, 0]},
        "part_regions": {"grip": "x.json"}, "warnings": [], "overlays": [],
    }}
    (ctx.project_dir / "socket_report.json").write_text(json.dumps(generated),
                                                        encoding="utf-8")
    stage_asset_contract.run(ctx)
    entry = json.loads((ctx.project_dir / "asset_contract.json").read_text("utf-8"))["blaster"]
    assert entry["sockets"]["grip"]["kind"] == "hand_grip"
    assert entry["socket_confidence"]["grip"] == "vision_verified"
    assert entry["hold"]["aim_aligned"] is True
    assert entry["part_regions"] == {"grip": "x.json"}


def test_with_an_interaction_design_unlisted_props_get_no_guessed_sockets(
        tmp_path: Path, monkeypatch):
    from ptv_schemas import InteractionDesign

    ctx = _ctx(tmp_path, [])
    _stub_tool(monkeypatch)
    ctx.store.save_interaction_design(ctx.project_id, InteractionDesign(prompt_id="demo"))
    stage_asset_contract.run(ctx)
    entry = json.loads((ctx.project_dir / "asset_contract.json").read_text("utf-8"))["blaster"]
    assert entry["sockets"] == {} and "interaction_design" in entry["reason"]


def test_no_pipeline_stage_resolves_paths_against_cwd():
    """Artifact paths are repo-root-relative, always.

    A grep guard rather than a behavioural test because the failure mode is
    invisible: the stage still runs, still writes a file, still says
    "succeeded". Parsed with ``ast`` so prose about the old bug in a docstring
    does not trip it.
    """
    pipeline_dir = Path(stage_asset_contract.__file__).parent
    offenders: list[str] = []
    for path in sorted(pipeline_dir.rglob("*.py")):
        tree = ast.parse(path.read_text(encoding="utf-8"))
        for node in ast.walk(tree):
            if not isinstance(node, ast.Call):
                continue
            func = node.func
            if isinstance(func, ast.Attribute) and func.attr in (
                "cwd",
                "getcwd",
            ):
                offenders.append(f"{path.name}:{node.lineno} {func.attr}()")
    assert offenders == [], (
        "pipeline stages must resolve artifact paths against"
        f" settings.repo_root, not the process cwd: {offenders}"
    )
