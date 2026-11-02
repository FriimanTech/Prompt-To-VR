"""Quest optimization: user preferences -> locks, scene complexity, and the
on-device lock checks."""

from __future__ import annotations

import json
from types import SimpleNamespace

from ptv_schemas import (
    CheckOutcome,
    OptimizationPreferences,
    PerformanceTier,
    TargetDevice,
    apply_overrides,
    derive_profile,
)

from ptv_orchestrator.pipeline import scene_complexity as sc
from ptv_orchestrator.pipeline import stage_optimization_preferences as op
from ptv_orchestrator.pipeline.stage_quest_verify import evaluate_locks


class FakeClient:
    def __init__(self, data):
        self.data = data
        self.calls = 0

    def generate_json(self, **_kw):
        self.calls += 1
        return SimpleNamespace(data=self.data)


def setup_function(_fn):
    op._CACHE.clear()


# --- optimization_preferences ----------------------------------------------

def test_no_optimization_wording_makes_no_model_call():
    client = FakeClient({"items": []})
    prefs = op.parse_preferences("A goblin arena with three waves of goblins.", "", client)
    assert client.calls == 0 and not prefs.model_called and not prefs.settings()


def test_max_quality_example_maps_to_locks():
    box = "no FFR, no resolution reduction, high textures, detailed models far away"
    client = FakeClient({"items": [
        {"setting": "ffr", "value": "off", "source": "custom_box", "quote": "no FFR"},
        {"setting": "resolution", "value": "off", "source": "custom_box",
         "quote": "no resolution reduction"},
        {"setting": "texture_quality", "value": "high", "source": "custom_box",
         "quote": "high textures"},
        {"setting": "lod", "value": "detail", "source": "custom_box",
         "quote": "detailed models far away"},
    ]})
    prefs = op.parse_preferences("Navy SEAL raid on a compound.", box, client)
    assert client.calls == 1
    assert prefs.ffr.value == "off" and prefs.resolution.value == "off"
    assert prefs.texture_quality.value == "high" and prefs.lod.value == "detail"
    locks = prefs.to_locks()
    base = derive_profile(target_device=TargetDevice.quest, asset_count=10,
                          mesh_entity_count=30, tier_override=PerformanceTier.standard)
    p, notes = apply_overrides(base, locks)
    assert p.foveation_level == 0 and p.governor.max_foveation_level == 0 and not p.ffr_fdm
    assert p.render_scale_floor == 1.0 and p.render_scale_ceiling == 1.0
    assert p.texture_astc_block == "4x4" and p.texture_max_size["standard"] == 2048
    assert p.governor.max_lod_bias == 0.0 and p.lod_screen_error_px < base.lod_screen_error_px
    assert len(notes) == 4
    # Everything the user did not mention stays automatic.
    assert p.multiview and p.instancing and p.static_batching
    # Locks reach the scene's performance block.
    dumped = json.loads(p.model_dump_json())
    assert dumped["locks"]["ffr"]["value"] == "off"
    assert dumped["locks"]["ffr"]["quote"] == "no FFR"


def test_quote_must_be_verbatim_and_box_wins():
    client = FakeClient({"items": [
        {"setting": "ffr", "value": "max", "level": "low", "source": "main_prompt",
         "quote": "keep foveation low"},
        {"setting": "ffr", "value": "off", "source": "custom_box", "quote": "FFR off"},
        {"setting": "texture_quality", "value": "high", "source": "custom_box",
         "quote": "ultra textures please"},  # not in the box: rejected
    ]})
    prefs = op.parse_preferences("Arena. keep foveation low", "FFR off", client)
    assert prefs.ffr.value == "off" and prefs.ffr.source == "custom_box"
    assert prefs.texture_quality is None


