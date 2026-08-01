"""Boundary enforcement for generated C++ code.

Validates that generated files (both ``.cpp`` and ``.h``/``.hpp``) live inside
``engine/generated/{mechanics,ai,dialogue}/`` and only ``#include`` headers
from the engine's public API, approved third-party libraries, the C++ standard
library, or same-task generated headers in the same directory.
"""

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

# ``ptv/api/*`` is intentionally NOT a prefix match: only the headers listed
# here actually exist on disk. Any other ``ptv/api/foo.h`` is almost always a
# model hallucination (e.g. input.h, audio_capture.h).
_PTV_API_ALLOWED = frozenset(
    {
        "ptv/api/IMechanic.h",
        "ptv/api/IBehaviour.h",
        "ptv/api/IDialogueHook.h",
        # Read-only live controller/headset input handed to on_update.
        "ptv/api/InputSnapshot.h",
        # Locomotion / physics / debug-draw / hand services handed to
        # on_update. Pulled in by IMechanic.h, but allowed explicitly so an
        # agent may include it directly.
        "ptv/api/MechanicContext.h",
    }
)

# Allowed C++ standard-library headers. Guiding principle: permit pure,
# deterministic, side-effect-free computation. Generated gameplay code runs
# inside the ECS on the engine's main loop, so headers that do console/file
# I/O, spawn threads, touch atomics, read wall-clock time, or reach the
# filesystem / process are intentionally EXCLUDED (see _FORBIDDEN_STD_HEADERS
# below) -- they break determinism or the single-threaded execution model.
_HEADER_WHITELIST_EXACT = {
    # Integer / byte / math primitives.
    "cstdint",
    "cstddef",
    "cstring",
    "cmath",
    "cfloat",
    "climits",
    "cstdlib",
    "limits",
    "bit",
    "numeric",
    "ratio",
    "complex",
    "compare",
    # Strings / text (in-memory only; NOT iostream/fstream).
    "string",
    "string_view",
    "cctype",
    "cwctype",
    "charconv",
    "sstream",
    "iomanip",
    # Containers.
    "vector",
    "array",
    "span",
    "deque",
    "list",
    "forward_list",
    "stack",
    "queue",
    "unordered_map",
    "unordered_set",
    "map",
    "set",
    "bitset",
    "initializer_list",
    # Utilities / metaprogramming.
    "optional",
    "variant",
    "tuple",
    "algorithm",
    "functional",
    "memory",
    "new",
    "utility",
    "iterator",
    "chrono",
    "type_traits",
    "random",
    # Error handling.
    "cassert",
    "stdexcept",
    "exception",
}

# Documented for humans (and tests): standard headers we deliberately keep
# OUT of the allowlist because they violate determinism or the
# single-threaded, no-side-effects execution model of generated gameplay
# code. Not used for matching (anything not in the allowlist is rejected);
# this is the rationale record.
_FORBIDDEN_STD_HEADERS = frozenset(
    {
        # Console / file I/O.
        "iostream",
        "fstream",
        "cstdio",
        "syncstream",
        # Filesystem.
        "filesystem",
        # Threading / concurrency / atomics.
        "thread",
        "mutex",
        "shared_mutex",
        "atomic",
        "future",
        "condition_variable",
        "semaphore",
        "barrier",
        "latch",
        "stop_token",
        "execution",
        # Wall-clock time (use the engine's dt, not real time).
        "ctime",
        # Heavy / locale-dependent.
        "regex",
        "locale",
        "clocale",
    }
)

_REGISTRATION_MACROS = re.compile(
    r"\bPTV_REGISTER_(?:MECHANIC|BEHAVIOUR|DIALOGUE_HOOK)\b"
)


class GeneratedBoundsError(RuntimeError):
    pass


