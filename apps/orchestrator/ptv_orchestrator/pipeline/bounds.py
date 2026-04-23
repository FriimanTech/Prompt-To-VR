"""Boundary enforcement for generated C++ code."""

from __future__ import annotations

import re
from pathlib import Path


_ALLOWED_HEADERS = re.compile(
    r"""#\s*include\s*[<"](?P<h>[^">]+)[">]""",
    re.MULTILINE,
)

_HEADER_WHITELIST_PREFIXES = (
    "ptv/ecs/",
    "ptv/math/",
    "ptv/mechanics/",
    "ptv/dialogue/",
    "ptv/ai/",
    "glm/",
    "entt/",
    "nlohmann/",
)

# ``ptv/api/*`` is intentionally NOT a prefix match: only the three stable
# interface headers actually exist on disk. Any other ``ptv/api/foo.h`` is
# almost always a model hallucination (e.g. input.h, audio_capture.h).
_PTV_API_ALLOWED = frozenset(
    {
        "ptv/api/IMechanic.h",
        "ptv/api/IBehaviour.h",
        "ptv/api/IDialogueHook.h",
    }
)

_HEADER_WHITELIST_EXACT = {
    "cstdint",
    "cstddef",
    "cstring",
    "cmath",
    "cfloat",
    "string",
    "string_view",
    "vector",
    "array",
    "span",
    "unordered_map",
    "unordered_set",
    "map",
    "set",
    "optional",
    "variant",
    "tuple",
    "algorithm",
    "functional",
    "memory",
    "utility",
    "chrono",
    "type_traits",
    "numeric",
}


class GeneratedBoundsError(RuntimeError):
    pass


def assert_path_allowed(path: Path, gen_root: Path) -> None:
    """Raise if ``path`` is outside ``<repo>/engine/generated/{mechanics,ai,dialogue}/``."""
    rp = path.resolve()
    gr = gen_root.resolve()
    if not str(rp).startswith(str(gr)):
        raise GeneratedBoundsError(
            f"Generated file {rp} is outside {gr}"
        )
    rel = rp.relative_to(gr)
    first = rel.parts[0] if rel.parts else ""
    if first not in ("mechanics", "ai", "dialogue"):
        raise GeneratedBoundsError(
            f"Generated file must live under mechanics/, ai/, or dialogue/ (got '{first}')"
        )


def filter_cpp_only(text: str) -> str:
    """Reject obviously out-of-bounds includes.

    The goal is to keep generated modules from reaching into engine internals
    or arbitrary system headers. If any include fails the whitelist we replace
    it with a commented-out line so the code still compiles after review.
    """
    out_lines: list[str] = []
    for line in text.splitlines():
        m = _ALLOWED_HEADERS.match(line)
        if m:
            header = m.group("h")
            if not _header_allowed(header):
                out_lines.append(f"// [ptv-bounds] removed disallowed include: {header}")
                continue
        out_lines.append(line)
    return "\n".join(out_lines)


def _header_allowed(header: str) -> bool:
    if header in _HEADER_WHITELIST_EXACT:
        return True
    if header in _PTV_API_ALLOWED:
        return True
    # Case-insensitive match for the ptv/api/ interface headers (the LLM
    # sometimes lowercases them).
    low = header.lower()
    for h in _PTV_API_ALLOWED:
        if low == h.lower():
            return True
    for p in _HEADER_WHITELIST_PREFIXES:
        if header.startswith(p):
            return True
    return False
