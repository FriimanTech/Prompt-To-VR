"""Model-frame measurement: soles, height and forward from the skeleton."""

from __future__ import annotations

import math
from pathlib import Path

import pytest

from ptv_orchestrator.pipeline import model_frame

REPO = Path(__file__).resolve().parents[3]
UTHANA_FIXTURE = REPO / "engine" / "runtime" / "tests" / "fixtures" / "uthana_walk_merged.glb"


def test_yaw_offset_turns_every_axis_onto_minus_z() -> None:
    # Same convention as ptv::yaw_from_forward: R_y(t) * f must equal -Z.
    for fx, fz in [(1, 0), (-1, 0), (0, 1), (0, -1), (0.7071, 0.7071)]:
        t = model_frame.yaw_offset_for_forward(fx, fz)
        rx = math.cos(t) * fx + math.sin(t) * fz
        rz = -math.sin(t) * fx + math.cos(t) * fz
        assert rx == pytest.approx(0.0, abs=1e-4)
        assert rz == pytest.approx(-1.0, abs=1e-4)


@pytest.mark.skipif(not UTHANA_FIXTURE.exists(), reason="engine fixture missing")
def test_uthana_character_is_hips_centred_and_faces_plus_z() -> None:
    frame = model_frame.measure(UTHANA_FIXTURE, rigged=True)
    assert frame["status"] == "measured"
    assert frame["forward"] == [0.0, 0.0, 1.0]
    assert frame["yaw_offset_rad"] == pytest.approx(math.pi, abs=1e-4)
    # Uthana centres the hips: the soles are half the height below the origin.
    assert frame["sole_y"] == pytest.approx(-0.5 * frame["height"], abs=0.02)
    assert frame["biped"] is True


def test_unrigged_character_uses_its_hint_and_warns_without_one(tmp_path: Path) -> None:
    from ptv_orchestrator.pipeline.uthana.gltf_io import Glb

    # A 1 x 2 x 1 box with no skin.
    glb = Glb(doc={"asset": {"version": "2.0"}, "buffers": [{"byteLength": 0}]},
              bin=bytearray())
    pts = [(x, y, z) for x in (-0.5, 0.5) for y in (0.0, 2.0) for z in (-0.5, 0.5)]
    acc = glb.append_float_accessor(pts, "VEC3")
    glb.doc["meshes"] = [{"primitives": [{"attributes": {"POSITION": acc}}]}]
    glb.doc["nodes"] = [{"mesh": 0}]
    glb.doc["scenes"] = [{"nodes": [0]}]
    path = tmp_path / "box.glb"
    glb.save(path)

    hinted = model_frame.measure(path, rigged=False, forward_hint="+x")
    assert hinted["method"] == "hint"
    assert hinted["forward"] == [1.0, 0.0, 0.0]
    assert hinted["sole_y"] == pytest.approx(0.0)
    assert hinted["height"] == pytest.approx(2.0)

    guessed = model_frame.measure(path, rigged=False)
    assert guessed["status"] == "default"
    assert guessed["warnings"]


def test_try_measure_reports_unreadable_files(tmp_path: Path) -> None:
    bad = tmp_path / "bad.glb"
    bad.write_bytes(b"nope")
    out = model_frame.try_measure(bad, rigged=True)
    assert out["status"] == "failed"
