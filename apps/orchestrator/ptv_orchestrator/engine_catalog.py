"""Engine catalog: the single source of truth for what the VR runtime
can already do, plus a registry of mechanics promoted from past games.

The mechanic-planner agent is shown this catalog at runtime so it can
prefer reuse over generation. Any time a generated mechanic compiles
cleanly, ``stage_engine_promotion`` appends an entry to
``engine/promoted/registry.json``; on the next pipeline run that entry
is exposed to the planner as a ``promoted.*`` reusable id, so the
engine "grows" with each game built on it.

Design notes
------------

* Built-in entries below mirror the actual ``PTV_REGISTER_MECHANIC`` calls
  in ``engine/runtime/src/mechanics/BuiltinMechanics.cpp`` plus the
  three locomotion ids implemented inline by ``engine/runtime/src/core/Application.cpp``.
  When you add a new built-in there, add it here too.
* The locomotion ids use the ``builtin.locomotion.*`` namespace introduced
  alongside Meta's terminology (``slide.head_relative`` /
  ``slide.controller_relative`` / ``teleport``). The legacy short ids
  (``builtin.teleport`` / ``builtin.smooth_locomotion`` /
  ``builtin.snap_turn``) are kept as aliases so existing scenes keep
  working.
* Reading the promoted registry never raises: a missing or malformed
  file just yields an empty list. Promotion is best-effort enrichment,
  not a load-bearing dependency.
"""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable

# ---------------------------------------------------------------------------
# Static built-in catalog
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class CatalogEntry:
    """A single mechanic the planner is allowed to pick by id."""

    id: str
    summary: str
    category: str
    params: dict[str, str] = field(default_factory=dict)
    inputs: tuple[str, ...] = ()
    comfort_notes: str = ""
    aliases: tuple[str, ...] = ()

    def to_json(self) -> dict[str, object]:
        return {
            "id": self.id,
            "summary": self.summary,
            "category": self.category,
            "params": dict(self.params),
            "inputs": list(self.inputs),
            "comfort_notes": self.comfort_notes,
            "aliases": list(self.aliases),
        }


_BUILTINS: tuple[CatalogEntry, ...] = (
    # ----- Locomotion (canonical Meta-aligned ids) -----
    CatalogEntry(
        id="builtin.locomotion.teleport",
        summary=(
            "Discrete teleport locomotion: aim a parabolic arc from the "
            "left controller; release the stick to commit. Comfort-safe."
        ),
        category="locomotion",
        params={
            "max_horizontal_m": "float, default 12.0",
            "launch_speed": "float, default 6.0",
        },
        inputs=("left thumbstick magnitude",),
        comfort_notes="Discrete; safe for VR-novice users.",
        aliases=("builtin.teleport",),
    ),
    CatalogEntry(
        id="builtin.locomotion.slide.head_relative",
        summary=(
            "Sliding (continuous) locomotion. Movement direction is the "
            "HMD's forward vector projected onto the floor; turning the "
            "head re-steers smoothly. Meta term: 'sliding, head-relative'."
        ),
        category="locomotion",
        params={
            "max_speed_mps": "float, default 1.5",
            "accel_tau_s": "float, default 0.12",
            "decel_tau_s": "float, default 0.18",
        },
        inputs=("left thumbstick (X=strafe, -Y=forward)",),
        comfort_notes=(
            "Comfort ramp + circular dead zone. Can induce vection on "
            "long traversals; pair with snap turn."
        ),
    ),
    CatalogEntry(
        id="builtin.locomotion.slide.controller_relative",
        summary=(
            "Sliding (continuous) locomotion. Movement direction is the "
            "left controller's pointing vector projected onto the floor; "
            "head yaw is irrelevant. Meta term: 'sliding, controller-relative'."
        ),
        category="locomotion",
        params={
            "max_speed_mps": "float, default 1.5",
            "accel_tau_s": "float, default 0.12",
            "decel_tau_s": "float, default 0.18",
        },
        inputs=("left thumbstick (X=strafe, -Y=forward)",),
        comfort_notes=(
            "Same ramps as head-relative; lets the user move sideways "
            "while looking elsewhere. Common in shooters / combat games."
        ),
    ),
    CatalogEntry(
        id="builtin.locomotion.snap_turn",
        summary=(
            "Right-stick snap rotation, comfort-safe. Always-on while a "
            "locomotion menu binding is present."
        ),
        category="locomotion",
        params={
            "degrees": "float, default 45.0",
            "cooldown_s": "float, default 0.30",
        },
        inputs=("right thumbstick X",),
        comfort_notes="Discrete rotation; safer than smooth turn for novices.",
        aliases=("builtin.snap_turn",),
    ),
    # Legacy generic 'smooth_locomotion' id, kept as a thin alias of the
    # head-relative slide so old scenes don't break.
    CatalogEntry(
        id="builtin.smooth_locomotion",
        summary=(
            "Legacy alias of 'builtin.locomotion.slide.head_relative'. "
            "Prefer the explicit slide.* id in new BuildSpecs."
        ),
        category="locomotion",
        aliases=("builtin.locomotion.slide.head_relative",),
    ),
    # ----- Object interaction -----
    CatalogEntry(
        id="builtin.grab",
        summary="Hand-attach a tagged 'grabbable' object on grip squeeze; release drops it.",
        category="interaction",
        params={"hold_offset_local": "Vec3 (auto if omitted)"},
        inputs=("either grip",),
    ),
    CatalogEntry(
        id="builtin.throw",
        summary="When grab is released, applies an impulse based on the smoothed hand velocity.",
        category="interaction",
        params={"impulse": "float, default 4.0"},
    ),
    CatalogEntry(
        id="builtin.use_interact",
        summary="Generic button-press event emitter for buttons / switches / NPCs.",
        category="interaction",
    ),
    CatalogEntry(
        id="builtin.push_pull",
        summary="Hand-driven push/pull on heavy props with a configured friction model.",
        category="interaction",
    ),
    # ----- World / scripting -----
    CatalogEntry(
        id="builtin.trigger_zone",
        summary="Volume that fires enter/exit events when the player or any tagged entity overlaps it.",
        category="scripting",
    ),
    CatalogEntry(
        id="builtin.spawner",
        summary="Periodically spawns a templated entity; supports max-alive cap.",
        category="scripting",
        params={"interval_s": "float, default 5.0"},
    ),
    CatalogEntry(
        id="builtin.inventory",
        summary="Single-slot inventory marker on an entity (for held / equipped items).",
        category="scripting",
    ),
    CatalogEntry(
        id="builtin.dialogue_trigger",
        summary="Hands off to a dialogue hook by id when activated by the player.",
        category="scripting",
        params={"hook_id": "string"},
    ),
    CatalogEntry(
        id="builtin.health_damage",
        summary="HP/damage component with configurable max HP and on-death event.",
        category="combat",
        params={"max_hp": "float, default 100.0"},
    ),
    CatalogEntry(
        id="builtin.enemy_ai_hook",
        summary="Bridges to a generated IBehaviour by id (enemy_ai_hook -> behaviour_id).",
        category="combat",
        params={"behaviour_id": "string"},
    ),
    CatalogEntry(
        id="builtin.projectile_hook",
        summary="Spawns and integrates simple ballistic projectiles (bullets, arrows, energy beams).",
        category="combat",
        params={
            "speed_mps": "float, default 18.0",
            "lifetime_s": "float, default 2.0",
        },
    ),
    CatalogEntry(
        id="builtin.hud_anchor",
        summary="Head-locked or world-anchored HUD panel; configure follow_head + offset.",
        category="ui",
        params={
            "follow_head": "bool, default true",
            "offset_m": "float, default 1.2",
        },
    ),
)


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------


