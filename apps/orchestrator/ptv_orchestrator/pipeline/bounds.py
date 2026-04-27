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


# ---------------------------------------------------------------------------
# Auto-repair for known LLM slip-ups
# ---------------------------------------------------------------------------


# Locator for ``<lhs>.scale = Vec3``. Once we land on this prefix we walk
# the source forward by hand to find the matching brace / paren so we can
# tolerate nested calls like ``Vec3(1.0f + std::sin(t), ...)`` that a flat
# regex would mis-balance.
_SCALE_VEC3_PREFIX = re.compile(
    r"(?P<lhs>[A-Za-z_][\w.]*\.scale)\s*=\s*Vec3\s*(?P<open>[\{(])"
)


_BRACE_PAIRS = {"{": "}", "(": ")"}


def _split_top_level_args(s: str) -> list[str]:
    """Split a comma-delimited Vec3 argument list while respecting nested
    ``()``/``{}``/``[]``. Whitespace around each argument is stripped."""
    out: list[str] = []
    buf: list[str] = []
    depth = 0
    for ch in s:
        if ch in "({[":
            depth += 1
        elif ch in ")}]":
            depth = max(0, depth - 1)
        if ch == "," and depth == 0:
            out.append("".join(buf).strip())
            buf = []
        else:
            buf.append(ch)
    if buf:
        out.append("".join(buf).strip())
    return out


def _repair_scale_vec3(text: str) -> tuple[str, int]:
    """Rewrite ``tr.scale = Vec3{...};`` (or ``Vec3(...);``) to a scalar.

    Returns the repaired text plus the number of replacements made.
    """
    count = 0
    out: list[str] = []
    pos = 0
    n = len(text)
    while pos < n:
        m = _SCALE_VEC3_PREFIX.search(text, pos)
        if m is None:
            out.append(text[pos:])
            break
        # Emit anything before the match unchanged.
        out.append(text[pos:m.start()])

        open_ch = m.group("open")
        close_ch = _BRACE_PAIRS[open_ch]
        # Walk to the matching close-bracket, respecting nesting.
        depth = 1
        i = m.end()  # one past the opening bracket
        while i < n and depth > 0:
            c = text[i]
            if c == open_ch:
                depth += 1
            elif c == close_ch:
                depth -= 1
                if depth == 0:
                    break
            i += 1
        if i >= n or depth != 0:
            # Malformed; bail out without rewriting this site.
            out.append(text[m.start():m.end()])
            pos = m.end()
            continue
        args_str = text[m.end():i]
        # Skip past optional whitespace + trailing semicolon.
        j = i + 1
        while j < n and text[j] in " \t":
            j += 1
        if j < n and text[j] == ";":
            j += 1

        args = _split_top_level_args(args_str)
        first_arg = args[0] if args else ""
        replacement = first_arg if first_arg else "1.0f"
        out.append(
            f"{m.group('lhs')} = {replacement}; "
            f"// [ptv-repair] Transform::scale is float, not Vec3"
        )
        count += 1
        pos = j

    return "".join(out), count


def repair_generated_cpp(text: str) -> tuple[str, list[str]]:
    """Fix common LLM compile-time slip-ups in generated C++.

    Returns the repaired text plus a list of human-readable repair notes
    that the code-gen stage logs for traceability. The function MUST be a
    pure string transform so it can run without engine headers available.

    Currently fixes:
      * ``Transform::scale = Vec3{...}``  -> ``Transform::scale = <scalar>``

    Add new repairs here as new failure modes appear.
    """

    notes: list[str] = []
    repaired, n_scale = _repair_scale_vec3(text)
    if n_scale:
        notes.append(
            f"rewrote {n_scale} 'Transform::scale = Vec3{{...}}' assignment(s) "
            "to a scalar float (Transform::scale is a uniform float, not a Vec3)"
        )
    return repaired, notes


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
