#!/usr/bin/env python3
"""Emit a minimal rigged GLB used to test the skinning tier.

Nothing in the generation pipeline produces a rigged asset yet (Tripo
rig-check / rig lands later), so the skinning loader, the GPU skinning
pipeline, the pose system and the skin-preserving decimator all need a
known-good input that does not depend on the network or on a binary blob
checked into git. This script writes one on demand.

The fixture is a vertical column bound to two joints:

    bone_tip   (joint 1)  at y = 1, child of bone_root
    bone_root  (joint 0)  at the origin

Vertices below y = 0.5 belong entirely to ``bone_root``, vertices above
y = 1.5 entirely to ``bone_tip``, and the band between them blends linearly.
That band is the part that matters: it is the only place a bug in weight
decoding, weight quantization or joint indexing actually shows up.

It also carries one animation clip that exercises all three glTF
interpolation modes and all three animated paths at once:

    bone_tip  rotation     LINEAR
    bone_root translation  STEP
    bone_root scale        CUBICSPLINE

Usage:
    python make_rigged_fixture.py --output rigged_column.glb
"""

from __future__ import annotations

import argparse
import json
import math
import struct
from pathlib import Path

GLB_MAGIC = 0x46546C67
CHUNK_JSON = 0x4E4F534A
CHUNK_BIN = 0x004E4942

# glTF component types
UNSIGNED_BYTE = 5121
UNSIGNED_INT = 5125
FLOAT = 5126

# glTF bufferView targets
ARRAY_BUFFER = 34962
ELEMENT_ARRAY_BUFFER = 34963

COLUMN_HEIGHT = 2.0
COLUMN_RADIUS = 0.25
# The blend band. Outside it a vertex is fully owned by one joint.
BLEND_LOW = 0.5
BLEND_HIGH = 1.5
# bone_tip sits here in bind pose, so its inverse bind matrix is a -1 on y.
TIP_BIND_Y = 1.0
# Pivot offset used by the --no-skin variant. Kept here because the C++
# smoke test asserts against the exact same numbers.
STATIC_PIVOT_TRANSLATION = (1.0, 0.5, 0.0)


class BufferBuilder:
    """Accumulates the BIN chunk, keeping every view 4-byte aligned."""

    def __init__(self) -> None:
        self.blob = bytearray()
        self.views: list[dict] = []

    def add_view(self, data: bytes, target: int | None = None) -> int:
        while len(self.blob) % 4:
            self.blob.append(0)
        view = {"buffer": 0, "byteOffset": len(self.blob), "byteLength": len(data)}
        if target is not None:
            view["target"] = target
        self.blob.extend(data)
        self.views.append(view)
        return len(self.views) - 1


def _skin_weights(y: float) -> tuple[float, float]:
    """Root/tip influence for a vertex at height ``y``."""
    t = (y - BLEND_LOW) / (BLEND_HIGH - BLEND_LOW)
    t = min(1.0, max(0.0, t))
    return 1.0 - t, t


def build_geometry(rings: int, sides: int):
    """A closed column: ``sides``-gon cross-section, ``rings`` height levels."""
    positions: list[tuple[float, float, float]] = []
    normals: list[tuple[float, float, float]] = []
    colors: list[tuple[float, float, float, float]] = []
    joints: list[tuple[int, int, int, int]] = []
    weights: list[tuple[float, float, float, float]] = []
    indices: list[int] = []

    def emit(pos, nrm) -> int:
        y = pos[1]
        w_root, w_tip = _skin_weights(y)
        positions.append(pos)
        normals.append(nrm)
        # Gradient so a colour-channel regression is visible, not just spatial.
        shade = 0.35 + 0.65 * (y / COLUMN_HEIGHT)
        colors.append((shade, 0.45, 1.0 - shade, 1.0))
        joints.append((0, 1, 0, 0))
        weights.append((w_root, w_tip, 0.0, 0.0))
        return len(positions) - 1

    # --- side wall -------------------------------------------------------
    ring_start: list[int] = []
    for r in range(rings):
        y = COLUMN_HEIGHT * r / (rings - 1)
        ring_start.append(len(positions))
        for s in range(sides):
            a = 2.0 * math.pi * s / sides
            cx, cz = math.cos(a), math.sin(a)
            emit((COLUMN_RADIUS * cx, y, COLUMN_RADIUS * cz), (cx, 0.0, cz))

    for r in range(rings - 1):
        lo, hi = ring_start[r], ring_start[r + 1]
        for s in range(sides):
            n = (s + 1) % sides
            indices.extend([lo + s, lo + n, hi + n])
            indices.extend([lo + s, hi + n, hi + s])

    # --- caps, with their own vertices so the normals are correct --------
    for y, ny in ((0.0, -1.0), (COLUMN_HEIGHT, 1.0)):
        base = len(positions)
        for s in range(sides):
            a = 2.0 * math.pi * s / sides
            emit(
                (COLUMN_RADIUS * math.cos(a), y, COLUMN_RADIUS * math.sin(a)),
                (0.0, ny, 0.0),
            )
        for s in range(1, sides - 1):
            if ny < 0.0:
                indices.extend([base, base + s + 1, base + s])
            else:
                indices.extend([base, base + s, base + s + 1])

    return positions, normals, colors, joints, weights, indices


