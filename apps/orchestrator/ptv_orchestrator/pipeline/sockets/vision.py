"""Ask the vision model which pixels of each render show each part.

One request per view (they run in parallel), each asking for a polygon mask
of every part the item's sockets need plus a few CONTEXT parts the solvers
use to orient them: a handle's axis is pointed toward the barrel or the
blade, a muzzle faces along the barrel, a strap sits on the side opposite
the shield's front face.

Masks come back as polygons. The Gemini docs specify ``[x, y]`` normalised to
0-1000 *inside* ``box_2d`` (itself ``[ymin, xmin, ymax, xmax]``), but models
drift between conventions, so every plausible reading is rasterised and the
one whose pixels best land on the object (the triangle-id buffer says which
pixels do) wins. A part whose polygon fits nowhere falls back to its box
intersected with the object's silhouette.
"""

from __future__ import annotations

import json
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from typing import Any, Callable, Optional, Sequence

import numpy as np
from PIL import Image, ImageDraw

from ptv_schemas import FireSystem, InteractionDesign, ItemInteraction, SocketKind

from .views import ViewData, ViewSet


@dataclass(frozen=True)
class PartRequest:
    part_id: str
    description: str
    #: True for parts a socket is solved on; False for orientation context.
    primary: bool = True


@dataclass
class ViewMasks:
    view: int
    masks: dict[str, np.ndarray] = field(default_factory=dict)
    raw: dict[str, Any] = field(default_factory=dict)
    convention: dict[str, str] = field(default_factory=dict)


#: A callable doing one structured vision call: (prompt, image png bytes,
#: json schema) -> parsed JSON. Injected so tests never touch the network.
VisionFn = Callable[[str, bytes, dict], dict]


_RANGED = {FireSystem.hitscan, FireSystem.projectile, FireSystem.hybrid, FireSystem.beam}


def parts_for(item: ItemInteraction, design: Optional[InteractionDesign] = None
              ) -> list[PartRequest]:
    """The parts to segment for one item: its sockets' parts plus context."""

    out: list[PartRequest] = []
    seen: set[str] = set()

    def add(pid: str, desc: str, primary: bool = True) -> None:
        if pid not in seen:
            seen.add(pid)
            out.append(PartRequest(pid, desc, primary))

    kinds = {s.kind for s in item.sockets}
    for s in item.sockets:
        if s.kind == SocketKind.support_wrap:
            continue  # derived from the primary grip, not segmented
        add(s.name, s.part_description)
    ranged = item.weapon is not None and item.weapon.fire_system in _RANGED
    if ranged or SocketKind.muzzle in kinds:
        add("barrel", "the barrel, slide or emitter tube that shots travel along "
                      "(from the body of the weapon to its muzzle)", False)
        if SocketKind.muzzle not in kinds:
            add("muzzle", "the open end of the barrel where shots leave", False)
    if SocketKind.strike_tip in kinds or SocketKind.strike_base in kinds:
        add("blade", "the whole blade, from the guard to the point", False)
    if SocketKind.strike_face in kinds:
        add("head", "the head of the weapon (the heavy striking end)", False)
    if SocketKind.hand_strap in kinds or SocketKind.forearm_strap in kinds:
        add("front_face", "the decorated / curved FRONT face of the shield that faces "
                          "the enemy (not the back where the straps are)", False)
    if SocketKind.limb_tip_top in kinds or SocketKind.string_rest in kinds:
        add("bowstring", "the bowstring", False)
        add("riser", "the bow's middle section (riser) including the handle", False)
    if SocketKind.projectile_nose in kinds:
        add("front_tip", "the front tip of the projectile (arrowhead / nose / point)",
            False)
    if SocketKind.arrow_nock in kinds:
        add("back_end", "the back end of the arrow with the fletching and nock", False)
    if any(s.kind in (SocketKind.hand_grip, SocketKind.support_grip) for s in item.sockets) \
            and not ranged and SocketKind.strike_tip not in kinds \
            and SocketKind.strike_face not in kinds and SocketKind.hand_strap not in kinds:
        add("working_end", "the working end of the item (the part that is not the "
                           "handle)", False)
    return out


def build_prompt(asset_name: str, summary: str, parts: Sequence[PartRequest],
                 view_name: str) -> str:
    lines = [
        f"This image is a render of a 3D model of: {asset_name}.",
        f"Description: {summary[:400]}" if summary else "",
        f"Camera: {view_name.replace('_', ' ')} view (the model may appear rotated or "
        "upside down; judge parts by their shape, not by screen direction).",
        "Segment each of these parts of the object:",
    ]
    for p in parts:
        lines.append(f"- {p.part_id}: {p.description}")
    lines += [
        "For every part return: part_id; visible (false if the part is hidden or "
        "out of view in THIS image); box_2d as [ymin, xmin, ymax, xmax] normalized "
        "to 0-1000; mask as the part's segmentation mask: a polygon of [x, y] "
        "points normalized to 0-1000 inside box_2d. Be precise: only the pixels "
        "of that part.",
    ]
    return "\n".join(l for l in lines if l)


