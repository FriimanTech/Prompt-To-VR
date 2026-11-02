"""Textured assets: size policy, budget fitting, GLB references, the headset
screenshot judge and the Quest telemetry parsers."""

from __future__ import annotations

import json
import struct
from pathlib import Path

import numpy as np

from ptv_orchestrator.pipeline import quest_metrics as qm
from ptv_orchestrator.pipeline import texture_gallery as tg
from ptv_orchestrator.pipeline import texture_policy as tp
from ptv_orchestrator.pipeline.texture_refs import check_mesh_textures, glb_texture_refs


# --- texture_policy ---------------------------------------------------------

def test_needed_size_scales_with_distance():
    near = tp.needed_size(0.25, 0.4)  # a held pistol
    far = tp.needed_size(0.25, 20.0)
    assert near >= 1024 and far == tp.MIN_SIZE
    assert near & (near - 1) == 0  # power of two


def test_size_cap_by_importance_and_source():
    assert tp.size_cap("background", 1.0, 30.0, 4096) == 512
    assert tp.size_cap("held", 0.3, 0.4, 4096) == 2048
    # never above the source image
    assert tp.size_cap("held", 0.3, 0.4, 600) == 512
    # a big object the player can walk up to gets the next class up
    assert tp.size_cap("background", 10.0, 2.0, 4096) == 1024


def test_fit_budget_shrinks_least_visible_first():
    held = tp.TextureNeed(hash="held", source_size=2048, importance="held", size_m=0.3,
                          closest_view_m=0.4)
    sky = tp.TextureNeed(hash="sky", source_size=2048, importance="background",
                         size_m=30.0, closest_view_m=60.0)
    needs = [held, sky]
    tp.choose_sizes(needs)
    before_held = held.chosen
    budget = tp.total_bytes(needs) - 1
    notes = tp.fit_budget(needs, budget)
    assert notes and sky.reductions and held.chosen == before_held


def test_fit_budget_never_below_min_size():
    n = tp.TextureNeed(hash="a", source_size=2048, importance="standard")
    tp.choose_sizes([n])
    tp.fit_budget([n], 1)
    assert n.chosen == tp.MIN_SIZE


def test_astc_bytes_counts_mips():
    assert tp.astc_bytes(6, block=6, mips=False) == 16
    assert tp.astc_bytes(1024, 6) > tp.astc_bytes(1024, 6, mips=False)


# --- texture_refs -----------------------------------------------------------

def _glb(path: Path, doc: dict) -> Path:
    js = json.dumps(doc).encode("utf-8")
    js += b" " * ((4 - len(js) % 4) % 4)
    total = 12 + 8 + len(js)
    path.write_bytes(struct.pack("<III", 0x46546C67, 2, total)
                     + struct.pack("<II", len(js), 0x4E4F534A) + js)
    return path


def test_texture_refs_and_missing_files(tmp_path: Path):
    (tmp_path / "ptv_textures").mkdir()
    (tmp_path / "ptv_textures" / "a.png").write_bytes(b"png")
    glb = _glb(tmp_path / "m.glb", {
        "asset": {"version": "2.0"},
        "images": [{"uri": "ptv_textures/a.png"}, {"uri": "ptv_textures/b.png"}],
        "textures": [
            {"source": 0, "extensions": {"PTV_texture_astc": {"uri": "ptv_textures/a.ktx2"}}},
            {"source": 1},
        ],
    })
    refs = glb_texture_refs(glb)
    assert [r["uri"] for r in refs] == ["ptv_textures/a.png", "ptv_textures/b.png"]
    missing, no_ktx2, n = check_mesh_textures(glb, need_ktx2=True)
    assert n == 2
    assert any("a.ktx2" in m for m in missing) and any("b.png" in m for m in missing)
    assert no_ktx2  # the second texture has no ASTC twin


# --- texture_gallery --------------------------------------------------------

def _img(colour, size=64, square=None, sq_colour=None):
    a = np.zeros((size, size, 3), np.uint8)
    a[:] = colour
    if square:
        a[square[0]:square[1], square[0]:square[1]] = sq_colour
    return a


