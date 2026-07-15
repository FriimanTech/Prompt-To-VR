"""Resolve the SOURCE of a built-in mechanic for the code-edit path.

When the MechanicPlan classifies a mechanic as
``builtin+config_code_edit`` it names a ``base_builtin_id``. The code
generation agent has no filesystem access to the engine tree, so the
orchestrator reads that built-in's source here and injects it into the
agent's context as a "copy-and-modify" starting point.

Every registered built-in mechanic has a class in
``BuiltinMechanics.cpp``. A few built-ins (grab / throw / locomotion /
snap turn) implement their real per-frame BEHAVIOR inside the
engine-owned ``Application.cpp`` main loop (they can access the player
rig, physics, and the raw OpenXR poses that a generated ``IMechanic``
cannot). For those, we still hand over the registration class as the
base but add a note telling the agent to re-implement the behavior via
the public ``IMechanic`` + ``InputSnapshot`` + ``World`` API instead of
reaching into engine internals.
"""

from __future__ import annotations

from pathlib import Path

_BUILTIN_CPP_REL = "engine/runtime/src/mechanics/BuiltinMechanics.cpp"

# Built-ins whose real per-frame behavior lives in Application.cpp (engine
# core), not in their BuiltinMechanics.cpp class (which is a thin
# registration + component marker). A code_edit version must reimplement the
# needed behavior through the public mechanic API.
_APP_OWNED_BEHAVIOR = {
    "builtin.grab",
    "builtin.throw",
    "builtin.teleport",
    "builtin.locomotion.teleport",
    "builtin.smooth_locomotion",
    "builtin.locomotion.slide.head_relative",
    "builtin.locomotion.slide.controller_relative",
    "builtin.snap_turn",
    "builtin.locomotion.snap_turn",
}

_MAX_SOURCE_CHARS = 24_000


def editable_builtins_note() -> str:
    """A short note for the planner listing which built-ins are code-editable
    and which have engine-owned behavior."""
    return (
        "- Any catalog built-in id may be used as `base_builtin_id` for "
        "`builtin+config_code_edit`; its registration class source "
        "(BuiltinMechanics.cpp) is injected into the code agent.\n"
        "- These built-ins implement their live behavior in the engine core "
        "(Application.cpp), so a code_edit version must RE-IMPLEMENT the "
        "behavior via IMechanic + InputSnapshot + World (it cannot call "
        "engine internals): grab, throw, teleport/slide locomotion, snap turn.\n"
        "- The other built-ins (trigger_zone, spawner, health_damage, "
        "projectile_hook, hud_anchor, use_interact, push_pull, inventory, "
        "dialogue_trigger, enemy_ai_hook) are thin catalog stubs: code_edit "
        "means authoring the real logic on top of their component/registration "
        "shape."
    )


def read_builtin_sources(repo_root: Path, base_builtin_id: str) -> dict[str, str]:
    """Return ``{label: contents}`` source blocks for ``base_builtin_id``.

    Always includes the full ``BuiltinMechanics.cpp`` (the registration +
    class source for every built-in) so the agent can copy the matching
    class. For engine-owned-behavior built-ins, adds a NOTE block.
    """
    out: dict[str, str] = {}
    src = repo_root / _BUILTIN_CPP_REL
    try:
        text = src.read_text(encoding="utf-8")
    except OSError:
        text = ""
    if text:
        if len(text) > _MAX_SOURCE_CHARS:
            text = text[:_MAX_SOURCE_CHARS] + "\n// ... [truncated] ...\n"
        out["BuiltinMechanics.cpp (base built-in class source)"] = text

    if base_builtin_id in _APP_OWNED_BEHAVIOR:
        out["NOTE (engine-owned behavior)"] = (
            f"The live behavior of '{base_builtin_id}' is implemented in the "
            "engine core (Application.cpp) and is NOT part of the base class "
            "above. Re-implement the behavior you need in your new generated "
            "mechanic using the public IMechanic + InputSnapshot + World API "
            "(read thumbstick/trigger/grip/buttons/poses from the InputSnapshot "
            "passed to on_update). Do NOT include or call any engine-internal "
            "header; register the result under a NEW generated id and do NOT "
            "replace the built-in."
        )

    return out


def format_builtin_sources_block(sources: dict[str, str]) -> str:
    """Render source blocks for embedding in the code agent's user message."""
    if not sources:
        return "(no base built-in source available)"
    parts: list[str] = []
    for label, contents in sources.items():
        parts.append(f"----- {label} -----\n{contents}")
    return "\n\n".join(parts)