def _is_bare_local_header(header: str) -> bool:
    """Return ``True`` when *header* is a bare filename ending in ``.h`` or
    ``.hpp`` with no path separators or back-references."""
    if "/" in header or "\\" in header or ".." in header:
        return False
    low = header.lower()
    return low.endswith(".h") or low.endswith(".hpp")


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


# ---------------------------------------------------------------------------
# Include validation (fail-loudly path -- preferred)
# ---------------------------------------------------------------------------


def validate_includes(
    text: str,
    filename: str,
    *,
    local_headers: frozenset[str] = frozenset(),
    dep_headers: frozenset[str] = frozenset(),
) -> list[str]:
    """Scan *text* for ``#include`` directives and return a list of
    human-readable violation strings.  An empty list means all includes
    are allowed.

    *local_headers* is the set of bare header filenames emitted by the
    same CodeTask.  *dep_headers* is the set of bare header filenames
    emitted by tasks this task declares in ``depends_on`` (they live in
    the same project directory and are guaranteed built first, so a
    dependent task MAY include them -- e.g. a weapon profile including
    ``shooting_hub_shared.h``).  Bare-filename includes that are in
    NEITHER set are rejected (prevents undeclared cross-task coupling or
    hallucinated local headers).
    """
    violations: list[str] = []
    for lineno, line in enumerate(text.splitlines(), 1):
        m = _ALLOWED_HEADERS.match(line)
        if not m:
            continue
        header = m.group("h")
        if _header_allowed(header, local_headers, dep_headers):
            continue

        if _is_bare_local_header(header):
            violations.append(
                f"{filename}:{lineno}: include \"{header}\" references a "
                f"header not produced by this task or any of its declared "
                f"dependency tasks. "
                f"This task's headers: {sorted(local_headers)}. "
                f"Dependency-task headers: {sorted(dep_headers)}. "
                f"Emit the header in your files[], add the owning task to "
                f"depends_on, or fix the typo."
            )
        else:
            violations.append(
                f"{filename}:{lineno}: disallowed include '{header}' -- "
                f"this header is not in the engine allowlist. "
                f"See the allowed-includes list in CODE_TASK_SYSTEM."
            )
    return violations


def validate_header_content(text: str, filename: str) -> list[str]:
    """Check that a generated ``.h``/``.hpp`` file does not contain
    registration macros (those must live in ``.cpp`` files only).
    Returns a list of violation strings (empty = OK)."""
    violations: list[str] = []
    for lineno, line in enumerate(text.splitlines(), 1):
        if _REGISTRATION_MACROS.search(line):
            macro = _REGISTRATION_MACROS.search(line).group(0)  # type: ignore[union-attr]
            violations.append(
                f"{filename}:{lineno}: header files must not contain "
                f"{macro}. Move registration to a .cpp file."
            )
    return violations


# ---------------------------------------------------------------------------
# Include filter (secondary safety-net -- belt-and-suspenders)
# ---------------------------------------------------------------------------


def filter_cpp_only(
    text: str,
    *,
    local_headers: frozenset[str] = frozenset(),
    dep_headers: frozenset[str] = frozenset(),
) -> str:
    """Replace disallowed ``#include`` lines with a comment.

    This is a secondary defence behind :func:`validate_includes` which
    should already have rejected the file.  ``local_headers`` is the set
    of bare header filenames produced by the same CodeTask; ``dep_headers``
    is the set produced by its declared ``depends_on`` tasks (allowed).
    """
    out_lines: list[str] = []
    for line in text.splitlines():
        m = _ALLOWED_HEADERS.match(line)
        if m:
            header = m.group("h")
            if not _header_allowed(header, local_headers, dep_headers):
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


def _header_allowed(
    header: str,
    local_headers: frozenset[str] = frozenset(),
    dep_headers: frozenset[str] = frozenset(),
) -> bool:
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
    if _is_bare_local_header(header) and (
        header in local_headers or header in dep_headers
    ):
        return True
    return False


