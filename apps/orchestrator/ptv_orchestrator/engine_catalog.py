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
  in ``engine/runtime/src/mechanics/builtin/<mechanic>/``. Every built-in is
  a real ``IMechanic`` module there -- nothing is implemented inline by
  ``Application.cpp`` any more. When you add a new built-in module, add it
  here too.
* The locomotion ids use the ``builtin.locomotion.*`` namespace introduced
  alongside Meta's terminology (``slide.head_relative`` /
  ``slide.controller_relative`` / ``teleport``). The legacy short ids
  ``builtin.teleport`` and ``builtin.snap_turn`` are kept as aliases so
  existing scenes keep working. ``builtin.smooth_locomotion`` has been
  REMOVED -- use the explicit ``slide.*`` id.
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
            # ``max_distance_m`` is accepted as a synonym of the first one --
            # the hand-authored starter room uses that spelling.
            "max_horizontal_m": "float, default 12.0",
            "launch_speed_mps": "float, default 6.0",
            "hand": "'left' (default) or 'right'",
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
    # ----- Object interaction -----
    CatalogEntry(
        id="builtin.grab",
        summary=(
            "Hand-attach an object on grip squeeze; release drops it. "
            "Binding this mechanic to an entity is what MAKES it grabbable "
            "(it tags the entity), so no separate 'grabbable' tag is needed. "
            "When the asset has a derived 'grip' socket the object snaps so "
            "that socket sits in the hand, which is what stops a weapon from "
            "being held sideways just because the player reached for it from "
            "above."
        ),
        category="interaction",
        params={
            "radius_m": "float, default 0.18 (reach around the hand)",
            "snap_to_grip": (
                "bool, default true. Align the object's 'grip' socket to the "
                "hand. Set false for something with no single correct "
                "orientation (a ball, a rock, a crate)."
            ),
        },
        inputs=("either grip",),
    ),
    CatalogEntry(
        id="builtin.throw",
        summary=(
            "Turns releasing a grabbed object into a throw, scaling the "
            "smoothed hand velocity. Bind it to the object alongside "
            "builtin.grab; an object with grab but no throw simply drops."
        ),
        category="interaction",
        params={"impulse": "float, default 4.0 (throw strength)"},
    ),
    # ----- Feedback / juice -----
    CatalogEntry(
        id="builtin.recoil",
        summary=(
            "Kicks an object backward and pitches its nose up when the bound "
            "control is pressed, then springs it home. Purely visual (it "
            "drives the engine's draw-time transform-offset layer), so it "
            "never fights physics or builtin.grab. Bind it to a weapon "
            "ALONGSIDE the mechanic that actually fires: both read the same "
            "trigger, so they stay in sync with no coupling. This is the "
            "cheapest 'the weapon fired' cue available -- no sockets, no "
            "moving parts and no skinning required."
        ),
        category="interaction",
        params={
            "kick_back_m": "float, default 0.035 (shove along the object's +Z)",
            "kick_up_deg": "float, default 3.0 (muzzle rise)",
            "recover_tau_s": "float, default 0.07 (spring-back time constant)",
            "require_held": (
                "bool, default true (only kick while a hand holds the object)"
            ),
        },
        inputs=("the bound control on whichever hand holds the object",),
    ),
    CatalogEntry(
        id="builtin.hand_pose",
        summary=(
            "Curls the PLAYER'S OWN fingers from the controller: index "
            "follows the trigger, the other three follow the grip, and the "
            "thumb folds when it touches the thumbrest or a face button. "
            "Capacitive touch gives a resting bend, so a finger lying on the "
            "trigger without pulling it looks relaxed instead of pointing. "
            "The engine binds this automatically -- you do NOT need to plan "
            "it, and it is not a mechanic for scene objects. Listed here so "
            "you know the player's hands already animate themselves: do not "
            "generate a mechanic to do it, and never couple a weapon to the "
            "hand (both read the same trigger value and stay in sync on "
            "their own)."
        ),
        category="interaction",
        params={
            "smoothing_tau_s": (
                "float, default 0.045 (finger curl smoothing time constant)"
            ),
            "hold_curl": (
                "float, default 0.85 (how closed the fingers sit while the "
                "hand is holding something)"
            ),
        },
        inputs=("both triggers", "both grips", "trigger/thumbrest touch"),
    ),
    CatalogEntry(
        id="builtin.character_locomotion",
        summary=(
            "Moves an NPC and keeps its animation in agreement. Bind it to "
            "every character that moves; AI code then only writes a desired "
            "velocity into CLocomotionIntent and this does the rest: sweeps "
            "the body so it cannot walk through walls, snaps it to the "
            "floor, turns it to face travel, picks idle/walk/run from the "
            "speed it ACHIEVED, and scales clip playback so the stride "
            "matches the ground covered (which is what stops the feet "
            "skating). Set 'animated': false and the same mechanic slides an "
            "un-animated model instead -- that is the correct answer when a "
            "prompt explicitly asks for no character animation, and it needs "
            "no separate mechanic. NEVER generate a mechanic that plays "
            "locomotion clips or moves a character by writing CTransform; "
            "it will fight this one and get the foot-sliding wrong."
        ),
        category="npc",
        params={
            "walk_speed_mps": "float, default 1.4",
            "run_speed_mps": "float, default 4.0",
            "turn_rate_deg_s": "float, default 540",
            "animated": (
                "bool, default true. false = move the model with no clips "
                "(the 'ducks just slide' case)"
            ),
            "ground_snap": "bool, default true",
            "reference_walk_mps": (
                "float, default 0 = derive from the character's height. Only "
                "set this when a clip's authored stride is unusual."
            ),
            "reference_run_mps": "float, default 0 = derive from height",
        },
        inputs=("CLocomotionIntent written by AI code",),
        comfort_notes=(
            "Not player-facing. Animated characters are the most expensive "
            "thing on a Quest 3, so respect the NpcPlan's max_concurrent."
        ),
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
