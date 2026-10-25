"""Does every mechanic a compiled scene binds actually exist in the build?

The runtime resolves a binding's ``builtin_id`` (or, failing that, its
``id``) against the factories registered with ``PTV_REGISTER_MECHANIC`` /
``PTV_REGISTER_BEHAVIOUR``. An id nobody registered is logged once at load
and then does nothing -- which is how a generated follow behaviour and a
generated stalker AI shipped in an APK, compiled and linked, and never ran.

This answers the same question offline, from the source the build compiles:
the engine's built-ins under ``engine/runtime/src`` plus this project's
generated code under ``engine/generated/{mechanics,ai,dialogue}/<id>``.
"""

from __future__ import annotations

import re
from pathlib import Path
from typing import Iterable

_REGISTRATION = re.compile(
    r'\bPTV_REGISTER_(MECHANIC|BEHAVIOUR|DIALOGUE_HOOK)\s*\(\s*"([^"]+)"'
)
#: Only these namespaces are checked. ``promoted.*`` ids live in the planner
#: catalog, not as separately registered code, and are left to the planner.
_CHECKED_PREFIXES = ("builtin.", "generated.")

_builtin_cache: dict[Path, dict[str, str]] = {}


def _scan(paths: Iterable[Path]) -> dict[str, str]:
    out: dict[str, str] = {}
    for p in paths:
        try:
            text = p.read_text(encoding="utf-8", errors="ignore")
        except OSError:
            continue
        for kind, ident in _REGISTRATION.findall(text):
            out.setdefault(ident, kind.lower())
    return out


def registered_ids(repo_root: Path, project_id: str) -> dict[str, str]:
    """id -> ``mechanic`` | ``behaviour`` | ``dialogue_hook`` for everything
    this project's build registers. Empty when the engine source is not
    present (a test fixture), in which case callers should skip the check."""

    src = repo_root / "engine" / "runtime" / "src"
    if not src.is_dir():
        return {}
    builtins = _builtin_cache.get(src)
    if builtins is None:
        builtins = _scan(src.rglob("*.cpp"))
        _builtin_cache[src] = builtins
    out = dict(builtins)
    gen = repo_root / "engine" / "generated"
    for kind in ("mechanics", "ai", "dialogue"):
        d = gen / kind / project_id
        if d.is_dir():
            out.update(_scan(d.glob("*.cpp")))
    return out


def binding_target(binding: dict) -> str:
    """The id the runtime resolves: ``builtin_id`` wins over ``id``."""
    return str(binding.get("builtin_id") or binding.get("id") or "")


def is_hosted(ident: str, registered: dict[str, str]) -> bool:
    """True when the runtime will instantiate and tick ``ident``."""
    return registered.get(ident) in ("mechanic", "behaviour")


def unresolved_bindings(
    entities: list[dict], registered: dict[str, str]
) -> list[str]:
    """``"<entity>: <id>"`` for every binding the runtime cannot run."""

    problems: list[str] = []
    for e in entities:
        for b in e.get("mechanic_bindings") or []:
            if not isinstance(b, dict):
                continue
            ident = binding_target(b)
            if not ident.startswith(_CHECKED_PREFIXES):
                continue
            if is_hosted(ident, registered):
                continue
            why = ("is a dialogue hook, which nothing runs as a mechanic"
                   if registered.get(ident) == "dialogue_hook"
                   else "is registered nowhere in the engine or this "
                        "project's generated code")
            problems.append(f"{e.get('id', '?')}: {ident} {why}")
    return problems


def drop_unhosted_npc_ai(
    entities: list[dict], registered: dict[str, str],
    generated_ai_ids: Iterable[str],
) -> tuple[set[str], list[str]]:
    """Remove generated-AI bindings the build will not host from NPCs.

    Returns ``(hosted_ai_ids, notes)``. An NPC whose AI failed to generate
    then gets the built-in behaviour (the population step binds it to
    any NPC without generated AI) instead of standing still."""

    hosted = {i for i in generated_ai_ids if is_hosted(i, registered)}
    notes: list[str] = []
    for e in entities:
        bindings = e.get("mechanic_bindings") or []
        keep = []
        for b in bindings:
            ident = binding_target(b) if isinstance(b, dict) else ""
            if ident in set(generated_ai_ids) and ident not in hosted:
                notes.append(
                    f"{e.get('id', '?')}: generated AI {ident} is not in the "
                    "build; using the built-in NPC behaviour instead")
                continue
            keep.append(b)
        if len(keep) != len(bindings):
            e["mechanic_bindings"] = keep
    return hosted, notes
