"""Per-view part masks -> triangle regions on the mesh.

Every mask pixel names a triangle through the id buffer, so a view's mask is
already a set of triangles. Views disagree at the edges and the model is
sometimes simply wrong in one view, so a triangle joins a part's region only
when the views that can SEE it mostly agree:

    score(t) = sum over views of (mask pixels of t / visible pixels of t)
               / number of views in which t is visible

Triangles scoring at least ``threshold`` form the region; the region is then
cut to its largest connected piece (by shared vertex positions), which drops
stray pixels the model painted on a neighbouring part.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Optional, Sequence

import numpy as np

from .views import MeshData, ViewSet
from .vision import ViewMasks


@dataclass
class Region:
    part_id: str
    triangles: np.ndarray          # triangle indices
    score: np.ndarray              # per triangle in ``triangles``
    views_seen: int                # views whose mask contributed
    area: float
    centroid: np.ndarray
    points: np.ndarray             # vertex positions of the region (unique)
    weights: np.ndarray            # per point (area-based)

    @property
    def empty(self) -> bool:
        return self.triangles.size == 0


def _vertex_keys(mesh: MeshData) -> np.ndarray:
    """Position-quantised vertex ids, so UV seams do not split a region."""
    span = float(np.ptp(mesh.positions, axis=0).max()) or 1.0
    q = np.round(mesh.positions / (span * 1e-5)).astype(np.int64)
    _, inv = np.unique(q, axis=0, return_inverse=True)
    return inv.reshape(-1)


class Fuser:
    def __init__(self, vs: ViewSet) -> None:
        self.vs = vs
        self.mesh = vs.mesh
        n = self.mesh.tri_count
        # Visible pixel count per triangle per view.
        self.visible = []
        for v in vs.views:
            ids = v.ids.reshape(-1)
            ids = ids[ids > 0].astype(np.int64) - 1
            self.visible.append(np.bincount(ids, minlength=n))
        # Per-triangle vertex keys (T,3), position-quantised.
        self._tri_keys = _vertex_keys(self.mesh)[self.mesh.indices]

    def _largest_component(self, tris: np.ndarray, area: np.ndarray) -> np.ndarray:
        if tris.size <= 1:
            return tris
        keys = self._tri_keys
        # Vertex key -> region triangles touching it.
        groups = {}
        for t in tris:
            for kk in keys[t]:
                groups.setdefault(int(kk), []).append(int(t))
        seen = np.zeros(self.mesh.tri_count, dtype=bool)
        best, best_area = None, -1.0
        for t0 in tris:
            if seen[t0]:
                continue
            stack, comp = [int(t0)], []
            seen[t0] = True
            while stack:
                t = stack.pop()
                comp.append(t)
                for kk in keys[t]:
                    for nb in groups.get(int(kk), ()):
                        if not seen[nb]:
                            seen[nb] = True
                            stack.append(nb)
            comp_a = float(area[comp].sum())
            if comp_a > best_area:
                best, best_area = comp, comp_a
        return np.array(sorted(best or []), dtype=np.int64)

    def region(self, part_id: str, results: Sequence[ViewMasks], *,
               threshold: float = 0.5, largest_only: bool = True) -> Region:
        n = self.mesh.tri_count
        vote = np.zeros(n)
        seen_in = np.zeros(n)
        contributing = 0
        for r in results:
            vis = self.visible[r.view]
            mask = r.masks.get(part_id)
            if mask is None:
                continue
            contributing += 1
            ids = self.vs.views[r.view].ids[mask]
            ids = ids[ids > 0].astype(np.int64) - 1
            inmask = np.bincount(ids, minlength=n)
            has = vis > 0
            vote[has] += inmask[has] / vis[has]
            seen_in[has] += 1
        # Views where the part was NOT reported still count as "seen" for
        # triangles visible there, but only in views that answered at all;
        # otherwise one view's omission would veto a part it simply missed.
        score = np.where(seen_in > 0, vote / np.maximum(seen_in, 1), 0.0)
        tris = np.flatnonzero(score >= threshold)
        area = self.mesh.areas
        if largest_only and tris.size:
            tris = self._largest_component(tris, area)
        return self._make(part_id, tris, score, contributing)

    def _make(self, part_id: str, tris: np.ndarray, score: np.ndarray,
              contributing: int) -> Region:
        mesh = self.mesh
        if tris.size == 0:
            z = np.zeros((0, 3))
            return Region(part_id, tris, np.zeros(0), contributing, 0.0,
                          np.zeros(3), z, np.zeros(0))
        a = mesh.areas[tris]
        vids = np.unique(mesh.indices[tris].reshape(-1))
        pts = mesh.positions[vids]
        # Per-vertex weight: area of incident region triangles / 3.
        w = np.zeros(mesh.positions.shape[0])
        np.add.at(w, mesh.indices[tris].reshape(-1), np.repeat(a / 3.0, 3))
        weights = w[vids]
        total = float(a.sum())
        centroid = (mesh.centroids[tris] * a[:, None]).sum(0) / max(total, 1e-20)
        return Region(part_id, tris, score[tris], contributing, total, centroid,
                      pts, weights)


def pca(points: np.ndarray, weights: Optional[np.ndarray] = None
        ) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Weighted PCA. Returns (mean, eigenvalues desc, eigenvectors as rows)."""
    if weights is None or weights.sum() <= 0:
        weights = np.ones(len(points))
    w = weights / weights.sum()
    mean = (points * w[:, None]).sum(0)
    d = points - mean
    cov = (d * w[:, None]).T @ d
    vals, vecs = np.linalg.eigh(cov)
    order = np.argsort(vals)[::-1]
    return mean, vals[order], vecs[:, order].T
