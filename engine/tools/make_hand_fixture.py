#!/usr/bin/env python3
"""Emit the rigged player-hand GLBs the engine draws for the VR controllers.

Tripo cannot supply this. Every rig type its auto-rigger offers -- biped,
quadruped, hexapod, octopod, avian, serpentine, aquatic -- describes a whole
creature, so a disembodied hand comes back ``riggable: false``. Generating the
hand here keeps it deterministic, offline, and out of git as a binary.

Skeleton: 16 joints, comfortably under the 256-joint cap the byte-packed
JOINTS_0 attribute imposes.

    hand_root                      wrist, the mesh's anchor
      thumb_01 -> _02 -> _03
      index_01 -> _02 -> _03
      middle_01 -> _02 -> _03
      ring_01 -> _02 -> _03
      pinky_01 -> _02 -> _03

Names are prefixed ``mixamorig:`` to match the ``spec="mixamo"`` convention the
auto-rigging stage requests for characters, so generated C++ resolves joints
the same way regardless of where the rig came from.

Geometry is deliberately blocky: each bone owns one tapered box segment, hard
bound to that joint (weight 1.0) except at the knuckles, where a narrow band
blends between parent and child so the finger creases instead of shearing.

The pose is authored in the OpenXR ``/grip/pose`` frame, per the 1.1 spec's
standard pose identifiers, matching the procedural hand this replaces:

    -Z  forward, from pinky toward index when making a fist
    +Z  backward, toward the forearm
    +Y  orthogonal up
    +X  palm normal

Usage:
    python make_hand_fixture.py --output hand_left.glb --hand left
"""

from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path

GLB_MAGIC = 0x46546C67
CHUNK_JSON = 0x4E4F534A
CHUNK_BIN = 0x004E4942

UNSIGNED_BYTE = 5121
UNSIGNED_INT = 5125
FLOAT = 5126

ARRAY_BUFFER = 34962
ELEMENT_ARRAY_BUFFER = 34963

BONE_PREFIX = "mixamorig:"

# Palm block, in metres. Roughly a real hand minus the fingers.
PALM_HALF_X = 0.022   # palm thickness
PALM_HALF_Y = 0.042   # pinky-to-index span
PALM_LEN_Z = 0.075    # wrist to knuckles, along -Z

# Per-finger layout: (name, y offset across the palm, total length,
# segment radius, forward lean so the fingers fan slightly).
FINGERS = [
    ("index",  0.030, 0.072, 0.0085),
    ("middle", 0.010, 0.078, 0.0090),
    ("ring",  -0.010, 0.072, 0.0085),
    ("pinky", -0.030, 0.056, 0.0075),
]
# Segment length split along a finger: proximal, intermediate, distal.
SEGMENT_SPLIT = (0.42, 0.34, 0.24)

THUMB_NAME = "thumb"
THUMB_LEN = 0.058
THUMB_RADIUS = 0.010


class BufferBuilder:
    """Accumulates the BIN chunk, keeping every view 4-byte aligned."""

    def __init__(self) -> None:
        self.blob = bytearray()
        self.views: list[dict] = []

    def add_view(self, data: bytes, target: int | None = None) -> int:
        while len(self.blob) % 4:
            self.blob.append(0)
        view = {
            "buffer": 0,
            "byteOffset": len(self.blob),
            "byteLength": len(data),
        }
        if target is not None:
            view["target"] = target
        self.blob.extend(data)
        self.views.append(view)
        return len(self.views) - 1


class Skeleton:
    """Joint list in skin order, which is also JOINTS_0 index order."""

    def __init__(self) -> None:
        self.names: list[str] = []
        self.parents: list[int] = []
        self.translations: list[tuple[float, float, float]] = []

    def add(self, name: str, parent: int, translation) -> int:
        self.names.append(BONE_PREFIX + name)
        self.parents.append(parent)
        self.translations.append(tuple(float(v) for v in translation))
        return len(self.names) - 1

    def world_translation(self, joint: int) -> tuple[float, float, float]:
        """Bind-pose world position. Every joint here is translation-only, so
        this is a plain sum up the parent chain -- no matrix maths needed."""
        x = y = z = 0.0
        cur = joint
        while cur >= 0:
            tx, ty, tz = self.translations[cur]
            x += tx
            y += ty
            z += tz
            cur = self.parents[cur]
        return (x, y, z)

    def index_of(self, short_name: str) -> int:
        return self.names.index(BONE_PREFIX + short_name)

    def __len__(self) -> int:
        return len(self.names)


