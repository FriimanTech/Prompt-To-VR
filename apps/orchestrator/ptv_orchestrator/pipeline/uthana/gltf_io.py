"""Minimal, dependency-free glTF 2.0 (GLB) reader/writer and math.

Enough of the spec to measure, analyse and merge rigged characters:

* GLB container read/write (JSON + BIN chunks, 4-byte alignment);
* accessor decode (all component types, ``normalized``, ``byteStride``,
  sparse) and float accessor append with the required ``min``/``max``;
* node hierarchy and world matrices (TRS or ``matrix``);
* linear blend skinning of bind-pose vertices -- a skinned mesh's own node
  transform is ignored, as the spec requires;
* keyframe sampling (STEP, LINEAR with slerp, CUBICSPLINE) and FK at time t.

Matrices are row-major 4x4 lists (``m[row][col]``). glTF stores matrices
column-major, so ``node.matrix`` and inverse-bind matrices are transposed on
read. Quaternions are ``(x, y, z, w)``, glTF's order.
"""

from __future__ import annotations

import json
import math
import struct
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Optional, Sequence

GLB_MAGIC = 0x46546C67
CHUNK_JSON = 0x4E4F534A
CHUNK_BIN = 0x004E4942

_COMPONENT = {
    5120: ("b", 1, 127.0),
    5121: ("B", 1, 255.0),
    5122: ("h", 2, 32767.0),
    5123: ("H", 2, 65535.0),
    5125: ("I", 4, None),
    5126: ("f", 4, None),
}
_TYPE_SIZE = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT2": 4, "MAT3": 9, "MAT4": 16}

Mat4 = list
Vec3 = tuple


class GltfError(RuntimeError):
    """The file is not something this toolkit can analyse."""


# ---------------------------------------------------------------------------
# Container
# ---------------------------------------------------------------------------