def response_schema(parts: Sequence[PartRequest]) -> dict:
    return {
        "type": "object",
        "properties": {
            "parts": {
                "type": "array",
                "items": {
                    "type": "object",
                    "properties": {
                        "part_id": {"type": "string", "enum": [p.part_id for p in parts]},
                        "visible": {"type": "boolean"},
                        "box_2d": {"type": "array", "items": {"type": "integer"}},
                        "mask": {"type": "array",
                                 "items": {"type": "array", "items": {"type": "integer"}}},
                    },
                    "required": ["part_id", "visible", "box_2d", "mask"],
                }
            }
        },
        "required": ["parts"],
    }


def _raster(poly: list[tuple[float, float]], size: int) -> np.ndarray:
    img = Image.new("L", (size, size), 0)
    if len(poly) >= 3:
        ImageDraw.Draw(img).polygon(poly, fill=1)
    return np.asarray(img, dtype=bool)


def decode_mask(entry: dict, view: ViewData) -> tuple[np.ndarray, str]:
    """Best pixel mask for one part entry, and the convention it used."""

    size = view.size
    on_mesh = view.ids > 0
    box = entry.get("box_2d") or []
    pts = [p for p in (entry.get("mask") or []) if isinstance(p, list) and len(p) >= 2]
    s = size / 1000.0
    cands: dict[str, list[tuple[float, float]]] = {}
    if len(box) == 4:
        y0, x0, y1, x1 = (float(v) for v in box)
        bw, bh = max(1.0, x1 - x0), max(1.0, y1 - y0)
        if pts:
            cands["box_xy"] = [((x0 + p[0] / 1000.0 * bw) * s, (y0 + p[1] / 1000.0 * bh) * s)
                               for p in pts]
            cands["box_yx"] = [((x0 + p[1] / 1000.0 * bw) * s, (y0 + p[0] / 1000.0 * bh) * s)
                               for p in pts]
    if pts:
        cands["abs_xy"] = [(p[0] * s, p[1] * s) for p in pts]
        cands["abs_yx"] = [(p[1] * s, p[0] * s) for p in pts]
    best, best_name, best_score = None, "", -1.0
    for name, poly in cands.items():
        m = _raster(poly, size)
        area = int(m.sum())
        if area < 4:
            continue
        hit = float((m & on_mesh).sum()) / area
        if len(box) == 4:
            # Decisive: under the reading the model actually used, the
            # polygon's own bounding box IS its box_2d. Wrong readings land
            # somewhere else (often still on the object, so on-mesh alone
            # cannot tell them apart).
            px = np.array([p[0] for p in poly])
            py = np.array([p[1] for p in poly])
            y0, x0, y1, x1 = (float(v) * s for v in box)
            ix = max(0.0, min(px.max(), x1) - max(px.min(), x0))
            iy = max(0.0, min(py.max(), y1) - max(py.min(), y0))
            inter = ix * iy
            union = (np.ptp(px) * np.ptp(py)) + (x1 - x0) * (y1 - y0) - inter
            iou = inter / union if union > 0 else 0.0
            score = iou * (0.6 + 0.4 * hit)
        else:
            score = hit
        if score > best_score:
            best, best_name, best_score = m, name, score
    if best is not None and best_score >= 0.35:
        return best & on_mesh, best_name
    if len(box) == 4:
        y0, x0, y1, x1 = (int(round(float(v) * s)) for v in box)
        m = np.zeros((size, size), dtype=bool)
        m[max(0, y0):min(size, y1), max(0, x0):min(size, x1)] = True
        return m & on_mesh, "box"
    return np.zeros((size, size), dtype=bool), "none"


def segment_views(vs: ViewSet, parts: Sequence[PartRequest], *, asset_name: str,
                  summary: str, vision: VisionFn, max_parallel: int = 6,
                  extra_note: str = "") -> list[ViewMasks]:
    schema = response_schema(parts)

    def one(view: ViewData) -> ViewMasks:
        prompt = build_prompt(asset_name, summary, parts, view.name)
        if extra_note:
            prompt += "\n" + extra_note
        out = ViewMasks(view=view.index)
        try:
            resp = vision(prompt, view.image_path.read_bytes(), schema)
        except Exception as exc:  # noqa: BLE001 - one bad view is not fatal
            out.raw = {"error": str(exc)[:300]}
            return out
        out.raw = resp
        for entry in resp.get("parts") or []:
            pid = entry.get("part_id")
            if pid not in {p.part_id for p in parts} or not entry.get("visible", True):
                continue
            mask, conv = decode_mask(entry, view)
            if mask.sum() >= 3:
                prev = out.masks.get(pid)
                out.masks[pid] = mask if prev is None else (prev | mask)
                out.convention[pid] = conv
        return out

    with ThreadPoolExecutor(max_workers=max(1, max_parallel)) as pool:
        return list(pool.map(one, vs.views))


def dump_raw(results: Sequence[ViewMasks]) -> str:
    return json.dumps([{"view": r.view, "convention": r.convention, "response": r.raw}
                       for r in results], indent=1)
