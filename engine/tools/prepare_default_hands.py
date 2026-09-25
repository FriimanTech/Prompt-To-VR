#!/usr/bin/env python3
"""Turn ``Default_Models/default_rigged_hand_model.glb`` into the engine's
default player hands: ``default_hand_left.glb`` and ``default_hand_right.glb``.

The source is one rigged hand (a Sketchfab export: ``Bone.0xx`` joint names,
an ``Armature`` node scaled by 100 and rotated -90 degrees about X). The
engine needs a pair, and it needs joints it can find without knowing this
particular file. So this script:

1. Finds the wrist (the joint with the most multi-joint chains hanging off
   it), the forearm above it, and the five finger chains below it. The thumb
   is the chain rooted nearest the wrist; the rest are ordered outward from
   the thumb.
2. Measures which hand it is from the geometry -- fingers, knuckle line and
   the direction the rest pose curls toward -- rather than trusting a name.
3. Renames every joint to the canonical BodySlots vocabulary
   (``hand_r``, ``lowerarm_r``, ``index_01_r`` .. ``index_04_r``), so
   ``ISkeleton::joint_for_slot`` and ``HandRig`` resolve it like any rig.
4. Writes that hand as-is, and a mirror image for the other side. Mirroring
   conjugates EVERY node transform by a reflection across X, reflects vertex
   positions, normals and tangents (flipping tangent handedness), mirrors the
   inverse bind matrices and the animation keys, and reverses triangle
   winding so back-face culling still sees the outside of the hand.

Orientation relative to the controller is NOT baked in: the runtime measures
each hand's palm frame with HandRig and maps it onto the OpenXR grip frame,
which works for this asset and for any finger-rigged avatar alike.

Pure standard library, so it runs wherever the engine build runs.
"""

from __future__ import annotations

import argparse
import copy
import json
import math
import struct
import sys
from pathlib import Path

GLB_MAGIC = 0x46546C67
CHUNK_JSON = 0x4E4F534A
CHUNK_BIN = 0x004E4942

FINGERS = ["thumb", "index", "middle", "ring", "pinky"]

COMPONENT_FORMAT = {5120: "b", 5121: "B", 5122: "h", 5123: "H", 5125: "I", 5126: "f"}
TYPE_COUNT = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}


# --------------------------------------------------------------------------
# GLB I/O
# --------------------------------------------------------------------------

def read_glb(path: Path) -> tuple[dict, bytearray]:
    data = path.read_bytes()
    magic, _version, length = struct.unpack_from("<III", data, 0)
    if magic != GLB_MAGIC:
        raise SystemExit(f"{path} is not a GLB")
    off = 12
    gltf = None
    binary = bytearray()
    while off < length:
        clen, ctype = struct.unpack_from("<II", data, off)
        off += 8
        chunk = data[off:off + clen]
        off += clen
        if ctype == CHUNK_JSON:
            gltf = json.loads(chunk.decode("utf-8"))
        elif ctype == CHUNK_BIN:
            binary = bytearray(chunk)
    if gltf is None:
        raise SystemExit(f"{path} has no JSON chunk")
    return gltf, binary


def write_glb(path: Path, gltf: dict, binary: bytes) -> None:
    js = json.dumps(gltf, separators=(",", ":")).encode("utf-8")
    js += b" " * ((4 - len(js) % 4) % 4)
    bn = bytes(binary) + b"\0" * ((4 - len(binary) % 4) % 4)
    total = 12 + 8 + len(js) + (8 + len(bn) if bn else 0)
    out = bytearray(struct.pack("<III", GLB_MAGIC, 2, total))
    out += struct.pack("<II", len(js), CHUNK_JSON) + js
    if bn:
        out += struct.pack("<II", len(bn), CHUNK_BIN) + bn
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(bytes(out))


class Accessor:
    """Element-wise view onto one accessor inside the BIN chunk."""

    def __init__(self, gltf: dict, binary: bytearray, index: int):
        acc = gltf["accessors"][index]
        if "sparse" in acc:
            raise SystemExit("sparse accessors are not supported")
        view = gltf["bufferViews"][acc["bufferView"]]
        self.acc = acc
        self.binary = binary
        self.fmt = COMPONENT_FORMAT[acc["componentType"]]
        self.ncomp = TYPE_COUNT[acc["type"]]
        self.csize = struct.calcsize(self.fmt)
        self.count = acc["count"]
        self.base = view.get("byteOffset", 0) + acc.get("byteOffset", 0)
        self.stride = view.get("byteStride") or self.csize * self.ncomp

    def get(self, i: int) -> list:
        return list(struct.unpack_from(
            "<" + self.fmt * self.ncomp, self.binary, self.base + i * self.stride))

    def set(self, i: int, values: list) -> None:
        struct.pack_into("<" + self.fmt * self.ncomp, self.binary,
                         self.base + i * self.stride, *values)


