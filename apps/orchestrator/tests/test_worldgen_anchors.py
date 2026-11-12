"""The world host end to end: build ptv_worldgen with the hand-authored
samples, run ``sample.anchor_range`` and check what it emits -- objects
placed by their base, gameplay anchors and anchor sets, requirement claims,
tagged spawn zones -- and that the anchor validator accepts it (and catches a
broken anchor).

Builds a real CMake tree, so it is skipped without CMake / vcpkg."""

from __future__ import annotations

import json
import shutil
import subprocess
from pathlib import Path

import pytest

from ptv_orchestrator.pipeline import placement
from ptv_orchestrator.settings import get_settings


@pytest.fixture(scope="module")
def worldgen_exe() -> Path:
    settings = get_settings()
    if shutil.which("cmake") is None or not settings.vcpkg_root:
        pytest.skip("cmake / vcpkg not available")
    from ptv_orchestrator.build import configure_and_build

    build_dir = settings.engine_dir / "build" / "worldgen-samples"
    result = configure_and_build(
        source_dir=settings.engine_dir / "worldgen", build_dir=build_dir,
        generator=settings.cmake_generator, platform=settings.cmake_generator_platform,
        config="Release", vcpkg_root=settings.vcpkg_root,
        extra_cache=(("PTV_WORLDGEN_SAMPLES", "ON"),), on_line=lambda _l: None)
    if result.exit_code != 0:
        pytest.fail("ptv_worldgen did not build with the samples")
    exe = next(iter(sorted(build_dir.rglob("ptv_worldgen.exe")) or sorted(
        build_dir.rglob("ptv_worldgen"))), None)
    assert exe is not None
    return exe


def _run(exe: Path, tmp_path: Path) -> dict:
    assets = tmp_path / "assets.json"
    assets.write_text(json.dumps({"crate": {
        "glb_path": "crate.glb", "target_scale_meters": 1.0,
        "aabb_min": [-0.4, -0.3, -0.4], "aabb_max": [0.4, 0.3, 0.4]}}), encoding="utf-8")
    out = tmp_path / "world.scene.json"
    proc = subprocess.run([str(exe), "--builder", "sample.anchor_range", "--assets",
                           str(assets), "--out", str(out)], capture_output=True, text=True,
                          timeout=120)
    assert proc.returncode == 0, proc.stderr
    return json.loads(out.read_text(encoding="utf-8"))


def test_sample_world_declares_the_placement_contract(worldgen_exe, tmp_path):
    doc = _run(worldgen_exe, tmp_path)
    ents = {e["id"]: e for e in doc["entities"]}
    crate = ents["supply_crate"]
    # Placed by its base: the 0.6 m crate's origin sits 0.3 m up.
    assert abs(crate["transform"]["position"][1] - 0.3) < 1e-4
    assert crate["align"] == "base"
    assert crate["mesh"]["aabb_max"] == pytest.approx([0.4, 0.3, 0.4])
    anchors = {a["id"]: a for a in doc["anchors"]}
    assert anchors["glock_on_table"]["relation"] == "on_surface"
    assert abs(anchors["lamp_on_crate"]["position"][1] - 0.6) < 1e-4   # asset_size().size.y
    sets = {s["id"]: s for s in doc["anchor_sets"]}
    assert sets["lane2_hang_points"]["relation"] == "hang_from"
    assert len(sets["lane2_hang_points"]["points"]) == 4
    assert {"id": "req_tables"}.items() <= next(
        r for r in doc["requirements"] if r["id"] == "req_tables").items()
    tags = {t for z in doc["npc_spawn_zones"] for t in z.get("tags") or []}
    assert {"front_band", "back_band"} <= tags


def test_anchor_validator_accepts_it_and_catches_a_floating_anchor(worldgen_exe, tmp_path):
    doc = _run(worldgen_exe, tmp_path)
    reqs = [{"entity": "glock_pistol", "asset_id": "glock", "relation": "on_surface",
             "anchor": "glock_on_table", "size_m": [0.2, 0.14, 0.04]},
            {"entity": "lane2_target", "asset_id": "target", "relation": "hang_from",
             "anchor_set": "lane2_hang_points", "count": 4}]
    assert placement.validate_anchors(doc, reqs) == []
    for a in doc["anchors"]:
        if a["id"] == "glock_on_table":
            a["position"][1] += 0.4   # 40 cm above the table top
    problems = placement.validate_anchors(doc, reqs)
    assert any("glock_on_table" in p for p in problems)