def test_unsupported_requests_and_clamps():
    client = FakeClient({"items": [
        {"setting": "resolution", "value": "high", "source": "custom_box",
         "quote": "render at 2x resolution"},
        {"setting": "frame_rate", "value": "120", "source": "custom_box", "quote": "144 Hz"},
    ]})
    box = "render at 2x resolution, 144 Hz, eye-tracked foveation, no texture compression"
    prefs = op.parse_preferences("Arena", box, client)
    assert prefs.resolution.value == "high"
    assert {c.setting for c in prefs.clamped} == {"resolution", "frame_rate"}
    reqs = " ".join(u.request for u in prefs.unsupported)
    assert "eye-tracked" in reqs and "compression" in reqs


def test_unsupported_reported_even_without_a_model():
    prefs = op.parse_preferences("Arena", "please disable the memory guard", None)
    assert any("memory" in u.request for u in prefs.unsupported)


def test_preview_payload_warning_only_when_differs():
    empty = op.preview_payload(OptimizationPreferences())
    assert not empty["differs_from_auto"] and empty["warning"] == ""
    client = FakeClient({"items": [
        {"setting": "ffr", "value": "off", "source": "custom_box", "quote": "no FFR"}]})
    p = op.preview_payload(op.parse_preferences("x", "no FFR", client))
    assert p["differs_from_auto"] and p["warning"].startswith("Warning:")


# --- scene_complexity -------------------------------------------------------

def _box(eid, pos, size, tags=()):
    return {"id": eid, "transform": {"position": list(pos), "rotation": [0, 0, 0, 1],
                                     "scale": 1.0},
            "primitive": {"kind": "box", "size": list(size)}, "tags": list(tags),
            "mechanic_bindings": [], "physics": {"kind": "static"}}


def _room_doc():
    # Two 10x10 rooms side by side, each walled on all four sides (doorway gap
    # in the shared wall), plus a far room linked only through them.
    ents = [_box("ground", (0, -0.1, 0), (60, 0.2, 60), ["ground"])]
    for i, x0 in enumerate((0.0, 10.0, 20.0)):
        cx = x0 + 5
        ents += [_box(f"n{i}", (cx, 1.5, 0), (10, 3, 0.3)),
                 _box(f"s{i}", (cx, 1.5, 10), (10, 3, 0.3)),
                 _box(f"w{i}", (x0, 1.5, 3), (0.3, 3, 6)),
                 _box(f"e{i}", (x0 + 10, 1.5, 3), (0.3, 3, 6))]
    ents.append({"id": "crate", "transform": {"position": [25, 0.5, 5]},
                 "mesh": {"glb_path": "a_lod0.glb", "lod_paths": ["a_lod0.glb", "a_lod1.glb"],
                          "lod_triangle_counts": [5000, 1000], "bounds_radius_m": 0.5},
                 "mechanic_bindings": [{"id": "builtin.grab"}]})
    regions = [{"id": f"r{i}", "min": [x0, 0, 0], "max": [x0 + 10, 0, 10], "floor_y": 0}
               for i, x0 in enumerate((0.0, 10.0, 20.0))]
    links = [{"from": "r0", "to": "r1", "point": [10, 0, 8]},
             {"from": "r1", "to": "r2", "point": [20, 0, 8]}]
    return {"entities": ents, "navigation": {"regions": regions, "links": links},
            "performance": {"lod_switch_distances_m": [12, 30, 65], "cull_distance_m": 1e5,
                            "refresh_hz": 72, "governor": {"max_foveation_level": 3}}}


def test_closed_rooms_need_four_walls():
    doc = _room_doc()
    regions = sc._regions(doc)
    assert sc.closed_rooms(doc, regions) == ["r0", "r1", "r2"]
    doc["entities"] = [e for e in doc["entities"] if e["id"] != "s2"]
    assert "r2" not in sc.closed_rooms(doc, sc._regions(doc))


def test_analyse_counts_visibility_and_batching():
    doc = _room_doc()
    a = sc.analyse(doc)
    assert a["counts"]["primitives"] == 13 and a["counts"]["mesh_entities"] == 1
    assert a["visibility"]["samples"] > 0
    assert "crate" not in a["static_batch_ids"]  # it can be grabbed
    assert "ground" in a["static_batch_ids"]
    assert a["visibility_with_room_culling"]["visible_tris_p95"] <= \
        a["visibility"]["visible_tris_p95"]


