"""How big each base-colour texture should be on the headset.

Pure functions (no I/O) so the asset_optimization stage and the tests share
one policy:

* ``needed_size`` -- the texel density an asset actually needs, from its size
  in metres and the CLOSEST distance the player can view it from (a held
  pistol at 0.4 m, a crate beside the path at 1 m, a skyline tower nobody can
  approach at 40 m). Quest 3 renders ~1760 px over a ~96 degree vertical
  field, i.e. roughly 790 px per metre at 1 m; a Tripo texture is one atlas
  for the whole surface, of which a visible side spans ~40 %.
* ``size_cap`` -- the importance ceiling (held / hero 2048, standard 1024,
  background 512, projectile 256), raised one class when the player can stand
  right next to a big object, never above the source image.
* ``fit_budget`` -- when the whole set is over the texture memory budget,
  step down the textures that lose the least visible detail first (farthest
  and least important), never below a floor and never to solid colour.

Sizes are powers of two; GPU bytes assume ASTC with a full mip chain.
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field
from typing import Optional

#: Pixels per metre at 1 m on Quest 3 (1760 px / (2 * tan(48 deg))).
PX_PER_M_AT_1M = 792.0
#: Share of a texture atlas a single visible side of the object covers.
VISIBLE_ATLAS_SHARE = 0.4
MIN_SIZE = 128
ABSOLUTE_MAX_SIZE = 2048

IMPORTANCE_CAPS = {
    "held": 2048,
    "hero": 2048,
    "standard": 1024,
    "background": 512,
    "projectile": 256,
    "ui": 1024,
}
_ORDER = ["projectile", "background", "standard", "hero", "held"]


def pow2_at_least(n: float) -> int:
    n = max(1.0, float(n))
    return 1 << int(math.ceil(math.log2(n)))


def pow2_at_most(n: float) -> int:
    n = max(1.0, float(n))
    return 1 << int(math.floor(math.log2(n)))


def astc_bytes(size: int, block: int = 6, mips: bool = True) -> int:
    """GPU bytes of a square ASTC texture (16 bytes per block)."""
    total = 0
    s = max(1, int(size))
    while True:
        blocks = math.ceil(s / block) ** 2
        total += blocks * 16
        if not mips or s == 1:
            break
        s = max(1, s // 2)
    return total


def needed_size(size_m: float, closest_view_m: float) -> int:
    """Texture edge (px, power of two) that resolves the object at its
    closest view distance."""
    d = max(0.25, float(closest_view_m))
    px = max(0.05, float(size_m)) * PX_PER_M_AT_1M / d / VISIBLE_ATLAS_SHARE
    return max(MIN_SIZE, min(ABSOLUTE_MAX_SIZE, pow2_at_least(px)))


def size_cap(importance: str, size_m: float, closest_view_m: float,
             source_size: int, max_sizes: Optional[dict[str, int]] = None) -> int:
    caps = dict(IMPORTANCE_CAPS)
    if max_sizes:
        caps.update({k: int(v) for k, v in max_sizes.items() if v})
    cls = importance if importance in caps else "standard"
    cap = caps[cls]
    # A big object the player can walk right up to (a building, a vehicle)
    # deserves the next class up even if its category said "background".
    if cls in _ORDER and closest_view_m <= max(1.0, size_m) and cls != "held":
        nxt = _ORDER[min(len(_ORDER) - 1, _ORDER.index(cls) + 1)]
        cap = max(cap, caps.get(nxt, cap))
    if source_size > 0:
        cap = min(cap, pow2_at_most(source_size))
    return max(MIN_SIZE, cap)


@dataclass
class TextureNeed:
    """One unique image (by content hash) and who uses it."""

    hash: str
    source_size: int
    importance: str = "standard"
    size_m: float = 1.0
    closest_view_m: float = 2.0
    block: int = 6
    users: list[str] = field(default_factory=list)
    chosen: int = 0
    reductions: list[str] = field(default_factory=list)

    @property
    def cap(self) -> int:
        return size_cap(self.importance, self.size_m, self.closest_view_m,
                        self.source_size)

    def bytes_at(self, size: int) -> int:
        return astc_bytes(size, self.block)


def choose_sizes(needs: list[TextureNeed],
                 max_sizes: Optional[dict[str, int]] = None) -> None:
    for n in needs:
        cap = size_cap(n.importance, n.size_m, n.closest_view_m, n.source_size,
                       max_sizes)
        n.chosen = max(MIN_SIZE, min(cap, needed_size(n.size_m, n.closest_view_m)))
        if n.source_size > 0:
            n.chosen = min(n.chosen, pow2_at_most(n.source_size))


def total_bytes(needs: list[TextureNeed]) -> int:
    return sum(n.bytes_at(n.chosen) for n in needs)


def _loss_priority(n: TextureNeed) -> tuple:
    """Lower = cheaper to shrink: far, unimportant, already oversized."""
    rank = _ORDER.index(n.importance) if n.importance in _ORDER else 2
    return (rank, -n.closest_view_m, -n.chosen)


def fit_budget(needs: list[TextureNeed], budget_bytes: int,
               floor: int = 256) -> list[str]:
    """Halve textures, least-visible first, until the set fits. Returns the
    reductions made (human readable); an empty list means it already fit."""
    notes: list[str] = []
    guard = 0
    while total_bytes(needs) > budget_bytes and guard < 4096:
        guard += 1
        candidates = [n for n in needs if n.chosen > floor]
        if not candidates:
            # Below the floor only as a last resort, and never below MIN_SIZE.
            candidates = [n for n in needs if n.chosen > MIN_SIZE]
            if not candidates:
                break
        victim = min(candidates, key=_loss_priority)
        before = victim.chosen
        victim.chosen = max(MIN_SIZE, victim.chosen // 2)
        note = f"{victim.hash[:8]} ({','.join(victim.users)[:60]}) {before}->{victim.chosen}"
        victim.reductions.append(f"{before}->{victim.chosen}")
        notes.append(note)
    return notes