# --------------------------------------------------------------------------
# Small matrix helpers (column-major 4x4 as flat lists, like glTF)
# --------------------------------------------------------------------------

def mat_mul(a: list, b: list) -> list:
    out = [0.0] * 16
    for c in range(4):
        for r in range(4):
            out[c * 4 + r] = sum(a[k * 4 + r] * b[c * 4 + k] for k in range(4))
    return out


def trs_matrix(node: dict) -> list:
    if "matrix" in node:
        return list(node["matrix"])
    tx, ty, tz = node.get("translation", [0.0, 0.0, 0.0])
    qx, qy, qz, qw = node.get("rotation", [0.0, 0.0, 0.0, 1.0])
    sx, sy, sz = node.get("scale", [1.0, 1.0, 1.0])
    r = [
        1 - 2 * (qy * qy + qz * qz), 2 * (qx * qy + qz * qw), 2 * (qx * qz - qy * qw),
        2 * (qx * qy - qz * qw), 1 - 2 * (qx * qx + qz * qz), 2 * (qy * qz + qx * qw),
        2 * (qx * qz + qy * qw), 2 * (qy * qz - qx * qw), 1 - 2 * (qx * qx + qy * qy),
    ]
    return [
        r[0] * sx, r[1] * sx, r[2] * sx, 0.0,
        r[3] * sy, r[4] * sy, r[5] * sy, 0.0,
        r[6] * sz, r[7] * sz, r[8] * sz, 0.0,
        tx, ty, tz, 1.0,
    ]


def translation(m: list) -> tuple[float, float, float]:
    return (m[12], m[13], m[14])


def sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0])


def length(a):
    return math.sqrt(dot(a, a))


def normalize(a):
    n = length(a)
    return (a[0] / n, a[1] / n, a[2] / n) if n > 1e-9 else (0.0, 0.0, 0.0)


# --------------------------------------------------------------------------
# Rig analysis
# --------------------------------------------------------------------------

def node_parents(gltf: dict) -> dict[int, int]:
    parent = {}
    for i, n in enumerate(gltf["nodes"]):
        for c in n.get("children", []):
            parent[c] = i
    return parent


def world_matrices(gltf: dict) -> list[list]:
    parent = node_parents(gltf)
    cache: dict[int, list] = {}

    def world(i: int) -> list:
        if i not in cache:
            local = trs_matrix(gltf["nodes"][i])
            cache[i] = mat_mul(world(parent[i]), local) if i in parent else local
        return cache[i]

    return [world(i) for i in range(len(gltf["nodes"]))]


def chain(gltf: dict, start: int) -> list[int]:
    out = []
    node = start
    while node is not None and len(out) < 8:
        out.append(node)
        kids = gltf["nodes"][node].get("children", [])
        node = kids[0] if kids else None
    return out


def analyse(gltf: dict) -> dict:
    """Wrist, forearm and finger chains (node indices), plus handedness."""
    skin = gltf["skins"][0]
    joints = set(skin["joints"])
    parent = node_parents(gltf)
    world = world_matrices(gltf)

    def chains_under(j):
        return [c for c in gltf["nodes"][j].get("children", [])
                if c in joints and len(chain(gltf, c)) >= 3]

    wrist = max(joints, key=lambda j: len(chains_under(j)))
    finger_roots = chains_under(wrist)
    if len(finger_roots) < 5:
        raise SystemExit(
            f"expected five finger chains under the wrist, found {len(finger_roots)}")

    wpos = translation(world[wrist])
    bases = {r: translation(world[r]) for r in finger_roots}
    thumb = min(finger_roots, key=lambda r: length(sub(bases[r], wpos)))
    others = sorted((r for r in finger_roots if r != thumb),
                    key=lambda r: length(sub(bases[r], bases[thumb])))[:4]
    fingers = {"thumb": chain(gltf, thumb)}
    for name, root in zip(FINGERS[1:], others):
        fingers[name] = chain(gltf, root)

    # Handedness from geometry. Right hand, palm down, fingers forward: the
    # thumb is on the left, so cross(fingers, index->pinky) is the palm
    # normal. A rest pose that curls toward the other side is a left hand.
    f = normalize(sub(bases[fingers["middle"][0]], wpos))
    s = sub(bases[fingers["pinky"][0]], bases[fingers["index"][0]])
    s = normalize(sub(s, tuple(f[k] * dot(s, f) for k in range(3))))
    n_right = cross(f, s)
    curl = 0.0
    for name in FINGERS[1:]:
        ch = fingers[name]
        if len(ch) < 3:
            continue
        c = sub(translation(world[ch[2]]), translation(world[ch[0]]))
        c = sub(c, tuple(f[k] * dot(c, f) for k in range(3)))
        curl += dot(c, n_right)
    side = "right" if curl >= 0.0 else "left"

    forearm = parent.get(wrist)
    if forearm not in joints:
        forearm = None
    return {"wrist": wrist, "forearm": forearm, "fingers": fingers, "side": side}