def build_gltf(rings: int, sides: int, skinned: bool = True) -> tuple[dict, bytes]:
    """Build the fixture document.

    ``skinned=False`` emits the same column with no rig, parented under a
    rotated + translated pivot node. That variant exists to test the opposite
    rule from the skinned one: a static mesh MUST inherit its node's world
    transform, while a skinned mesh must ignore it.
    """
    positions, normals, colors, joints, weights, indices = build_geometry(
        rings, sides
    )
    buf = BufferBuilder()

    idx_view = buf.add_view(
        struct.pack(f"<{len(indices)}I", *indices), ELEMENT_ARRAY_BUFFER
    )
    pos_view = buf.add_view(
        b"".join(struct.pack("<3f", *p) for p in positions), ARRAY_BUFFER
    )
    nrm_view = buf.add_view(
        b"".join(struct.pack("<3f", *n) for n in normals), ARRAY_BUFFER
    )
    col_view = buf.add_view(
        b"".join(struct.pack("<4f", *c) for c in colors), ARRAY_BUFFER
    )
    jnt_view = buf.add_view(
        b"".join(struct.pack("<4B", *j) for j in joints), ARRAY_BUFFER
    )
    wgt_view = buf.add_view(
        b"".join(struct.pack("<4f", *w) for w in weights), ARRAY_BUFFER
    )

    # Inverse bind matrices, column-major. joint 0 sits at the origin so its
    # IBM is the identity; joint 1 sits at y = TIP_BIND_Y so its IBM undoes it.
    ibm = [
        [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1],
        [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -TIP_BIND_Y, 0, 1],
    ]
    ibm_view = buf.add_view(
        b"".join(struct.pack("<16f", *[float(v) for v in m]) for m in ibm)
    )

    # --- animation data --------------------------------------------------
    rot_times = [0.0, 0.5, 1.0]
    half = math.radians(45.0) * 0.5
    rot_values = [
        (0.0, 0.0, 0.0, 1.0),
        (0.0, 0.0, math.sin(half), math.cos(half)),
        (0.0, 0.0, 0.0, 1.0),
    ]
    trs_times = [0.0, 0.5, 1.0]
    trs_values = [(0.0, 0.0, 0.0), (0.1, 0.0, 0.0), (0.0, 0.0, 0.0)]
    # CUBICSPLINE stores in-tangent, value, out-tangent for every key.
    scl_times = [0.0, 1.0]
    scl_values = [
        (0.0, 0.0, 0.0), (1.0, 1.0, 1.0), (0.0, 0.0, 0.0),
        (0.0, 0.0, 0.0), (1.0, 1.2, 1.0), (0.0, 0.0, 0.0),
    ]

    rot_t_view = buf.add_view(struct.pack(f"<{len(rot_times)}f", *rot_times))
    rot_v_view = buf.add_view(b"".join(struct.pack("<4f", *v) for v in rot_values))
    trs_t_view = buf.add_view(struct.pack(f"<{len(trs_times)}f", *trs_times))
    trs_v_view = buf.add_view(b"".join(struct.pack("<3f", *v) for v in trs_values))
    scl_t_view = buf.add_view(struct.pack(f"<{len(scl_times)}f", *scl_times))
    scl_v_view = buf.add_view(b"".join(struct.pack("<3f", *v) for v in scl_values))

    px = [p[0] for p in positions]
    py = [p[1] for p in positions]
    pz = [p[2] for p in positions]

    accessors = [
        {"bufferView": idx_view, "componentType": UNSIGNED_INT,
         "count": len(indices), "type": "SCALAR"},
        {"bufferView": pos_view, "componentType": FLOAT,
         "count": len(positions), "type": "VEC3",
         "min": [min(px), min(py), min(pz)],
         "max": [max(px), max(py), max(pz)]},
        {"bufferView": nrm_view, "componentType": FLOAT,
         "count": len(normals), "type": "VEC3"},
        {"bufferView": col_view, "componentType": FLOAT,
         "count": len(colors), "type": "VEC4"},
        {"bufferView": jnt_view, "componentType": UNSIGNED_BYTE,
         "count": len(joints), "type": "VEC4"},
        {"bufferView": wgt_view, "componentType": FLOAT,
         "count": len(weights), "type": "VEC4"},
        {"bufferView": ibm_view, "componentType": FLOAT,
         "count": 2, "type": "MAT4"},
        {"bufferView": rot_t_view, "componentType": FLOAT,
         "count": len(rot_times), "type": "SCALAR",
         "min": [min(rot_times)], "max": [max(rot_times)]},
        {"bufferView": rot_v_view, "componentType": FLOAT,
         "count": len(rot_values), "type": "VEC4"},
        {"bufferView": trs_t_view, "componentType": FLOAT,
         "count": len(trs_times), "type": "SCALAR",
         "min": [min(trs_times)], "max": [max(trs_times)]},
        {"bufferView": trs_v_view, "componentType": FLOAT,
         "count": len(trs_values), "type": "VEC3"},
        {"bufferView": scl_t_view, "componentType": FLOAT,
         "count": len(scl_times), "type": "SCALAR",
         "min": [min(scl_times)], "max": [max(scl_times)]},
        {"bufferView": scl_v_view, "componentType": FLOAT,
         "count": len(scl_values), "type": "VEC3"},
    ]

    if skinned:
        nodes = [
            {"name": "SkinnedColumn", "mesh": 0, "skin": 0},
            {"name": "bone_root", "children": [2]},
            {"name": "bone_tip", "translation": [0.0, TIP_BIND_Y, 0.0]},
        ]
        scene_nodes = [0, 1]
        attributes = {"POSITION": 1, "NORMAL": 2, "COLOR_0": 3,
                      "JOINTS_0": 4, "WEIGHTS_0": 5}
    else:
        # 90 degrees about +Y, so (x, y, z) -> (z, y, -x), then lifted and
        # pushed along +X. Both parts are recoverable from a single vertex.
        s = math.sin(math.radians(90.0) * 0.5)
        c = math.cos(math.radians(90.0) * 0.5)
        nodes = [
            {"name": "Pivot", "children": [1],
             "rotation": [0.0, s, 0.0, c],
             "translation": list(STATIC_PIVOT_TRANSLATION)},
            {"name": "Column", "mesh": 0},
        ]
        scene_nodes = [0]
        attributes = {"POSITION": 1, "NORMAL": 2, "COLOR_0": 3}

    doc = {
        "asset": {"version": "2.0", "generator": "ptv make_rigged_fixture"},
        "scene": 0,
        "scenes": [{"nodes": scene_nodes}],
        "nodes": nodes,
        "meshes": [{
            "name": "column",
            "primitives": [{
                "attributes": attributes,
                "indices": 0,
                "material": 0,
                "mode": 4,
            }],
        }],
        "materials": [{
            "name": "column_mat",
            "pbrMetallicRoughness": {
                "baseColorFactor": [1.0, 1.0, 1.0, 1.0],
                "metallicFactor": 0.0,
                "roughnessFactor": 0.8,
            },
            "doubleSided": True,
        }],
        "bufferViews": buf.views,
        "accessors": accessors,
        "buffers": [{"byteLength": len(buf.blob)}],
    }

    if skinned:
        doc["skins"] = [{"name": "column_rig", "joints": [1, 2],
                         "inverseBindMatrices": 6}]
        doc["animations"] = [{
            "name": "bend",
            "samplers": [
                {"input": 7, "output": 8, "interpolation": "LINEAR"},
                {"input": 9, "output": 10, "interpolation": "STEP"},
                {"input": 11, "output": 12, "interpolation": "CUBICSPLINE"},
            ],
            "channels": [
                {"sampler": 0, "target": {"node": 2, "path": "rotation"}},
                {"sampler": 1, "target": {"node": 1, "path": "translation"}},
                {"sampler": 2, "target": {"node": 1, "path": "scale"}},
            ],
        }]
    return doc, bytes(buf.blob)


