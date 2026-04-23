"""CI-style guard script.

Ensures that every file under ``engine/generated/`` lives in one of the three
allowed subdirectories and only includes whitelisted headers.

Run::

    py engine\\tools\\check_generated_bounds.py
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ALLOWED_ROOTS = ("mechanics", "ai", "dialogue")

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

# Only the three stable interface headers live under ptv/api/. See
# apps/orchestrator/ptv_orchestrator/pipeline/bounds.py for the matching
# runtime logic.
PTV_API_EXACT = {
    "ptv/api/IMechanic.h",
    "ptv/api/IBehaviour.h",
    "ptv/api/IDialogueHook.h",
}

HEADER_EXACT = {
    "cstdint", "cstddef", "cstring", "cmath", "cfloat",
    "string", "string_view", "vector", "array", "span",
    "unordered_map", "unordered_set", "map", "set",
    "optional", "variant", "tuple", "algorithm", "functional",
    "memory", "utility", "chrono", "type_traits", "numeric",
}

INCLUDE_RE = re.compile(r"""^\s*#\s*include\s*[<"](?P<h>[^">]+)[">]""")


def main(root: Path) -> int:
    gen_root = root / "engine" / "generated"
    if not gen_root.exists():
        return 0
    errors: list[str] = []
    for p in gen_root.rglob("*.cpp"):
        rel = p.relative_to(gen_root)
        first = rel.parts[0]
        if first not in ALLOWED_ROOTS:
            errors.append(f"{p}: outside allowed subtrees {ALLOWED_ROOTS}")
            continue
        for i, line in enumerate(p.read_text(encoding="utf-8").splitlines(), 1):
            m = INCLUDE_RE.match(line)
            if not m:
                continue
            h = m.group("h")
            if h in HEADER_EXACT:
                continue
            if h in PTV_API_EXACT or h.lower() in {x.lower() for x in PTV_API_EXACT}:
                continue
            if any(h.startswith(pref) for pref in HEADER_PREFIXES):
                continue
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