# ---------------------------------------------------------------------------
# World-generation bounds (engine/generated/world/)
#
# Generated VR-environment code targets the offline ptv_worldgen host, NOT the
# engine runtime. Its include surface is intentionally tiny: only the single
# IWorldBuilder interface header, the C++ standard library, and GLM. It may NOT
# include ECS (entt / ptv/ecs), nlohmann, or any other engine internal, so it
# can never reach or modify the renderer, XR loop, physics, or ECS core.
# ---------------------------------------------------------------------------

_WORLD_PTV_API_ALLOWED = frozenset({"ptv/api/IWorldBuilder.h"})
_WORLD_HEADER_WHITELIST_PREFIXES = ("glm/",)

_WORLD_REGISTRATION_MACRO = re.compile(r"\bPTV_REGISTER_WORLD_BUILDER\b")


def _world_header_allowed(
    header: str,
    local_headers: frozenset[str] = frozenset(),
) -> bool:
    if header in _HEADER_WHITELIST_EXACT:
        return True
    if header in _WORLD_PTV_API_ALLOWED:
        return True
    if header.lower() == "ptv/api/iworldbuilder.h":
        return True
    for p in _WORLD_HEADER_WHITELIST_PREFIXES:
        if header.startswith(p):
            return True
    if _is_bare_local_header(header) and header in local_headers:
        return True
    return False


def assert_world_path_allowed(path: Path, gen_root: Path) -> None:
    """Raise if ``path`` is outside ``<repo>/engine/generated/world/``."""
    rp = path.resolve()
    gr = gen_root.resolve()
    if not str(rp).startswith(str(gr)):
        raise GeneratedBoundsError(f"Generated world file {rp} is outside {gr}")
    rel = rp.relative_to(gr)
    first = rel.parts[0] if rel.parts else ""
    if first != "world":
        raise GeneratedBoundsError(
            f"Generated world file must live under world/ (got '{first}')"
        )


def validate_world_includes(
    text: str,
    filename: str,
    *,
    local_headers: frozenset[str] = frozenset(),
) -> list[str]:
    """Like :func:`validate_includes` but for the world-generation surface:
    only ``ptv/api/IWorldBuilder.h`` + std + GLM + same-task headers."""
    violations: list[str] = []
    for lineno, line in enumerate(text.splitlines(), 1):
        m = _ALLOWED_HEADERS.match(line)
        if not m:
            continue
        header = m.group("h")
        if _world_header_allowed(header, local_headers):
            continue
        if _is_bare_local_header(header):
            violations.append(
                f"{filename}:{lineno}: include \"{header}\" references a header "
                f"not produced by this task. This task's headers: "
                f"{sorted(local_headers)}."
            )
        else:
            violations.append(
                f"{filename}:{lineno}: disallowed include '{header}' -- world "
                f"code may only include \"ptv/api/IWorldBuilder.h\", the C++ "
                f"standard library, and <glm/...>."
            )
    return violations


def validate_world_header_content(text: str, filename: str) -> list[str]:
    """Reject ``PTV_REGISTER_WORLD_BUILDER`` in a header (must be in a .cpp)."""
    violations: list[str] = []
    for lineno, line in enumerate(text.splitlines(), 1):
        if _WORLD_REGISTRATION_MACRO.search(line):
            violations.append(
                f"{filename}:{lineno}: header files must not contain "
                f"PTV_REGISTER_WORLD_BUILDER. Move registration to a .cpp file."
            )
    return violations


def filter_world_cpp_only(
    text: str,
    *,
    local_headers: frozenset[str] = frozenset(),
) -> str:
    """Secondary safety-net: comment out disallowed includes in world code."""
    out_lines: list[str] = []
    for line in text.splitlines():
        m = _ALLOWED_HEADERS.match(line)
        if m and not _world_header_allowed(m.group("h"), local_headers):
            out_lines.append(
                f"// [ptv-bounds] removed disallowed include: {m.group('h')}"
            )
            continue
        out_lines.append(line)
    return "\n".join(out_lines)