def builtin_catalog() -> list[dict[str, object]]:
    """JSON-friendly list of built-in mechanic descriptors."""
    return [e.to_json() for e in _BUILTINS]


def builtin_ids() -> list[str]:
    """Just the canonical ids (no aliases) — handy for assertions/tests."""
    return [e.id for e in _BUILTINS]


def all_known_ids() -> set[str]:
    """Canonical ids + aliases. Used by the planner to validate a plan
    references something the engine actually knows about."""
    out: set[str] = set()
    for e in _BUILTINS:
        out.add(e.id)
        out.update(e.aliases)
    return out


def promoted_registry_path(repo_root: Path) -> Path:
    """Where promoted generated mechanics get indexed."""
    return repo_root / "engine" / "promoted" / "registry.json"


def promoted_catalog(repo_root: Path) -> list[dict[str, object]]:
    """Read the promoted registry; tolerate missing/malformed file."""
    path = promoted_registry_path(repo_root)
    if not path.exists():
        return []
    try:
        text = path.read_text(encoding="utf-8")
        data = json.loads(text)
    except (OSError, json.JSONDecodeError):
        return []
    if not isinstance(data, dict):
        return []
    items = data.get("items")
    if not isinstance(items, list):
        return []
    return [it for it in items if isinstance(it, dict) and "id" in it]


def _render_entries(entries: Iterable[dict[str, object]]) -> str:
    lines: list[str] = []
    for e in entries:
        eid = e.get("id", "?")
        summary = e.get("summary", "")
        params = e.get("params") or {}
        inputs = e.get("inputs") or []
        comfort = e.get("comfort_notes") or ""
        aliases = e.get("aliases") or []
        bullet = f"- `{eid}` — {summary}"
        if params:
            param_bits = ", ".join(f"{k}={v}" for k, v in params.items())
            bullet += f"\n    params: {param_bits}"
        if inputs:
            bullet += f"\n    inputs: {', '.join(inputs)}"
        if comfort:
            bullet += f"\n    comfort: {comfort}"
        if aliases:
            bullet += f"\n    aliases: {', '.join(aliases)}"
        lines.append(bullet)
    return "\n".join(lines)


def as_planner_text(repo_root: Path) -> str:
    """Render the catalog (built-in + promoted) as planner-ready prompt text."""
    builtin_lines = _render_entries(builtin_catalog())
    promoted = promoted_catalog(repo_root)
    if promoted:
        promoted_lines = _render_entries(promoted)
        promoted_block = (
            "\n\nPROMOTED FROM PAST GAMES (also reusable; reference by id):\n"
            + promoted_lines
        )
    else:
        promoted_block = (
            "\n\nPROMOTED FROM PAST GAMES: (none yet — the engine will grow as games are built.)"
        )
    return (
        "ENGINE CATALOG — built-in mechanics (reference these by id whenever possible):\n"
        + builtin_lines
        + promoted_block
    )


def full_catalog_text(repo_root: Path) -> str:
    """Backwards-friendly alias for ``as_planner_text``."""
    return as_planner_text(repo_root)