def test_initial_settings_respect_locks():
    doc = _room_doc()
    perf = doc["performance"]
    perf["locks"] = {"ffr": {"value": "fixed", "level": 2}, "room_culling": {"value": "off"}}
    perf["foveation_level"] = 2
    perf["room_culling"] = False
    sc.decide_initial_settings(perf, sc.analyse(doc))
    assert perf["foveation_level"] == 2 and perf["room_culling"] is False
    perf2 = _room_doc()["performance"]
    sc.decide_initial_settings(perf2, sc.analyse(_room_doc()))
    assert perf2["room_culling"] is True and perf2["foveation_level"] == 1
    # PC VR has no fragment density map: no automatic FFR.
    perf3 = dict(_room_doc()["performance"], target_device="pcvr", foveation_level=0)
    sc.decide_initial_settings(perf3, sc.analyse(_room_doc()))
    assert perf3["foveation_level"] == 0


def test_static_batch_eligibility():
    assert sc.static_batch_eligible(_box("wall", (0, 0, 0), (1, 1, 1)))
    assert not sc.static_batch_eligible(_box("door", (0, 0, 0), (1, 1, 1), ["door"]))
    moving = _box("crate", (0, 0, 0), (1, 1, 1))
    moving["physics"] = {"kind": "dynamic"}
    assert not sc.static_batch_eligible(moving)


# --- verify: locks held ------------------------------------------------------

def _perf_line(ffr, scale=1.0):
    return (f"perf: 72.0 fps | work 1.0 ms avg, 1.0 ms worst | cpu 1.0 ms | gpu 9.0 ms avg, "
            f"9.0 ms worst | calls 10 pipes 2 sets 2 | draw 10 of 20 (cull frustum 0 dist 0 "
            f"small 0 room 0, batches 1) | tris 100 | meshes 1 resident (100 tris), 0 "
            f"streaming | rigs 0 (0 animating, budget 8) | lod_bias 0.0 cull 100000m ffr "
            f"{ffr} scale {scale:.2f}")


def test_locks_held_and_violated():
    scene = {"performance": {"locks": {"ffr": {"value": "off", "quote": "no FFR"},
                                       "resolution": {"value": "off"}}}}
    ok = evaluate_locks("\n".join([_perf_line(0)] * 3), scene)
    assert all(c.outcome == CheckOutcome.passed for c in ok) and len(ok) == 2
    bad = evaluate_locks("\n".join([_perf_line(0), _perf_line(2, 0.9)]), scene)
    assert all(c.outcome == CheckOutcome.failed for c in bad)


def test_user_caused_frame_miss_is_a_warning():
    scene = {"performance": {"locks": {"ffr": {"value": "off"}}}}
    text = _perf_line(0) + "\n[warn] governor: over budget, blocked by user settings (15 ms GPU"
    checks = evaluate_locks(text, scene)
    warn = [c for c in checks if c.outcome == CheckOutcome.warning]
    assert len(warn) == 1 and "user-chosen" in warn[0].name


def test_no_locks_no_checks():
    assert evaluate_locks(_perf_line(3), {"performance": {}}) == []


def test_render_health():
    from ptv_orchestrator.pipeline.stage_quest_verify import evaluate_render_health

    good = evaluate_render_health("Renderer ready\n" + _perf_line(1))
    assert [c.outcome for c in good] == [CheckOutcome.passed]
    bad = evaluate_render_health(
        "[error] Renderer: FFR render pass but no fragment density map image\n"
        "[warn] render: static batch 3 dissolved: a member moved\n")
    outcomes = {c.name: c.outcome for c in bad}
    assert outcomes["FFR density map reaches the render pass"] == CheckOutcome.failed
    assert outcomes["static batches stayed intact"] == CheckOutcome.warning
