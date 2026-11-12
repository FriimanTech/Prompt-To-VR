"""Run ``ptv_glb_views`` and load what it wrote.

The renders are only useful together with their triangle-id buffers: a pixel
the vision model marks as "handle" is looked up in ``view_<k>.ids`` and becomes
a triangle of the merged mesh in ``mesh.bin``. Everything is kept in the
MESH's own coordinates (the GLB at source scale), which is the space socket
positions are written in before unit normalisation.
"""

from __future__ import annotations

import json
import subprocess
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

import numpy as np


@dataclass
class ViewData:
    index: int
    name: str
    ortho: bool
    image_path: Path
    size: int
    view: np.ndarray        # 4x4, column-major source -> row-major here
    proj: np.ndarray
    eye: np.ndarray
    forward: np.ndarray
    up: np.ndarray
    ids: np.ndarray         # (size, size) uint32, triangle + 1
    depth: np.ndarray       # (size, size) float32, distance along forward

    def project(self, pts: np.ndarray) -> np.ndarray:
        """Mesh-space points (N,3) -> pixel coordinates (N,2) [x, y]."""
        h = np.c_[pts, np.ones(len(pts))]
        clip = (self.proj @ self.view @ h.T).T
        ndc = clip[:, :3] / clip[:, 3:4]
        x = (ndc[:, 0] * 0.5 + 0.5) * self.size
        y = (1.0 - (ndc[:, 1] * 0.5 + 0.5)) * self.size
        return np.c_[x, y]

    def pixel_ray(self, px: float, py: float) -> tuple[np.ndarray, np.ndarray]:
        """Origin and unit direction of the camera ray through a pixel."""
        inv = np.linalg.inv(self.proj @ self.view)
        nx = (px / self.size) * 2.0 - 1.0
        ny = (1.0 - py / self.size) * 2.0 - 1.0
        a = inv @ np.array([nx, ny, 0.0, 1.0])
        b = inv @ np.array([nx, ny, 1.0, 1.0])
        a = a[:3] / a[3]
        b = b[:3] / b[3]
        d = b - a
        return a, d / np.linalg.norm(d)


@dataclass
class MeshData:
    positions: np.ndarray   # (V,3) float64
    indices: np.ndarray     # (T,3) int64
    centroids: np.ndarray = field(init=False)
    areas: np.ndarray = field(init=False)
    normals: np.ndarray = field(init=False)

    def __post_init__(self) -> None:
        a = self.positions[self.indices[:, 0]]
        b = self.positions[self.indices[:, 1]]
        c = self.positions[self.indices[:, 2]]
        cr = np.cross(b - a, c - a)
        area2 = np.linalg.norm(cr, axis=1)
        self.areas = 0.5 * area2
        self.normals = cr / np.maximum(area2, 1e-20)[:, None]
        self.centroids = (a + b + c) / 3.0

    @property
    def tri_count(self) -> int:
        return int(self.indices.shape[0])


@dataclass
class ViewSet:
    out_dir: Path
    mesh: MeshData
    views: list[ViewData]
    meta: dict

    @property
    def longest(self) -> float:
        return float(self.meta["longest_axis"])

    @property
    def axes(self) -> np.ndarray:
        return np.array(self.meta["axes"], dtype=float)


def _mat(values: list[float]) -> np.ndarray:
    # glm::value_ptr is column-major.
    return np.array(values, dtype=float).reshape(4, 4).T


def render_views(tool: Path, glb: Path, out_dir: Path, size: int,
                 overlay: Optional[Path] = None, timeout_s: float = 600.0,
                 up: Optional[np.ndarray] = None,
                 forward: Optional[np.ndarray] = None,
                 focus: Optional[np.ndarray] = None,
                 focus_radius: float = 0.0) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    cmd = [str(tool), "--input", str(glb), "--out-dir", str(out_dir), "--size", str(size)]
    if overlay is not None:
        cmd += ["--overlay", str(overlay)]
    if up is not None:
        cmd += ["--up", ",".join(f"{float(v):.6f}" for v in up)]
        if forward is not None:
            cmd += ["--forward", ",".join(f"{float(v):.6f}" for v in forward)]
    if focus is not None and focus_radius > 0.0:
        cmd += ["--focus", ",".join(f"{float(v):.6f}" for v in focus),
                "--focus-radius", f"{float(focus_radius):.6f}"]
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout_s)
    if proc.returncode != 0:
        raise RuntimeError((proc.stderr or proc.stdout or "ptv_glb_views failed").strip()[:400])


def load_views(out_dir: Path) -> ViewSet:
    meta = json.loads((out_dir / "cameras.json").read_text(encoding="utf-8"))
    raw = (out_dir / "mesh.bin").read_bytes()
    nv, nt = np.frombuffer(raw[:8], dtype=np.uint32)
    off = 8
    pos = np.frombuffer(raw[off:off + nv * 12], dtype=np.float32).reshape(-1, 3).astype(float)
    off += int(nv) * 12
    idx = np.frombuffer(raw[off:off + nt * 12], dtype=np.uint32).reshape(-1, 3).astype(np.int64)
    mesh = MeshData(positions=pos, indices=idx)
    views: list[ViewData] = []
    for cam in meta["views"]:
        k = int(cam["index"])
        size = int(cam["width"])
        ids = np.fromfile(out_dir / f"view_{k}.ids", dtype=np.uint32).reshape(size, size)
        depth = np.fromfile(out_dir / f"view_{k}.depth", dtype=np.float32).reshape(size, size)
        views.append(ViewData(
            index=k, name=cam["name"], ortho=bool(cam["ortho"]),
            image_path=out_dir / cam["image"], size=size,
            view=_mat(cam["view"]), proj=_mat(cam["proj"]),
            eye=np.array(cam["eye"], float), forward=np.array(cam["forward"], float),
            up=np.array(cam["up"], float), ids=ids, depth=depth))
    return ViewSet(out_dir=out_dir, mesh=mesh, views=views, meta=meta)
