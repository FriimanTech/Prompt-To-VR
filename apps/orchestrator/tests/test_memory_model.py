"""Quest 3 memory: the build-time model, corrections, calibration, and the
on-device memory checks and status semantics."""

from __future__ import annotations

import json
from pathlib import Path

from ptv_schemas import CheckOutcome, MemoryStatus

from ptv_orchestrator.pipeline import memory_model as mm
from ptv_orchestrator.pipeline.stage_quest_verify import evaluate_memory

MB = 1024 * 1024


def _inp(tex_mb=10, mesh_mb=(4, 2, 1, 0.5), n_meshes=5, importance="standard"):
    return mm.MemoryInputs(
        textures=[mm.TextureCost(key=f"t{i}", bytes=int(tex_mb * MB), size=2048,
                                 importance=importance, closest_view_m=20.0)
                  for i in range(n_meshes)],
        meshes=[mm.MeshCost(key=f"m{i}", lod_bytes=[int(b * MB) for b in mesh_mb],
                            importance=importance) for i in range(n_meshes)],
        entities=100, bodies=50, largest_glb_bytes=20 * MB)


def test_small_game_fits_and_lists_every_category():
    e = mm.estimate(_inp())
    assert e["outcome"] == "fits"
    assert e["steady_mb"] < 1000 and e["peak_mb"] > e["steady_mb"]
    cats = e["categories"]
    for k in ("engine_baseline", "textures", "render_targets", "per_frame_buffers",
              "simulation", "heap_fragmentation", "audio"):
        assert k in cats and "kind" in cats[k]
    assert cats["textures"]["kind"] == "exact"


def test_big_world_uses_residency():
    inp = _inp(tex_mb=5, mesh_mb=(400, 5, 2, 1), n_meshes=12)
    e = mm.estimate(inp)
    assert e["outcome"] == "residency"
    assert e["steady_mb"] == e["target_mb"]


def test_over_target_runs_corrections_background_first():
    inp = _inp(tex_mb=240, mesh_mb=(40, 20, 10, 5), n_meshes=20, importance="background")
    inp.textures += [mm.TextureCost(key="held", bytes=int(5 * MB), size=2048,
                                    importance="held", closest_view_m=0.4)]
    assert mm.estimate(inp)["outcome"] == "over"
    e, notes = mm.plan_corrections(inp, baseline_mb=260, target_mb=4608, ceiling_mb=5120)
    assert notes and "cannot get close" in notes[0]
    held = next(t for t in inp.textures if t.key == "held")
    assert held.size == 2048  # near / held assets are the last resort
    assert len(inp.meshes) == 20  # never removes content


def test_calibration_widens_estimates(tmp_path: Path):
    path = tmp_path / "memory_calibration.json"
    assert mm.load_calibration(path) == 1.0
    mult = mm.update_calibration(path, "p1", estimated_mb=200.0, measured_mb=260.0)
    assert abs(mult - 1.3) < 1e-6 and mm.load_calibration(path) == mult
    # an optimistic estimate never shrinks the margin below 1
    assert mm.update_calibration(path, "p2", 300.0, 150.0) == mult
    low = mm.estimate(_inp())["steady_mb"]
    high = mm.estimate(_inp(), calibration=mult)["steady_mb"]
    assert high > low


def test_status_without_device_is_never_verified():
    assert mm.memory_status_without_device({"outcome": "fits"}) == \
        "estimated_safe_not_device_tested"
    assert mm.memory_status_without_device({"outcome": "over"}) == "at_risk"


SCENE = {"performance": {"memory_target_mb": 4608, "memory_ceiling_mb": 5120,
                         "memory_limit_mb": 5888,
                         "memory_estimate": {"steady_mb": 300.0, "peak_mb": 350.0}}}


def _flow(pss_values):
    lines = []
    for p in pss_values:
        lines += ["flow: state=playing why=start", f"mem: pss {p} MB (target 4608"]
    return "\n".join(lines)


def test_memory_verified_safe_from_measurements():
    samples = [{"total_pss_kb": 200 * 1024}, {"total_pss_kb": 210 * 1024}]
    checks, rep = evaluate_memory(samples=samples, gpu={"total_mb": 40.0},
                                  base_text="mem: pss 205 MB", flow_text=_flow([200, 205, 210]),
                                  soak=[(t, 210.0 + t / 600.0) for t in range(60, 600, 30)],
                                  pressure_text="mem: level red (projected\nmem: level red -> yellow",
                                  pressure_alive=True, kills=[], scene=SCENE)
    assert rep.status == MemoryStatus.verified_safe, [c.detail for c in checks]
    # The soak's last sample (210.9 MB) is the peak.
    assert rep.measured_peak_mb == 210.9 and rep.leak_mb_per_cycle == 5.0
    assert all(c.outcome != CheckOutcome.failed for c in checks)


def test_memory_failures_ceiling_leak_and_kill():
    samples = [{"total_pss_kb": 5300 * 1024}]
    checks, rep = evaluate_memory(samples=samples, gpu={}, base_text="",
                                  flow_text=_flow([200, 400, 600]), soak=[],
                                  pressure_text="", pressure_alive=None,
                                  kills=["lowmemorykiller: Kill 'com.x'"], scene=SCENE)
    assert rep.status == MemoryStatus.failed_on_device
    failed = {c.name for c in checks if c.outcome == CheckOutcome.failed}
    assert "peak PSS under the 5.0 GiB ceiling" in failed
    assert "no leak across restarts" in failed and "no low-memory kill" in failed


def test_no_measurement_is_not_safe():
    checks, rep = evaluate_memory(samples=[], gpu={}, base_text="", flow_text="", soak=[],
                                  pressure_text="", pressure_alive=None, kills=[], scene=SCENE)
    assert rep.status == MemoryStatus.failed_on_device and checks[0].outcome == CheckOutcome.failed


def test_clip_bytes_of_glb(tmp_path: Path):
    import struct

    doc = {"asset": {"version": "2.0"},
           "accessors": [{"count": 10, "componentType": 5126, "type": "SCALAR"},
                         {"count": 10, "componentType": 5126, "type": "VEC4"}],
           "animations": [{"samplers": [{"input": 0, "output": 1}], "channels": []}]}
    js = json.dumps(doc).encode()
    js += b" " * ((4 - len(js) % 4) % 4)
    glb = tmp_path / "a.glb"
    glb.write_bytes(struct.pack("<III", 0x46546C67, 2, 20 + len(js))
                    + struct.pack("<II", len(js), 0x4E4F534A) + js)
    assert mm.clip_bytes_of(glb) == 10 * 4 + 10 * 16
