"""CI-style guard script.

Ensures that every generated file (``.cpp``, ``.h``, ``.hpp``) under
``engine/generated/`` lives in one of the three allowed subdirectories,
only includes whitelisted headers (plus same-directory generated headers),
and that header files do not contain registration macros.

Run::

    py engine\\tools\\check_generated_bounds.py
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ALLOWED_ROOTS = ("mechanics", "ai", "dialogue", "world")

HEADER_PREFIXES = (
    "ptv/ecs/",
    "ptv/math/",
    "ptv/mechanics/",
    "ptv/dialogue/",
    "ptv/ai/",
    "glm/",
    "entt/",
    "nlohmann/",
)

# The world/ subtree targets the offline ptv_worldgen host and has a tighter
# include surface: only the IWorldBuilder interface header, the C++ standard
# library, and GLM.
WORLD_PTV_API_EXACT = {"ptv/api/IWorldBuilder.h"}
WORLD_HEADER_PREFIXES = ("glm/",)
WORLD_REGISTRATION_RE = re.compile(r"\bPTV_REGISTER_WORLD_BUILDER\b")

# Only the three stable interface headers live under ptv/api/. See
# apps/orchestrator/ptv_orchestrator/pipeline/bounds.py for the matching
# runtime logic.
PTV_API_EXACT = {
    "ptv/api/IMechanic.h",
    "ptv/api/IBehaviour.h",
    "ptv/api/IDialogueHook.h",
    # Read-only live controller/headset input handed to on_update.
    "ptv/api/InputSnapshot.h",
}

# Keep in sync with ``_HEADER_WHITELIST_EXACT`` in
# apps/orchestrator/ptv_orchestrator/pipeline/bounds.py. Guiding principle:
# only pure, deterministic, side-effect-free standard headers -- no console/
# file I/O, threading/atomics, wall-clock time, filesystem, or process control
# (those break the ECS single-threaded, deterministic execution model).
HEADER_EXACT = {
    # Integer / byte / math primitives.
    "cstdint", "cstddef", "cstring", "cmath", "cfloat", "climits", "cstdlib",
    "limits", "bit", "numeric", "ratio", "complex", "compare",
    # Strings / text (in-memory only; NOT iostream/fstream).
    "string", "string_view", "cctype", "cwctype", "charconv", "sstream",
    "iomanip",
    # Containers.
    "vector", "array", "span", "deque", "list", "forward_list", "stack",
    "queue", "unordered_map", "unordered_set", "map", "set", "bitset",
    "initializer_list",
    # Utilities / metaprogramming.
    "optional", "variant", "tuple", "algorithm", "functional", "memory", "new",
    "utility", "iterator", "chrono", "type_traits", "random",
    # Error handling.
    "cassert", "stdexcept", "exception",
}

INCLUDE_RE = re.compile(r"""^\s*#\s*include\s*[<"](?P<h>[^">]+)[">]""")

REGISTRATION_RE = re.compile(
    r"\bPTV_REGISTER_(?:MECHANIC|BEHAVIOUR|DIALOGUE_HOOK)\b"
)

_GENERATED_EXTS = ("*.cpp", "*.h", "*.hpp")


def _local_headers_in(directory: Path) -> frozenset[str]:
    """Return the set of ``.h``/``.hpp`` filenames present in *directory*."""
    names: set[str] = set()
    for ext in ("*.h", "*.hpp"):
        for hp in directory.glob(ext):
            names.add(hp.name)
    return frozenset(names)


def _is_bare_local_header(header: str) -> bool:
    if "/" in header or "\\" in header or ".." in header:
        return False
    low = header.lower()
    return low.endswith(".h") or low.endswith(".hpp")


def main(root: Path) -> int:
    gen_root = root / "engine" / "generated"
    if not gen_root.exists():
        return 0
    errors: list[str] = []

    # Collect all generated files across all three extensions.
    all_files: list[Path] = []
    for ext in _GENERATED_EXTS:
        all_files.extend(gen_root.rglob(ext))

    for p in sorted(all_files):
        rel = p.relative_to(gen_root)
        first = rel.parts[0]
        if first not in ALLOWED_ROOTS:
            errors.append(f"{p}: outside allowed subtrees {ALLOWED_ROOTS}")
            continue

        is_header = p.suffix.lower() in (".h", ".hpp")
        is_world = first == "world"
        local_headers = _local_headers_in(p.parent)
        text = p.read_text(encoding="utf-8")

        reg_re = WORLD_REGISTRATION_RE if is_world else REGISTRATION_RE

        for i, line in enumerate(text.splitlines(), 1):
            # Check registration macros in headers.
            if is_header and reg_re.search(line):
                macro = reg_re.search(line).group(0)  # type: ignore[union-attr]
                errors.append(
                    f"{p}:{i}: header files must not contain {macro} "
                    f"-- move registration to a .cpp file"
                )

            m = INCLUDE_RE.match(line)
            if not m:
                continue
            h = m.group("h")
            if h in HEADER_EXACT:
                continue
            if is_world:
                if (
                    h in WORLD_PTV_API_EXACT
                    or h.lower() in {x.lower() for x in WORLD_PTV_API_EXACT}
                    or any(h.startswith(pref) for pref in WORLD_HEADER_PREFIXES)
                ):
                    continue
            else:
                if h in PTV_API_EXACT or h.lower() in {
                    x.lower() for x in PTV_API_EXACT
                }:
                    continue
                if any(h.startswith(pref) for pref in HEADER_PREFIXES):
                    continue
            # Same-directory generated header (bare filename, must exist).
            if _is_bare_local_header(h) and h in local_headers:
                continue
            if _is_bare_local_header(h):
                errors.append(
                    f"{p}:{i}: include \"{h}\" references a header not "
                    f"found in {p.parent}"
                )
            else:
                errors.append(f"{p}:{i}: disallowed include '{h}'")

    if errors:
        for e in errors:
            print(e, file=sys.stderr)
        return 1
    print("engine/generated/: ok")
    return 0


if __name__ == "__main__":
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[2]
    raise SystemExit(main(root))