def build_skeleton(mirror: float) -> Skeleton:
    """``mirror`` is +1 for the right hand, -1 for the left.

    Only the across-palm axis flips. Fingers still extend along -Z on both
    hands, which is what keeps the mesh usable for either side without
    re-authoring it.
    """
    skel = Skeleton()
    root = skel.add("hand_root", -1, (0.0, 0.0, 0.0))

    for name, y_off, length, _radius in FINGERS:
        # Knuckle sits at the far end of the palm.
        parent = skel.add(
            f"{name}_01", root, (0.0, y_off * mirror, -PALM_LEN_Z)
        )
        seg = skel.add(f"{name}_02", parent, (0.0, 0.0, -length * SEGMENT_SPLIT[0]))
        skel.add(f"{name}_03", seg, (0.0, 0.0, -length * SEGMENT_SPLIT[1]))

    # The thumb leaves the side of the palm rather than the end, angled out
    # and forward; that offset is what makes a fist read as a fist.
    thumb = skel.add(
        f"{THUMB_NAME}_01",
        root,
        (PALM_HALF_X * 0.6 * mirror, 0.042 * mirror, -PALM_LEN_Z * 0.30),
    )
    t2 = skel.add(
        f"{THUMB_NAME}_02", thumb,
        (0.0, 0.012 * mirror, -THUMB_LEN * SEGMENT_SPLIT[0]),
    )
    skel.add(
        f"{THUMB_NAME}_03", t2,
        (0.0, 0.008 * mirror, -THUMB_LEN * SEGMENT_SPLIT[1]),
    )
    return skel


class MeshBuilder:
    def __init__(self) -> None:
        self.positions: list[tuple[float, float, float]] = []
        self.normals: list[tuple[float, float, float]] = []
        self.colors: list[tuple[float, float, float, float]] = []
        self.joints: list[tuple[int, int, int, int]] = []
        self.weights: list[tuple[float, float, float, float]] = []
        self.indices: list[int] = []

    def add_box(self, center, half, joints, weights, color) -> None:
        """One axis-aligned box, every vertex bound to the same joints.

        Flat-shaded: each face gets its own four vertices so knuckles read as
        creases instead of a smooth blob.
        """
        cx, cy, cz = center
        hx, hy, hz = half
        faces = [
            ((1, 0, 0), [(1, -1, -1), (1, 1, -1), (1, 1, 1), (1, -1, 1)]),
            ((-1, 0, 0), [(-1, -1, 1), (-1, 1, 1), (-1, 1, -1), (-1, -1, -1)]),
            ((0, 1, 0), [(-1, 1, -1), (-1, 1, 1), (1, 1, 1), (1, 1, -1)]),
            ((0, -1, 0), [(-1, -1, 1), (-1, -1, -1), (1, -1, -1), (1, -1, 1)]),
            ((0, 0, 1), [(-1, -1, 1), (1, -1, 1), (1, 1, 1), (-1, 1, 1)]),
            ((0, 0, -1), [(1, -1, -1), (-1, -1, -1), (-1, 1, -1), (1, 1, -1)]),
        ]
        for normal, corners in faces:
            base = len(self.positions)
            for sx, sy, sz in corners:
                self.positions.append((cx + sx * hx, cy + sy * hy, cz + sz * hz))
                self.normals.append(normal)
                self.colors.append(color)
                self.joints.append(joints)
                self.weights.append(weights)
            self.indices.extend([base, base + 1, base + 2])
            self.indices.extend([base, base + 2, base + 3])


def build_mesh(skel: Skeleton, mirror: float) -> MeshBuilder:
    mb = MeshBuilder()
    root = skel.index_of("hand_root")

    skin_tone = (0.62, 0.44, 0.36, 1.0)
    knuckle_tone = (0.55, 0.38, 0.32, 1.0)

    # --- palm, rigid to the wrist ---------------------------------------
    mb.add_box(
        center=(0.0, 0.0, -PALM_LEN_Z * 0.5),
        half=(PALM_HALF_X, PALM_HALF_Y, PALM_LEN_Z * 0.5),
        joints=(root, 0, 0, 0),
        weights=(1.0, 0.0, 0.0, 0.0),
        color=skin_tone,
    )

    def add_chain(name: str, length: float, radius: float) -> None:
        parent_joint = root
        for seg_idx, split in enumerate(SEGMENT_SPLIT):
            joint = skel.index_of(f"{name}_0{seg_idx + 1}")
            seg_len = length * split
            jx, jy, jz = skel.world_translation(joint)
            taper = radius * (1.0 - 0.15 * seg_idx)

            # Body of the segment: fully owned by its own joint.
            mb.add_box(
                center=(jx, jy, jz - seg_len * 0.5),
                half=(taper, taper, seg_len * 0.5),
                joints=(joint, 0, 0, 0),
                weights=(1.0, 0.0, 0.0, 0.0),
                color=skin_tone,
            )

            # Knuckle band straddling the joint, split 50/50 with the parent.
            # Without this the mesh shears open into a visible gap the moment
            # the finger bends -- it is the whole reason this asset is skinned
            # rather than a pile of separate boxes.
            mb.add_box(
                center=(jx, jy, jz),
                half=(taper * 0.95, taper * 0.95, taper * 0.6),
                joints=(joint, parent_joint, 0, 0),
                weights=(0.5, 0.5, 0.0, 0.0),
                color=knuckle_tone,
            )
            parent_joint = joint

    for name, _y, length, radius in FINGERS:
        add_chain(name, length, radius)
    add_chain(THUMB_NAME, THUMB_LEN, THUMB_RADIUS)

    _ = mirror  # geometry follows the skeleton, which is already mirrored
    return mb


