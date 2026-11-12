"""Hands with five fingers: one check, run on the reference image, on Tripo's
multiview renders and on the finished mesh.

The shooting range's player avatar came out with malformed hands. The
reference image was 1024 px in a T-pose, so each hand was ~60 px wide at the
edge of the frame, in fingerless gloves, fingers pressed together; Tripo had
nothing to reconstruct five fingers from, and the rig then posed blobs.

Every check asks the vision model the same per-hand questions -- visible,
open, how many separate fingers, what covers them -- and scores the answer
the same way, so attempts are comparable whichever stage produced them. The
caller regenerates up to ``hand_qa_max_retries`` times with the critique and
then keeps the best-scoring attempt with a loud warning: a character with
imperfect hands is better than no character.
"""

from __future__ import annotations

import io
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any, Optional, Sequence

import numpy as np

OK_COVERINGS = {"bare", "full_finger_glove"}

HAND_SCHEMA: dict[str, Any] = {
    "type": "object",
    "properties": {
        "hands": {
            "type": "array",
            "items": {
                "type": "object",
                "properties": {
                    "which": {"type": "string"},
                    "visible": {"type": "boolean"},
                    "open": {"type": "boolean"},
                    "finger_count": {"type": "integer"},
                    "fingers_separated": {"type": "boolean"},
                    "covering": {"type": "string", "enum": [
                        "bare", "full_finger_glove", "fingerless_glove", "mitten",
                        "fused_blob", "other"]},
                },
                "required": ["which", "visible", "open", "finger_count",
                             "fingers_separated", "covering"],
            },
        },
        "critique": {"type": "string"},
    },
    "required": ["hands", "critique"],
}

_QUESTIONS = (
    "For EACH hand report: which (as labelled), visible (fully in view, not hidden, "
    "not cropped), open (fingers extended, not a fist or a claw), finger_count (the "
    "distinct fingers you can count, thumb included), fingers_separated (visible gaps "
    "between the fingers, not one fused paddle), covering (bare, full_finger_glove, "
    "fingerless_glove, mitten, fused_blob, other). Then critique: one or two "
    "sentences saying exactly what to change so both hands show five open, separated "
    "fingers (empty when both are already fine)."
)

IMAGE_PROMPT = (
    "This is a character reference image that a 3D generator will turn into a mesh. "
    "The character's hands must hold objects in VR, so each hand needs five clearly "
    "separated fingers. Look ONLY at the character's two hands (label them 'left' and "
    "'right' as they appear in the image). " + _QUESTIONS
)

VIEWS_PROMPT = (
    "These are four views (front, left, back, right) of ONE character, generated as "
    "the input for 3D reconstruction. Its hands must hold objects in VR. Judge the "
    "character's two hands ('left' and 'right' of the character) across all four "
    "views. " + _QUESTIONS
)

MESH_PROMPT = (
    "These are close-up renders of a 3D character mesh's hands: several angles of "
    "hand A, then several of hand B. The mesh will be rigged and its fingers posed "
    "to hold objects, so each hand needs five distinct finger shapes in the "
    "GEOMETRY. Judge the shapes, not the texture. Count conservatively: fingers that "
    "touch along their length with no gap count as ONE, a stub shorter than a "
    "knuckle is not a finger, and fingers_separated is true only when a gap between "
    "EVERY pair of neighbouring fingers shows in at least one view. Label the hands "
    "'A' and 'B'. " + _QUESTIONS
)


@dataclass
class HandReport:
    source: str                       # image | multiview | mesh
    score: float = 0.0
    passed: bool = False
    critique: str = ""
    hands: list[dict] = field(default_factory=list)
    error: str = ""

    def as_dict(self) -> dict:
        return asdict(self)


def hand_score(h: dict) -> float:
    """0..1 for one hand."""
    if not h.get("visible"):
        return 0.0
    s = 0.2
    if h.get("open"):
        s += 0.2
    n = int(h.get("finger_count") or 0)
    s += 0.3 if n == 5 else (0.1 if n in (4, 6) else 0.0)
    if h.get("fingers_separated"):
        s += 0.2
    if h.get("covering") in OK_COVERINGS:
        s += 0.1
    return round(s, 3)


def hand_ok(h: dict) -> bool:
    return bool(h.get("visible") and h.get("open") and int(h.get("finger_count") or 0) == 5
                and h.get("fingers_separated") and h.get("covering") in OK_COVERINGS)


def report_from(answer: dict, source: str) -> HandReport:
    hands = [h for h in (answer.get("hands") or []) if isinstance(h, dict)][:2]
    scores = [hand_score(h) for h in hands] + [0.0] * (2 - len(hands))
    score = round(0.5 * min(scores) + 0.25 * sum(scores), 3)
    return HandReport(source=source, score=score,
                      passed=len(hands) == 2 and all(hand_ok(h) for h in hands),
                      critique=str(answer.get("critique") or "")[:600], hands=hands)


def _encode(path: Path, max_side: int = 2048) -> tuple[bytes, str]:
    """An image under the vision request's 20 MB inline cap: a 4K PNG is
    resized and sent as JPEG."""
    from PIL import Image

    raw = path.read_bytes()
    with Image.open(io.BytesIO(raw)) as im:
        if max(im.size) <= max_side and len(raw) < 6_000_000:
            return raw, "image/png" if path.suffix.lower() == ".png" else "image/jpeg"
        im = im.convert("RGB")
        im.thumbnail((max_side, max_side))
        buf = io.BytesIO()
        im.save(buf, format="JPEG", quality=92)
        return buf.getvalue(), "image/jpeg"