@dataclass
class Glb:
    doc: dict
    bin: bytearray
    path: Optional[Path] = None

    @classmethod
    def load(cls, path: Path | str) -> "Glb":
        path = Path(path)
        data = path.read_bytes()
        if len(data) < 20:
            raise GltfError(f"{path}: too small to be a GLB")
        magic, version, total = struct.unpack_from("<III", data, 0)
        if magic != GLB_MAGIC:
            raise GltfError(f"{path}: not a GLB (bad magic)")
        if version != 2:
            raise GltfError(f"{path}: GLB version {version}, expected 2")
        offset = 12
        doc = None
        binary = bytearray()
        while offset + 8 <= min(total, len(data)):
            length, kind = struct.unpack_from("<II", data, offset)
            chunk = data[offset + 8: offset + 8 + length]
            if kind == CHUNK_JSON:
                doc = json.loads(chunk.decode("utf-8"))
            elif kind == CHUNK_BIN and not binary:
                binary = bytearray(chunk)
            offset += 8 + length
        if doc is None:
            raise GltfError(f"{path}: no JSON chunk")
        return cls(doc=doc, bin=binary, path=path)

    def save(self, path: Path | str) -> None:
        path = Path(path)
        self._pad_bin()
        buffers = self.doc.setdefault("buffers", [{}])
        if not buffers:
            buffers.append({})
        buffers[0]["byteLength"] = len(self.bin)
        buffers[0].pop("uri", None)
        js = json.dumps(self.doc, separators=(",", ":")).encode("utf-8")
        js += b" " * ((4 - len(js) % 4) % 4)
        total = 12 + 8 + len(js) + (8 + len(self.bin) if self.bin else 0)
        out = bytearray(struct.pack("<III", GLB_MAGIC, 2, total))
        out += struct.pack("<II", len(js), CHUNK_JSON) + js
        if self.bin:
            out += struct.pack("<II", len(self.bin), CHUNK_BIN) + self.bin
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(bytes(out))

    def _pad_bin(self) -> None:
        pad = (4 - len(self.bin) % 4) % 4
        self.bin += b"\x00" * pad

    # ------------------------------------------------------------------
    # Accessors
    # ------------------------------------------------------------------
    def accessor_count(self, index: int) -> int:
        return int(self.doc["accessors"][index]["count"])

    def read_accessor(self, index: int) -> list[tuple[float, ...]]:
        """Decode an accessor into a list of float tuples (normalized ints
        are mapped to [0,1] / [-1,1]; plain ints are returned as floats)."""
        acc = self.doc["accessors"][index]
        comps = _TYPE_SIZE[acc["type"]]
        fmt, size, norm_div = _COMPONENT[acc["componentType"]]
        count = int(acc["count"])
        normalized = bool(acc.get("normalized")) and norm_div is not None
        out: list[tuple[float, ...]] = []
        if "bufferView" in acc:
            bv = self.doc["bufferViews"][acc["bufferView"]]
            base = bv.get("byteOffset", 0) + acc.get("byteOffset", 0)
            stride = bv.get("byteStride") or comps * size
            unpack = struct.Struct("<" + fmt * comps).unpack_from
            for i in range(count):
                vals = unpack(self.bin, base + i * stride)
                if normalized:
                    vals = tuple(max(-1.0, v / norm_div) for v in vals)
                out.append(tuple(float(v) for v in vals))
        else:
            out = [tuple(0.0 for _ in range(comps)) for _ in range(count)]
        sparse = acc.get("sparse")
        if sparse:
            idx = sparse["indices"]
            vals = sparse["values"]
            ifmt, isize, _ = _COMPONENT[idx["componentType"]]
            ibv = self.doc["bufferViews"][idx["bufferView"]]
            ibase = ibv.get("byteOffset", 0) + idx.get("byteOffset", 0)
            vbv = self.doc["bufferViews"][vals["bufferView"]]
            vbase = vbv.get("byteOffset", 0) + vals.get("byteOffset", 0)
            vunpack = struct.Struct("<" + fmt * comps).unpack_from
            for k in range(int(sparse["count"])):
                (target,) = struct.unpack_from("<" + ifmt, self.bin, ibase + k * isize)
                v = vunpack(self.bin, vbase + k * comps * size)
                if normalized:
                    v = tuple(max(-1.0, x / norm_div) for x in v)
                out[target] = tuple(float(x) for x in v)
        return out

    def append_float_accessor(
        self, values: Sequence[Sequence[float]], type_: str, *, with_bounds: bool = True
    ) -> int:
        """Append float data as a new bufferView + accessor; return its index."""
        comps = _TYPE_SIZE[type_]
        self._pad_bin()
        offset = len(self.bin)
        packer = struct.Struct("<" + "f" * comps)
        for v in values:
            if len(v) != comps:
                raise GltfError(f"accessor value has {len(v)} components, expected {comps}")
            self.bin += packer.pack(*[float(x) for x in v])
        views = self.doc.setdefault("bufferViews", [])
        views.append({"buffer": 0, "byteOffset": offset, "byteLength": len(values) * comps * 4})
        acc = {
            "bufferView": len(views) - 1,
            "componentType": 5126,
            "count": len(values),
            "type": type_,
        }
        if with_bounds and values:
            acc["min"] = [min(float(v[c]) for v in values) for c in range(comps)]
            acc["max"] = [max(float(v[c]) for v in values) for c in range(comps)]
        accessors = self.doc.setdefault("accessors", [])
        accessors.append(acc)
        if not self.doc.get("buffers"):
            self.doc["buffers"] = [{"byteLength": 0}]
        return len(accessors) - 1

    # ------------------------------------------------------------------
    # Hierarchy
    # ------------------------------------------------------------------
    @property
    def nodes(self) -> list[dict]:
        return self.doc.get("nodes") or []

    def parents(self) -> list[int]:
        parent = [-1] * len(self.nodes)
        for i, node in enumerate(self.nodes):
            for c in node.get("children") or []:
                parent[c] = i
        return parent

    def node_index_by_name(self) -> dict[str, int]:
        out: dict[str, int] = {}
        for i, node in enumerate(self.nodes):
            name = node.get("name")
            if name and name not in out:
                out[name] = i
        return out

    def duplicate_node_names(self) -> list[str]:
        seen: dict[str, int] = {}
        for node in self.nodes:
            name = node.get("name")
            if name:
                seen[name] = seen.get(name, 0) + 1
        return sorted(n for n, k in seen.items() if k > 1)

    def world_matrices(self, local_override: Optional[dict[int, Mat4]] = None) -> list[Mat4]:
        """World matrix of every node. ``local_override`` replaces the local
        matrix of selected nodes (used by FK at time t)."""
        parent = self.parents()
        local = [
            (local_override[i] if local_override and i in local_override else node_local(n))
            for i, n in enumerate(self.nodes)
        ]
        world: list[Optional[Mat4]] = [None] * len(self.nodes)

        def resolve(i: int) -> Mat4:
            if world[i] is not None:
                return world[i]  # type: ignore[return-value]
            chain = []
            j = i
            while j >= 0 and world[j] is None:
                chain.append(j)
                j = parent[j]
                if len(chain) > len(self.nodes):
                    raise GltfError("node hierarchy has a cycle")
            acc = world[j] if j >= 0 else identity()
            for k in reversed(chain):
                acc = mat_mul(acc, local[k])  # type: ignore[arg-type]
                world[k] = acc
            return world[i]  # type: ignore[return-value]

        return [resolve(i) for i in range(len(self.nodes))]

    def skin_joint_set(self) -> set[int]:
        out: set[int] = set()
        for skin in self.doc.get("skins") or []:
            out.update(skin.get("joints") or [])
        return out

    def mesh_nodes(self) -> list[int]:
        return [i for i, n in enumerate(self.nodes) if "mesh" in n]