def rename(gltf: dict, rig: dict, suffix: str) -> None:
    nodes = gltf["nodes"]
    nodes[rig["wrist"]]["name"] = f"hand_{suffix}"
    if rig["forearm"] is not None:
        nodes[rig["forearm"]]["name"] = f"lowerarm_{suffix}"
    for finger, ch in rig["fingers"].items():
        for seg, node in enumerate(ch[:4]):
            nodes[node]["name"] = f"{finger}_{seg + 1:02d}_{suffix}"


# --------------------------------------------------------------------------
# Mirroring across X
# --------------------------------------------------------------------------

def mirror_matrix(m: list) -> list:
    """M * m * M with M = diag(-1, 1, 1, 1)."""
    out = list(m)
    for c in range(4):
        for r in range(4):
            sign = (-1.0 if c == 0 else 1.0) * (-1.0 if r == 0 else 1.0)
            out[c * 4 + r] = m[c * 4 + r] * sign
    return out


def mirror_node(node: dict) -> None:
    if "matrix" in node:
        node["matrix"] = mirror_matrix(node["matrix"])
        return
    if "translation" in node:
        t = node["translation"]
        node["translation"] = [-t[0], t[1], t[2]]
    if "rotation" in node:
        x, y, z, w = node["rotation"]
        node["rotation"] = [x, -y, -z, w]


def mirror(gltf: dict, binary: bytearray) -> None:
    for node in gltf["nodes"]:
        mirror_node(node)

    done: set[int] = set()

    def once(index):
        if index in done:
            return None
        done.add(index)
        return Accessor(gltf, binary, index)

    for mesh in gltf.get("meshes", []):
        for prim in mesh["primitives"]:
            attrs = prim["attributes"]
            for key in ("POSITION", "NORMAL"):
                if key in attrs and (a := once(attrs[key])):
                    for i in range(a.count):
                        v = a.get(i)
                        a.set(i, [-v[0], v[1], v[2]])
                    if key == "POSITION" and "min" in a.acc and "max" in a.acc:
                        lo, hi = a.acc["min"], a.acc["max"]
                        a.acc["min"] = [-hi[0], lo[1], lo[2]]
                        a.acc["max"] = [-lo[0], hi[1], hi[2]]
            if "TANGENT" in attrs and (a := once(attrs["TANGENT"])):
                for i in range(a.count):
                    v = a.get(i)
                    a.set(i, [-v[0], v[1], v[2], -v[3]])
            if "indices" in prim and (a := once(prim["indices"])):
                for i in range(0, a.count - 2, 3):
                    i1 = a.get(i + 1)
                    i2 = a.get(i + 2)
                    a.set(i + 1, i2)
                    a.set(i + 2, i1)

    for skin in gltf.get("skins", []):
        if "inverseBindMatrices" in skin and (a := once(skin["inverseBindMatrices"])):
            for i in range(a.count):
                a.set(i, mirror_matrix(a.get(i)))

    for anim in gltf.get("animations", []):
        for ch in anim["channels"]:
            sampler = anim["samplers"][ch["sampler"]]
            path = ch["target"]["path"]
            a = once(sampler["output"])
            if a is None:
                continue
            for i in range(a.count):
                v = a.get(i)
                if path == "translation":
                    a.set(i, [-v[0], v[1], v[2]])
                elif path == "rotation":
                    a.set(i, [v[0], -v[1], -v[2], v[3]])


# --------------------------------------------------------------------------

def build(src: Path, out_dir: Path) -> None:
    gltf, binary = read_glb(src)
    if not gltf.get("skins"):
        raise SystemExit(f"{src} has no skin")
    rig = analyse(gltf)
    native = rig["side"]
    other = "left" if native == "right" else "right"
    print(f"prepare_default_hands: {src.name} is a {native} hand "
          f"({sum(len(c) for c in rig['fingers'].values())} finger joints)")

    for side, mirrored in ((native, False), (other, True)):
        g = copy.deepcopy(gltf)
        b = bytearray(binary)
        if mirrored:
            mirror(g, b)
        rename(g, rig, "r" if side == "right" else "l")
        g.setdefault("asset", {}).setdefault("extras", {})["ptv_hand"] = {
            "side": side, "mirrored_from": native if mirrored else None,
        }
        write_glb(out_dir / f"default_hand_{side}.glb", g, b)
        print(f"  wrote {out_dir / f'default_hand_{side}.glb'}")


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--input", required=True, type=Path)
    ap.add_argument("--out-dir", required=True, type=Path)
    args = ap.parse_args(argv)
    build(args.input, args.out_dir)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
