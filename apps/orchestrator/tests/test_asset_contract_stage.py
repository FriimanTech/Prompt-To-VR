"""Tests for the asset_contract stage (socket derivation).

The ``ptv_glb_sockets`` C++ tool is stubbed out: these cover the stage's own
contract -- which assets it runs on, how it shapes the tool's output, and how
it degrades when geometry cannot supply what gameplay asked for.
"""

from __future__ import annotations

import json
from pathlib import Path

from ptv_orchestrator.events import EventBus
from ptv_orchestrator.pipeline import stage_asset_contract
from ptv_orchestrator.pipeline.context import PipelineContext
from ptv_orchestrator.settings import Settings
from ptv_orchestrator.storage import ProjectStore
from ptv_schemas import AssetItem, AssetManifest, ReferenceImageRequest
from ptv_schemas.asset_manifest import AssetCategory

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


def _manifest(socket_hints: list[str]) -> AssetManifest:
    return AssetManifest(
        prompt_id="demo",
        items=[
            AssetItem(
                id="blaster",
                name="Blaster",
                category=AssetCategory.prop,
                style_notes="alien blaster",
                reference=ReferenceImageRequest(prompt="an alien blaster"),
                target_scale_meters=0.35,
                socket_hints=socket_hints,
            )
        ],
    )


def _ctx(tmp_path: Path, socket_hints: list[str]) -> PipelineContext:
    settings = Settings(repo_root=tmp_path, projects_dir=tmp_path / "projects")
    store = ProjectStore(settings.projects_dir)
    state = store.create("Sockets", "a prompt long enough to pass validation")
    store.save_asset_manifest(state.project_id, _manifest(socket_hints))

    ctx = PipelineContext(
        project_id=state.project_id,
        settings=settings,
        store=store,
        bus=EventBus(),
    )
    glb = ctx.project_dir / "assets" / "blaster" / "model.glb"
    glb.parent.mkdir(parents=True, exist_ok=True)
    glb.write_bytes(b"glTF\x02\x00\x00\x00")
    (ctx.project_dir / "asset_normalization.json").write_text(
        json.dumps(
            {
                "blaster": {
                    "status": "ok",
                    "glb_path": str(glb).replace("\\", "/"),
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


def test_tool_failure_degrades_to_empty_sockets(tmp_path: Path, monkeypatch):
    ctx = _ctx(tmp_path, ["muzzle"])
    _stub_tool(monkeypatch, payload=None, rc=1)

    stage_asset_contract.run(ctx)

    entry = json.loads(
        (ctx.project_dir / "asset_contract.json").read_text(encoding="utf-8")
    )["blaster"]
    assert entry["status"] == "failed"
    assert entry["sockets"] == {}


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