# ---------------------------------------------------------------------------
# Math
# ---------------------------------------------------------------------------


def identity() -> Mat4:
    return [[1.0 if r == c else 0.0 for c in range(4)] for r in range(4)]


def mat_mul(a: Mat4, b: Mat4) -> Mat4:
    return [[sum(a[r][k] * b[k][c] for k in range(4)) for c in range(4)] for r in range(4)]


def mat_point(m: Mat4, p: Sequence[float]) -> tuple[float, float, float]:
    x, y, z = p[0], p[1], p[2]
    return (
        m[0][0] * x + m[0][1] * y + m[0][2] * z + m[0][3],
        m[1][0] * x + m[1][1] * y + m[1][2] * z + m[1][3],
        m[2][0] * x + m[2][1] * y + m[2][2] * z + m[2][3],
    )


def mat_dir(m: Mat4, v: Sequence[float]) -> tuple[float, float, float]:
    return (
        m[0][0] * v[0] + m[0][1] * v[1] + m[0][2] * v[2],
        m[1][0] * v[0] + m[1][1] * v[1] + m[1][2] * v[2],
        m[2][0] * v[0] + m[2][1] * v[1] + m[2][2] * v[2],
    )


def mat_add_scaled(acc: Mat4, m: Mat4, w: float) -> None:
    for r in range(4):
        row_a, row_m = acc[r], m[r]
        for c in range(4):
            row_a[c] += row_m[c] * w


def zeros() -> Mat4:
    return [[0.0] * 4 for _ in range(4)]


def from_gltf_matrix(values: Sequence[float]) -> Mat4:
    """glTF column-major 16 floats -> row-major 4x4."""
    return [[float(values[c * 4 + r]) for c in range(4)] for r in range(4)]