def build_gltf(hand: str) -> tuple[dict, bytes]:
    mirror = 1.0 if hand == "right" else -1.0
    skel = build_skeleton(mirror)
    mb = build_mesh(skel, mirror)
    buf = BufferBuilder()

    idx_view = buf.add_view(
        struct.pack(f"<{len(mb.indices)}I", *mb.indices), ELEMENT_ARRAY_BUFFER
    )
    pos_view = buf.add_view(
        b"".join(struct.pack("<3f", *p) for p in mb.positions), ARRAY_BUFFER
    )
    nrm_view = buf.add_view(
        b"".join(struct.pack("<3f", *n) for n in mb.normals), ARRAY_BUFFER
    )
    col_view = buf.add_view(
        b"".join(struct.pack("<4f", *c) for c in mb.colors), ARRAY_BUFFER
    )
    jnt_view = buf.add_view(
        b"".join(struct.pack("<4B", *j) for j in mb.joints), ARRAY_BUFFER
    )
    wgt_view = buf.add_view(
        b"".join(struct.pack("<4f", *w) for w in mb.weights), ARRAY_BUFFER
    )

    # Inverse bind matrix = inverse of the joint's bind world transform.
    # Translation-only skeleton, so that is just a negated translation.
    ibm_bytes = bytearray()
    for j in range(len(skel)):
        wx, wy, wz = skel.world_translation(j)
        m = [1.0, 0.0, 0.0, 0.0,
             0.0, 1.0, 0.0, 0.0,
             0.0, 0.0, 1.0, 0.0,
             -wx, -wy, -wz, 1.0]
        ibm_bytes += struct.pack("<16f", *m)
    ibm_view = buf.add_view(bytes(ibm_bytes))

    px = [p[0] for p in mb.positions]
    py = [p[1] for p in mb.positions]
    pz = [p[2] for p in mb.positions]

    accessors = [
        {"bufferView": idx_view, "componentType": UNSIGNED_INT,
         "count": len(mb.indices), "type": "SCALAR"},
        {"bufferView": pos_view, "componentType": FLOAT,
         "count": len(mb.positions), "type": "VEC3",
         "min": [min(px), min(py), min(pz)],
         "max": [max(px), max(py), max(pz)]},
        {"bufferView": nrm_view, "componentType": FLOAT,
         "count": len(mb.normals), "type": "VEC3"},
        {"bufferView": col_view, "componentType": FLOAT,
         "count": len(mb.colors), "type": "VEC4"},
        {"bufferView": jnt_view, "componentType": UNSIGNED_BYTE,
         "count": len(mb.joints), "type": "VEC4"},
        {"bufferView": wgt_view, "componentType": FLOAT,
         "count": len(mb.weights), "type": "VEC4"},
        {"bufferView": ibm_view, "componentType": FLOAT,
         "count": len(skel), "type": "MAT4"},
    ]

    # Node 0 is the skinned mesh; joints occupy nodes 1..N in skin order.
    nodes: list[dict] = [
        {"name": f"hand_{hand}", "mesh": 0, "skin": 0},
    ]
    children: list[list[int]] = [[] for _ in range(len(skel))]
    roots: list[int] = []
    for j, parent in enumerate(skel.parents):
        if parent >= 0:
            children[parent].append(j)
        else:
            roots.append(j)
    for j in range(len(skel)):
        node = {
            "name": skel.names[j],
            "translation": list(skel.translations[j]),
        }
        if children[j]:
            node["children"] = [c + 1 for c in children[j]]
        nodes.append(node)

    doc = {
        "asset": {"version": "2.0", "generator": "ptv make_hand_fixture"},
        "scene": 0,
        "scenes": [{"nodes": [0] + [r + 1 for r in roots]}],
        "nodes": nodes,
        "skins": [{
            "name": f"hand_{hand}_rig",
            "joints": list(range(1, len(skel) + 1)),
            "inverseBindMatrices": 6,
        }],
        "meshes": [{
            "name": f"hand_{hand}",
            "primitives": [{
                "attributes": {"POSITION": 1, "NORMAL": 2, "COLOR_0": 3,
                               "JOINTS_0": 4, "WEIGHTS_0": 5},
                "indices": 0,
                "material": 0,
                "mode": 4,
            }],
        }],
        "materials": [{
            "name": "hand_mat",
            "pbrMetallicRoughness": {
                "baseColorFactor": [1.0, 1.0, 1.0, 1.0],
                "metallicFactor": 0.0,
                "roughnessFactor": 0.9,
            },
            "doubleSided": True,
        }],
        "bufferViews": buf.views,
        "accessors": accessors,
        "buffers": [{"byteLength": len(buf.blob)}],
    }
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
    ap.add_argument("--hand", choices=("left", "right"), default="right")
    args = ap.parse_args()

    doc, blob = build_gltf(args.hand)
    write_glb(doc, blob, args.output)

    print(
        f"wrote {args.output} ({doc['accessors'][1]['count']} verts, "
        f"{doc['accessors'][0]['count'] // 3} tris, "
        f"{len(doc['skins'][0]['joints'])} joints, {args.hand} hand)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
