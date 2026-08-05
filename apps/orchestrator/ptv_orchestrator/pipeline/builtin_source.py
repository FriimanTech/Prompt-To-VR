"""Resolve the SOURCE of a built-in mechanic for the code-edit path.

When the MechanicPlan classifies a mechanic as ``builtin+config_code_edit``
it names a ``base_builtin_id``. The code generation agent has no filesystem
access to the engine tree, so the orchestrator reads that built-in's source
here and injects it as a "copy-and-modify" starting point.

Each built-in lives in its OWN module directory under
``engine/runtime/src/mechanics/builtin/<module>/`` and may span several files.
We inject exactly the files of the requested mechanic's module (plus the
small shared headers those files include), not the whole built-in catalog --
which both keeps the agent focused and stops the payload blowing past the
size cap now that these modules contain real logic.

Every built-in is a plain ``IMechanic`` using only the public API the agent
itself is allowed to use (``InputSnapshot``, ``World``, ``MechanicContext``),
so the injected source is directly editable. Copying it produces a NEW
generated mechanic; the built-in is never replaced.
"""

from __future__ import annotations

from pathlib import Path

_BUILTIN_ROOT_REL = "engine/runtime/src/mechanics/builtin"

# Mechanic id -> module directory under _BUILTIN_ROOT_REL. Aliases map to the
# same module as their canonical id.
_MODULE_FOR_ID = {
    "builtin.locomotion.teleport": "teleport",
    "builtin.teleport": "teleport",
    "builtin.locomotion.slide.head_relative": "slide",
    "builtin.locomotion.slide.controller_relative": "slide",
    "builtin.locomotion.snap_turn": "snap_turn",
    "builtin.snap_turn": "snap_turn",
    "builtin.grab": "grab",
    "builtin.throw": "throw",
    "builtin.recoil": "recoil",
}

# Small headers shared across modules. Injected alongside a module's own files
# because the module's code references them directly.
_SHARED_HEADERS = ("BuiltinMechanicBase.h", "StickFilter.h")

# The throw module includes grab's release header; give the agent both so the
# handshake is legible rather than a dangling include.
_EXTRA_MODULE_FILES = {
    "throw": ("grab/GrabRelease.h",),
}

_MAX_SOURCE_CHARS = 24_000


def module_for_builtin(base_builtin_id: str) -> str | None:
    """Module directory implementing ``base_builtin_id``, or None."""
    return _MODULE_FOR_ID.get(base_builtin_id)


def editable_builtins_note() -> str:
    """Note for the planner about which built-ins are code-editable."""
    return (
        "- Any catalog built-in id may be used as `base_builtin_id` for "
        "`builtin+config_code_edit`. The full C++ source of that built-in's "
        "module is injected into the code agent.\n"
        "- Every built-in is an ordinary IMechanic written against the same "
        "public API generated code uses (InputSnapshot + World + "
        "MechanicContext), so its source can be copied and edited directly. "
        "The edited copy is registered under a NEW generated id; the built-in "
        "itself is never modified or replaced."
    )


def read_builtin_sources(repo_root: Path, base_builtin_id: str) -> dict[str, str]:
    """Return ``{label: contents}`` source blocks for ``base_builtin_id``."""
    module = module_for_builtin(base_builtin_id)
    if module is None:
        return {}

    root = repo_root / _BUILTIN_ROOT_REL
    paths: list[Path] = []

    module_dir = root / module
    if module_dir.is_dir():
        paths.extend(sorted(p for p in module_dir.iterdir() if p.is_file()))
    for rel in _EXTRA_MODULE_FILES.get(module, ()):
        paths.append(root / rel)
    for name in _SHARED_HEADERS:
        paths.append(root / name)

    out: dict[str, str] = {}
    budget = _MAX_SOURCE_CHARS
    for path in paths:
        try:
            text = path.read_text(encoding="utf-8")
        except OSError:
            continue
        if not text.strip():
            continue
        if len(text) > budget:
            text = text[:budget] + "\n// ... [truncated] ...\n"
        budget -= len(text)
        try:
            label = str(path.relative_to(root)).replace("\\", "/")
        except ValueError:
            label = path.name
        out[f"{label} (built-in source for {base_builtin_id})"] = text
        if budget <= 0:
            break

    return out


def format_builtin_sources_block(sources: dict[str, str]) -> str:
    """Render source blocks for embedding in the code agent's user message."""
    if not sources:
        return "(no base built-in source available)"
    parts: list[str] = []
    for label, contents in sources.items():
        parts.append(f"----- {label} -----\n{contents}")
    return "\n\n".join(parts)