def quat_to_mat3(q: Sequence[float]) -> list[list[float]]:
    x, y, z, w = q
    n = math.sqrt(x * x + y * y + z * z + w * w) or 1.0
    x, y, z, w = x / n, y / n, z / n, w / n
    return [
        [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
        [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
        [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)],
    ]


def trs_matrix(t: Sequence[float], r: Sequence[float], s: Sequence[float]) -> Mat4:
    rot = quat_to_mat3(r)
    m = identity()
    for i in range(3):
        for j in range(3):
            m[i][j] = rot[i][j] * s[j]
        m[i][3] = float(t[i])
    return m


def node_local(node: dict) -> Mat4:
    if "matrix" in node:
        return from_gltf_matrix(node["matrix"])
    return trs_matrix(
        node.get("translation", (0.0, 0.0, 0.0)),
        node.get("rotation", (0.0, 0.0, 0.0, 1.0)),
        node.get("scale", (1.0, 1.0, 1.0)),
    )


def node_trs(node: dict) -> tuple[list[float], list[float], list[float]]:
    """Node TRS; a ``matrix`` node is decomposed (no skew assumed)."""
    if "matrix" not in node:
        return (
            [float(v) for v in node.get("translation", (0.0, 0.0, 0.0))],
            [float(v) for v in node.get("rotation", (0.0, 0.0, 0.0, 1.0))],
            [float(v) for v in node.get("scale", (1.0, 1.0, 1.0))],
        )
    m = from_gltf_matrix(node["matrix"])
    t = [m[0][3], m[1][3], m[2][3]]
    cols = [[m[r][c] for r in range(3)] for c in range(3)]
    s = [math.sqrt(sum(v * v for v in col)) or 1.0 for col in cols]
    rot = [[m[r][c] / s[c] for c in range(3)] for r in range(3)]
    return t, mat3_to_quat(rot), s


def mat3_to_quat(m: Sequence[Sequence[float]]) -> list[float]:
    tr = m[0][0] + m[1][1] + m[2][2]
    if tr > 0:
        s = math.sqrt(tr + 1.0) * 2
        w = 0.25 * s
        x = (m[2][1] - m[1][2]) / s
        y = (m[0][2] - m[2][0]) / s
        z = (m[1][0] - m[0][1]) / s
    elif m[0][0] > m[1][1] and m[0][0] > m[2][2]:
        s = math.sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]) * 2
        w = (m[2][1] - m[1][2]) / s
        x = 0.25 * s
        y = (m[0][1] + m[1][0]) / s
        z = (m[0][2] + m[2][0]) / s
    elif m[1][1] > m[2][2]:
        s = math.sqrt(1.0 + m[1][1] - m[0][0] - m[2][2]) * 2
        w = (m[0][2] - m[2][0]) / s
        x = (m[0][1] + m[1][0]) / s
        y = 0.25 * s
        z = (m[1][2] + m[2][1]) / s
    else:
        s = math.sqrt(1.0 + m[2][2] - m[0][0] - m[1][1]) * 2
        w = (m[1][0] - m[0][1]) / s
        x = (m[0][2] + m[2][0]) / s
        y = (m[1][2] + m[2][1]) / s
        z = 0.25 * s
    return [x, y, z, w]


def inverse3(m: Sequence[Sequence[float]]) -> list[list[float]]:
    a, b, c = m[0][0], m[0][1], m[0][2]
    d, e, f = m[1][0], m[1][1], m[1][2]
    g, h, i = m[2][0], m[2][1], m[2][2]
    det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g)
    if abs(det) < 1e-20:
        raise GltfError("singular 3x3 matrix")
    inv = [
        [(e * i - f * h), -(b * i - c * h), (b * f - c * e)],
        [-(d * i - f * g), (a * i - c * g), -(a * f - c * d)],
        [(d * h - e * g), -(a * h - b * g), (a * e - b * d)],
    ]
    return [[v / det for v in row] for row in inv]


def linear3(m: Mat4) -> list[list[float]]:
    return [[m[r][c] for c in range(3)] for r in range(3)]


def mul3_vec(m: Sequence[Sequence[float]], v: Sequence[float]) -> tuple[float, float, float]:
    return (
        m[0][0] * v[0] + m[0][1] * v[1] + m[0][2] * v[2],
        m[1][0] * v[0] + m[1][1] * v[1] + m[1][2] * v[2],
        m[2][0] * v[0] + m[2][1] * v[1] + m[2][2] * v[2],
    )


def quat_normalize(q: Sequence[float]) -> tuple[float, float, float, float]:
    n = math.sqrt(sum(v * v for v in q)) or 1.0
    return tuple(v / n for v in q)  # type: ignore[return-value]


