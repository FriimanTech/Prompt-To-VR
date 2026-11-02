"""Judge Quest screenshots of the texture gallery (debug.ptv.verify_textures).

The engine shows each textured asset in front of the player in turn (see
engine/runtime/src/core/TextureGallery.h); quest_runtime_verify takes a Quest
screenshot of each and one of the empty background first. Here:

* the object is whatever differs from the background shot,
* its colours are measured in CIELAB -- the share of near-white pixels, of
  magenta missing-texture pixels, and the mean chroma / hue,
* the same numbers come from an offline render of the shipped LOD0
  (``ptv_glb_views``) as the reference.

Lighting differs (scene lights + ACES on the headset, a plain Lambert
offline), so the checks are about COLOUR SURVIVING, not pixel equality: a
colourful asset must still be colourful, must not have turned white, and
must show no magenta. Automated checks are backed by the saved screenshots
for a human (or the agent) to look at.
"""

from __future__ import annotations

from pathlib import Path
from typing import Any, Optional

import numpy as np

#: Pixel change from the background (sum of RGB deltas) that marks the object.
OBJECT_DELTA = 45
#: The object must cover at least this share of the screenshot to be judged.
MIN_OBJECT_SHARE = 0.004
MAX_MAGENTA = 0.01
#: Extra near-white surface allowed over the reference.
MAX_WHITE_GAIN = 0.30
#: A reference this colourful (mean chroma) must keep this share of it.
COLOURFUL_CHROMA = 10.0
MIN_CHROMA_KEPT = 0.30


def _srgb_to_lab(rgb: np.ndarray) -> np.ndarray:
    c = rgb.astype(np.float32) / 255.0
    lin = np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)
    m = np.array([[0.4124, 0.3576, 0.1805],
                  [0.2126, 0.7152, 0.0722],
                  [0.0193, 0.1192, 0.9505]], dtype=np.float32)
    xyz = lin @ m.T
    xyz /= np.array([0.95047, 1.0, 1.08883], dtype=np.float32)
    f = np.where(xyz > 0.008856, np.cbrt(xyz), 7.787 * xyz + 16.0 / 116.0)
    L = 116.0 * f[..., 1] - 16.0
    a = 500.0 * (f[..., 0] - f[..., 1])
    b = 200.0 * (f[..., 1] - f[..., 2])
    return np.stack([L, a, b], axis=-1)


def colour_stats(pixels: np.ndarray) -> dict[str, float]:
    """Stats of an (N, 3) uint8 RGB pixel array."""
    if pixels.size == 0:
        return {"pixels": 0, "white": 0.0, "magenta": 0.0, "chroma": 0.0,
                "a": 0.0, "b": 0.0, "L": 0.0}
    lab = _srgb_to_lab(pixels)
    chroma = np.sqrt(lab[:, 1] ** 2 + lab[:, 2] ** 2)
    white = float(np.mean((lab[:, 0] > 92.0) & (chroma < 6.0)))
    p = pixels.astype(np.int32)
    magenta = float(np.mean((p[:, 0] > 200) & (p[:, 1] < 70) & (p[:, 2] > 200)))
    return {"pixels": int(pixels.shape[0]), "white": white, "magenta": magenta,
            "chroma": float(np.mean(chroma)), "a": float(np.mean(lab[:, 1])),
            "b": float(np.mean(lab[:, 2])), "L": float(np.mean(lab[:, 0]))}


def load_rgb(path: Path) -> Optional[np.ndarray]:
    try:
        from PIL import Image
    except ImportError:  # pragma: no cover - Pillow ships with the venv
        return None
    try:
        with Image.open(path) as im:
            return np.asarray(im.convert("RGB"))
    except OSError:
        return None


def screenshot_object_stats(shot: np.ndarray, background: np.ndarray) -> dict[str, float]:
    """Colours of whatever differs from the background screenshot."""
    if shot.shape != background.shape:
        h = min(shot.shape[0], background.shape[0])
        w = min(shot.shape[1], background.shape[1])
        shot, background = shot[:h, :w], background[:h, :w]
    delta = np.abs(shot.astype(np.int32) - background.astype(np.int32)).sum(axis=-1)
    mask = delta > OBJECT_DELTA
    stats = colour_stats(shot[mask])
    stats["share"] = float(np.mean(mask))
    return stats


def reference_stats(png: Path, ids: Path, size: int) -> dict[str, float]:
    """Colours of the object in a ptv_glb_views render (ids > 0)."""
    rgb = load_rgb(png)
    if rgb is None or not ids.exists():
        return {}
    tri = np.fromfile(ids, dtype=np.uint32)
    if tri.size != size * size:
        return {}
    mask = tri.reshape(size, size) > 0
    return colour_stats(rgb[mask])


def judge(shot: dict[str, float], ref: dict[str, float], textured: int,
          missing: int) -> tuple[bool, list[str]]:
    """Whether one asset's screenshot shows its colours. Returns (ok, notes)."""
    notes: list[str] = []
    if missing > 0:
        notes.append(f"{missing} material(s) reported a missing texture")
    if shot.get("share", 0.0) < MIN_OBJECT_SHARE:
        notes.append(f"asset barely visible ({shot.get('share', 0.0):.2%} of the view)")
        return False, notes
    if shot.get("magenta", 0.0) > MAX_MAGENTA:
        notes.append(f"{shot['magenta']:.1%} magenta (missing-texture) pixels")
    if ref:
        gain = shot.get("white", 0.0) - ref.get("white", 0.0)
        if gain > MAX_WHITE_GAIN:
            notes.append(f"{gain:.0%} more near-white surface than the reference")
        if (textured > 0 and ref.get("chroma", 0.0) >= COLOURFUL_CHROMA
                and shot.get("chroma", 0.0) < MIN_CHROMA_KEPT * ref["chroma"]):
            notes.append(f"colour lost: chroma {shot.get('chroma', 0):.1f} vs reference "
                         f"{ref['chroma']:.1f}")
    return not notes, notes


def parse_gallery_lines(text: str) -> list[dict[str, Any]]:
    """``texgallery: show i/n <path> textured=.. missing=.. tris=.. skinned=..``."""
    out = []
    for line in text.splitlines():
        if "texgallery: show" not in line:
            continue
        body = line.split("texgallery: show", 1)[1].split()
        if len(body) < 2:
            continue
        idx, _, total = body[0].partition("/")
        entry: dict[str, Any] = {"index": int(idx) if idx.isdigit() else len(out) + 1,
                                 "total": int(total) if total.isdigit() else 0,
                                 "path": body[1]}
        for kv in body[2:]:
            k, _, v = kv.partition("=")
            if v.lstrip("-").isdigit():
                entry[k] = int(v)
        out.append(entry)
    return out