def test_gallery_judge_colour_survives():
    bg = _img((30, 20, 60))
    shot = _img((30, 20, 60), square=(16, 48), sq_colour=(200, 40, 40))
    stats = tg.screenshot_object_stats(shot, bg)
    ref = tg.colour_stats(np.full((100, 3), (200, 40, 40), np.uint8))
    ok, notes = tg.judge(stats, ref, textured=1, missing=0)
    assert ok, notes
    assert stats["share"] > 0.2


def test_gallery_judge_flags_white_magenta_and_invisible():
    bg = _img((30, 20, 60))
    red_ref = tg.colour_stats(np.full((100, 3), (200, 40, 40), np.uint8))
    white = tg.screenshot_object_stats(
        _img((30, 20, 60), square=(16, 48), sq_colour=(255, 255, 255)), bg)
    ok, notes = tg.judge(white, red_ref, textured=1, missing=0)
    assert not ok and any("white" in n or "colour lost" in n for n in notes)
    magenta = tg.screenshot_object_stats(
        _img((30, 20, 60), square=(16, 48), sq_colour=(255, 0, 255)), bg)
    ok, notes = tg.judge(magenta, {}, textured=1, missing=0)
    assert not ok and any("magenta" in n for n in notes)
    ok, notes = tg.judge(tg.screenshot_object_stats(bg, bg), red_ref, 1, 0)
    assert not ok and "barely visible" in notes[0]


def test_parse_gallery_lines():
    text = ("I/ptv: [info] texgallery: begin 2\n"
            "I/ptv: [info] texgallery: show 1/2 assets/a_lod0.glb textured=1 missing=0 "
            "tris=100 skinned=0\n")
    rows = tg.parse_gallery_lines(text)
    assert rows == [{"index": 1, "total": 2, "path": "assets/a_lod0.glb", "textured": 1,
                     "missing": 0, "tris": 100, "skinned": 0}]


# --- quest_metrics ----------------------------------------------------------

PERF = ("11-01 12:52:18.766 I/ptv (22115): [info] perf: 72.0 fps | work 1.4 ms avg, 2.0 ms "
        "worst | cpu 1.4 ms | gpu 8.8 ms avg, 9.3 ms worst | calls 14 pipes 3 sets 3 | "
        "draw 14 of 186 (cull frustum 10 dist 0 small 2 room 5, batches 10) | tris 10000 | "
        "meshes 11 resident (50402 tris), 0 streaming | rigs 2 (0 animating, budget 16) | "
        "lod_bias 0.0 cull 100000m ffr 2 scale 0.90")


def test_parse_perf_line_with_new_fields():
    row = qm.parse_perf_lines(PERF)[0]
    assert row["fps"] == 72.0 and row["gpu"] == 8.8 and row["calls"] == 14
    assert row["small"] == 2 and row["room"] == 5 and row["batches"] == 10
    assert row["ffr"] == 2 and abs(row["scale"] - 0.9) < 1e-6
    summary = qm.summarize_perf([row] * 6)
    assert summary["gpu"] == 8.8 and summary["batches"] == 10


def test_parse_memory_tools():
    dumpsys = "** MEMINFO in pid 1 [x] **\n  Native Heap  5000 1 2\n  TOTAL PSS:   204800  TOTAL RSS: 300000\n"
    m = qm.parse_dumpsys_meminfo(dumpsys)
    assert m["total_pss_kb"] == 204800 and m["native_heap_kb"] == 5000
    assert qm.parse_dumpsys_meminfo("No process found for: x") == {}
    gpu = qm.parse_gpumeminfo("vk_devicememory,10485760,0,4\ntotal,20971520\n")
    assert gpu["vk_devicememory_mb"] == 10.0 and gpu["total_mb"] == 20.0
    assert qm.parse_mem_lines("mem: pss 66 MB (target 4608") == [66]
    kills = qm.low_memory_kills(
        "lowmemorykiller: Kill 'com.ptv.game' (123)\nother line com.ptv.game\n", "com.ptv.game")
    assert len(kills) == 1