def write_glb(doc: dict, blob: bytes, out_path: Path) -> None:
    json_bytes = json.dumps(doc, separators=(",", ":")).encode("utf-8")
    json_bytes += b" " * ((4 - len(json_bytes) % 4) % 4)
    bin_bytes = blob + b"\x00" * ((4 - len(blob) % 4) % 4)

    total = 12 + 8 + len(json_bytes) + 8 + len(bin_bytes)
    out = bytearray()
    out += struct.pack("<III", GLB_MAGIC, 2, total)
    out += struct.pack("<II", len(json_bytes), CHUNK_JSON)
    out += json_bytes
    out += struct.pack("<II", len(bin_bytes), CHUNK_BIN)
    out += bin_bytes

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(bytes(out))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--output", required=True, type=Path)
    ap.add_argument("--rings", type=int, default=17,
                    help="height subdivisions (>= 2)")
    ap.add_argument("--sides", type=int, default=12,
                    help="cross-section vertices (>= 3)")
    ap.add_argument("--no-skin", action="store_true",
                    help="emit the static variant under a rotated pivot node")
    args = ap.parse_args()

    if args.rings < 2 or args.sides < 3:
        ap.error("--rings must be >= 2 and --sides >= 3")

    doc, blob = build_gltf(args.rings, args.sides, skinned=not args.no_skin)
    write_glb(doc, blob, args.output)

    kind = "static under a pivot" if args.no_skin else "2 joints, 1 clip"
    print(
        f"wrote {args.output} "
        f"({doc['accessors'][1]['count']} verts, "
        f"{doc['accessors'][0]['count'] // 3} tris, {kind})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
