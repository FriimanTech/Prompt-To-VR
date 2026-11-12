"""Colliders are measured from the mesh itself: real bounds (including how
far they sit from the origin), true hull points, and a static triangle LOD
accurate enough to stand things on."""

from __future__ import annotations

import struct
from pathlib import Path

from ptv_orchestrator.pipeline.collision_shapes import (
    choose_mesh_lod,
    ensure_records,
    hull_points,
    measure,
    to_meters,
)
from ptv_orchestrator.pipeline.uthana.gltf_io import Glb


def _box_glb(path: Path, lo, hi, node_translation=None) -> Path:
    """A one-mesh GLB holding the 8 corners of an axis-aligned box."""
    corners = [(x, y, z) for x in (lo[0], hi[0]) for y in (lo[1], hi[1])
               for z in (lo[2], hi[2])]
    glb = Glb(doc={"asset": {"version": "2.0"}, "buffers": [{"byteLength": 0}],
                   "meshes": [], "nodes": [], "scenes": [{"nodes": [0]}], "scene": 0},
              bin=bytearray())
    pos = glb.append_float_accessor(corners, "VEC3")
    idx_offset = len(glb.bin)
    tris = [0, 1, 2, 1, 3, 2, 4, 6, 5, 5, 6, 7, 0, 4, 1, 1, 4, 5,
            2, 3, 6, 3, 7, 6, 0, 2, 4, 2, 6, 4, 1, 5, 3, 3, 5, 7]
    glb.bin += struct.pack("<" + "H" * len(tris), *tris)
    glb.doc["bufferViews"].append({"buffer": 0, "byteOffset": idx_offset,
                                   "byteLength": 2 * len(tris)})
    glb.doc["accessors"].append({"bufferView": len(glb.doc["bufferViews"]) - 1,
                                 "componentType": 5123, "count": len(tris),
                                 "type": "SCALAR"})
    glb.doc["meshes"].append({"primitives": [{"attributes": {"POSITION": pos},
                                              "indices": len(glb.doc["accessors"]) - 1}]})
    node = {"mesh": 0}
    if node_translation:
        node["translation"] = list(node_translation)
    glb.doc["nodes"].append(node)
    glb.save(path)
    return path


def test_measure_keeps_the_origin_offset(tmp_path: Path):
    # A Tripo-style centred table: 1.4 x 0.75 x 1.4 units, origin in the middle.
    lod0 = _box_glb(tmp_path / "t_lod0.glb", (-0.7, -0.375, -0.7), (0.7, 0.375, 0.7))
    rec = measure([str(lod0)], [0.0], tmp_path)
    assert rec["aabb_min"][1] == -0.375 and rec["aabb_max"][1] == 0.375
    assert len(rec["hull"]) == 8  # every corner, nothing else
    m = to_meters(rec, 2.0)
    assert m["aabb_min"] == [-1.4, -0.75, -1.4]


def test_node_transforms_are_applied(tmp_path: Path):
    lod0 = _box_glb(tmp_path / "g_lod0.glb", (-0.03, 0.0, -0.025), (0.03, 0.09, 0.025),
                    node_translation=(0.0, 0.5, 0.0))
    rec = measure([str(lod0)], [0.0], tmp_path)
    assert abs(rec["aabb_min"][1] - 0.5) < 1e-6
    assert abs(rec["aabb_max"][1] - 0.59) < 1e-6


def test_collision_lod_is_the_coarsest_accurate_one():
    assert choose_mesh_lod([0.0, 0.003, 0.023, 0.18], 4) == 2
    assert choose_mesh_lod([0.0, 0.2, 0.3], 3) == 0
    assert choose_mesh_lod([], 0) == -1


def test_hull_points_are_extreme_vertices():
    pts = [(0, 0, 0), (1, 0, 0), (0, 1, 0), (0, 0, 1), (0.2, 0.2, 0.2)]
    hull = hull_points(pts)
    assert [0.2, 0.2, 0.2] not in hull  # interior point never chosen
    assert len(hull) == 4


def test_ensure_records_skips_characters(tmp_path: Path):
    lod0 = _box_glb(tmp_path / "p_lod0.glb", (-0.1, -0.1, -0.1), (0.1, 0.1, 0.1))
    norm = {
        "prop": {"lod_paths": [str(lod0)], "lod_errors_rel": [0.0]},
        "hero": {"lod_paths": [str(lod0)], "lod_errors_rel": [0.0], "skinned": True},
        "__performance__": {},
    }
    assert ensure_records(norm, tmp_path) == ["prop"]
    assert "collision" in norm["prop"] and "collision" not in norm["hero"]
    assert ensure_records(norm, tmp_path) == []  # cached