def _ask(vision: Any, prompt: str, images: Sequence[tuple[bytes, str, str]],
         source: str) -> HandReport:
    from ptv_providers.gemini_vision_client import VisionImage

    try:
        answer = vision.analyze(
            prompt=prompt, schema=HAND_SCHEMA, thinking_level="low",
            images=[VisionImage(data=d, mime_type=m, caption=c) for d, m, c in images])
    except Exception as exc:  # noqa: BLE001 - the caller decides what a failed check means
        return HandReport(source=source, error=f"{type(exc).__name__}: {exc}"[:400])
    return report_from(answer, source)


def check_image(vision: Any, path: Path) -> HandReport:
    data, mime = _encode(path)
    return _ask(vision, IMAGE_PROMPT, [(data, mime, "")], "image")


def check_views(vision: Any, views: dict[str, Path]) -> HandReport:
    images = []
    for name in ("front", "left", "back", "right"):
        p = views.get(name)
        if p is not None and p.is_file():
            data, mime = _encode(p, 1024)
            images.append((data, mime, f"{name} view"))
    return _ask(vision, VIEWS_PROMPT, images, "multiview")


# --- Mesh -------------------------------------------------------------------

@dataclass
class HandSite:
    center: np.ndarray
    radius: float


def locate_hands(positions: np.ndarray) -> Optional[tuple[list[HandSite], np.ndarray]]:
    """Both hands of a T-posed (Y-up) character: the two ends of the arm span
    at shoulder height. ``(sites, arm_axis)``, or None when the arms are not
    out (not a T-pose) or the mesh is degenerate."""
    if positions.ndim != 2 or len(positions) < 100:
        return None
    lo, hi = positions.min(0), positions.max(0)
    h = float(hi[1] - lo[1])
    if h <= 1e-6:
        return None
    band = positions[(positions[:, 1] > lo[1] + 0.5 * h) & (positions[:, 1] < lo[1] + 0.97 * h)]
    if len(band) < 50:
        return None
    ext = band.max(0) - band.min(0)
    axis = 0 if ext[0] >= ext[2] else 2
    if ext[axis] < 0.6 * h:
        return None
    arm = np.zeros(3)
    arm[axis] = 1.0
    sites = []
    for sign in (1.0, -1.0):
        a = band[:, axis] * sign
        tip = float(a.max())
        sel = band[a > tip - 0.11 * h]
        c = sel.mean(0)
        c[axis] = sign * (tip - 0.055 * h)
        sites.append(HandSite(center=c, radius=0.075 * h))
    return sites, arm


#: Views of ptv_glb_views' set worth showing per hand: front, back, above,
#: and a three-quarter (the end-on views look along the forearm).
_MESH_VIEWS = (0, 1, 2, 6)


def check_mesh(vision: Any, tool: Path, glb: Path, work_dir: Path,
               size: int = 512) -> HandReport:
    from .sockets.views import load_views, render_views

    try:
        whole = work_dir / "whole"
        render_views(tool, glb, whole, 256)
        found = locate_hands(load_views(whole).mesh.positions)
    except Exception as exc:  # noqa: BLE001
        return HandReport(source="mesh", error=f"render failed: {exc}"[:400])
    if found is None:
        return HandReport(source="mesh", critique="the mesh is not in a T-pose with both "
                          "arms out; its hands cannot be located",
                          error="hands not located")
    sites, arm = found
    images: list[tuple[bytes, str, str]] = []
    for label, site in zip(("A", "B"), sites):
        out = work_dir / f"hand_{label.lower()}"
        try:
            render_views(tool, glb, out, size, up=np.array([0.0, 1.0, 0.0]), forward=arm,
                         focus=site.center, focus_radius=site.radius)
            vs = load_views(out)
        except Exception as exc:  # noqa: BLE001
            return HandReport(source="mesh", error=f"close-up render failed: {exc}"[:400])
        for k in _MESH_VIEWS:
            if k >= len(vs.views):
                continue
            v = vs.views[k]
            if float((v.ids > 0).mean()) < 0.02:
                continue  # nothing of the mesh in this view
            images.append((v.image_path.read_bytes(), "image/png", f"hand {label}, {v.name}"))
    if not images:
        return HandReport(source="mesh", error="no hand pixels in any close-up",
                          critique="the hands are missing from the mesh")
    return _ask(vision, MESH_PROMPT, images, "mesh")


def best(attempts: Sequence[HandReport]) -> int:
    """Index of the attempt to keep: the first that passed, else the highest
    score (the earliest on a tie)."""
    for i, a in enumerate(attempts):
        if a.passed:
            return i
    scores = [a.score for a in attempts]
    return int(np.argmax(scores)) if scores else 0


def retry_clause(critique: str) -> str:
    """Appended to the next reference-image prompt."""
    text = critique.strip() or "the hands did not show five separated fingers"
    return ("HANDS -- the previous image failed a hand check: " + text + " This time both "
            "hands must be fully in frame, open with palms facing forward, all five "
            "fingers extended and spread apart with visible gaps between them, bare or in "
            "full-finger gloves.")