def quat_slerp(a: Sequence[float], b: Sequence[float], t: float) -> tuple[float, ...]:
    dot = sum(x * y for x, y in zip(a, b))
    if dot < 0.0:
        b = tuple(-v for v in b)
        dot = -dot
    if dot > 0.9995:
        return quat_normalize(tuple(x + (y - x) * t for x, y in zip(a, b)))
    theta = math.acos(max(-1.0, min(1.0, dot)))
    s = math.sin(theta)
    wa = math.sin((1 - t) * theta) / s
    wb = math.sin(t * theta) / s
    return tuple(wa * x + wb * y for x, y in zip(a, b))


def quat_angle_deg(a: Sequence[float], b: Sequence[float]) -> float:
    d = abs(sum(x * y for x, y in zip(quat_normalize(a), quat_normalize(b))))
    return math.degrees(2.0 * math.acos(min(1.0, d)))


# ---------------------------------------------------------------------------
# Skinning
# ---------------------------------------------------------------------------


def skinned_bind_positions(glb: Glb, max_vertices: Optional[int] = None) -> list[tuple[float, float, float]]:
    """World-space bind-pose vertex positions of every mesh in the file.

    Skinned primitives go through ``sum(w * joint_world * IBM) * v``; a
    skinned mesh node's own transform is ignored (glTF 2.0 section 3.7.3).
    Unskinned meshes use their node's world matrix.
    """
    nodes = glb.nodes
    world = glb.world_matrices()
    skins = glb.doc.get("skins") or []
    out: list[tuple[float, float, float]] = []
    for ni in glb.mesh_nodes():
        node = nodes[ni]
        mesh = glb.doc["meshes"][node["mesh"]]
        skin = skins[node["skin"]] if "skin" in node and node["skin"] < len(skins) else None
        joint_mats: list[Mat4] = []
        if skin is not None:
            ibms = (
                [from_gltf_matrix(v) for v in glb.read_accessor(skin["inverseBindMatrices"])]
                if "inverseBindMatrices" in skin
                else [identity() for _ in skin["joints"]]
            )
            joint_mats = [mat_mul(world[j], ibms[k]) for k, j in enumerate(skin["joints"])]
        for prim in mesh.get("primitives") or []:
            attrs = prim.get("attributes") or {}
            if "POSITION" not in attrs:
                continue
            positions = glb.read_accessor(attrs["POSITION"])
            step = 1
            if max_vertices and len(positions) > max_vertices:
                step = max(1, len(positions) // max_vertices)
            if skin is not None and "JOINTS_0" in attrs and "WEIGHTS_0" in attrs:
                joints = glb.read_accessor(attrs["JOINTS_0"])
                weights = glb.read_accessor(attrs["WEIGHTS_0"])
                for vi in range(0, len(positions), step):
                    m = zeros()
                    total = 0.0
                    for k in range(4):
                        w = weights[vi][k]
                        if w <= 0.0:
                            continue
                        mat_add_scaled(m, joint_mats[int(joints[vi][k])], w)
                        total += w
                    if total <= 0.0:
                        continue
                    if abs(total - 1.0) > 1e-3:
                        for r in range(4):
                            for c in range(4):
                                m[r][c] /= total
                    out.append(mat_point(m, positions[vi]))
            else:
                wm = world[ni]
                for vi in range(0, len(positions), step):
                    out.append(mat_point(wm, positions[vi]))
    return out


def aabb(points: Iterable[Sequence[float]]) -> tuple[list[float], list[float]]:
    lo = [math.inf] * 3
    hi = [-math.inf] * 3
    any_point = False
    for p in points:
        any_point = True
        for c in range(3):
            lo[c] = min(lo[c], p[c])
            hi[c] = max(hi[c], p[c])
    if not any_point:
        raise GltfError("no vertices to bound")
    return lo, hi


def raw_position_bounds(glb: Glb) -> tuple[list[float], list[float]]:
    """AABB of the raw POSITION accessors (what the engine normalises by)."""
    lo = [math.inf] * 3
    hi = [-math.inf] * 3
    found = False
    for ni in glb.mesh_nodes():
        mesh = glb.doc["meshes"][glb.nodes[ni]["mesh"]]
        for prim in mesh.get("primitives") or []:
            idx = (prim.get("attributes") or {}).get("POSITION")
            if idx is None:
                continue
            acc = glb.doc["accessors"][idx]
            mn, mx = acc.get("min"), acc.get("max")
            if not mn or not mx:
                pts = glb.read_accessor(idx)
                mn, mx = aabb(pts)
            for c in range(3):
                lo[c] = min(lo[c], float(mn[c]))
                hi[c] = max(hi[c], float(mx[c]))
            found = True
    if not found:
        raise GltfError("no POSITION accessor")
    return lo, hi


# ---------------------------------------------------------------------------
# Animation sampling + FK
# ---------------------------------------------------------------------------


@dataclass
class Channel:
    node: int
    path: str
    interp: str
    times: list[float]
    values: list[tuple[float, ...]]


def read_animation(glb: Glb, index: int) -> list[Channel]:
    anim = glb.doc["animations"][index]
    out: list[Channel] = []
    for ch in anim.get("channels") or []:
        target = ch.get("target") or {}
        if "node" not in target:
            continue
        smp = anim["samplers"][ch["sampler"]]
        out.append(
            Channel(
                node=int(target["node"]),
                path=str(target.get("path")),
                interp=str(smp.get("interpolation") or "LINEAR"),
                times=[t[0] for t in glb.read_accessor(smp["input"])],
                values=glb.read_accessor(smp["output"]),
            )
        )
    return out


def animation_duration(channels: Sequence[Channel]) -> float:
    return max((c.times[-1] for c in channels if c.times), default=0.0)


def sample_channel(ch: Channel, t: float) -> tuple[float, ...]:
    times = ch.times
    cubic = ch.interp == "CUBICSPLINE"

    def value_at(k: int) -> tuple[float, ...]:
        return ch.values[3 * k + 1] if cubic else ch.values[k]

    if t <= times[0] or len(times) == 1:
        return value_at(0)
    if t >= times[-1]:
        return value_at(len(times) - 1)
    lo, hi = 0, len(times) - 1
    while hi - lo > 1:
        mid = (lo + hi) // 2
        if times[mid] <= t:
            lo = mid
        else:
            hi = mid
    t0, t1 = times[lo], times[hi]
    u = (t - t0) / (t1 - t0) if t1 > t0 else 0.0
    if ch.interp == "STEP":
        return value_at(lo)
    if cubic:
        dt = t1 - t0
        p0, m0 = ch.values[3 * lo + 1], ch.values[3 * lo + 2]
        p1, m1 = ch.values[3 * hi + 1], ch.values[3 * hi]
        u2, u3 = u * u, u * u * u
        h00 = 2 * u3 - 3 * u2 + 1
        h10 = u3 - 2 * u2 + u
        h01 = -2 * u3 + 3 * u2
        h11 = u3 - u2
        v = tuple(h00 * a + h10 * dt * b + h01 * c + h11 * dt * d
                  for a, b, c, d in zip(p0, m0, p1, m1))
        return quat_normalize(v) if ch.path == "rotation" else v
    a, b = value_at(lo), value_at(hi)
    if ch.path == "rotation":
        return quat_slerp(a, b, u)
    return tuple(x + (y - x) * u for x, y in zip(a, b))


def pose_world(glb: Glb, channels: Sequence[Channel], t: float) -> list[Mat4]:
    """World matrices of every node with ``channels`` applied at time t."""
    trs: dict[int, list[list[float]]] = {}
    for ch in channels:
        if ch.path not in ("translation", "rotation", "scale") or not ch.times:
            continue
        if ch.node not in trs:
            trs[ch.node] = [list(x) for x in node_trs(glb.nodes[ch.node])]
        slot = {"translation": 0, "rotation": 1, "scale": 2}[ch.path]
        trs[ch.node][slot] = list(sample_channel(ch, t))
    override = {i: trs_matrix(*v) for i, v in trs.items()}
    return glb.world_matrices(override)
