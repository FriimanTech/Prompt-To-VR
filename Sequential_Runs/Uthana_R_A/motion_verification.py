#!/usr/bin/env python3
"""Motion Verification: is there already a motion with this exact name?

Pass a motion name in the format ``style-str{N}-ms{speed}-deg{angle}``
(for example ``neutral_non_gendered-str3-ms1.4-deg0``) on the command line or
in MOTION_NAME below. Lists the organisation's motions (first 500) and
prints either::

    match found: <name> <motion id>

or::

    no match is found in the Uthana Organization for that motion requested

The VR pipeline runs the identical check before generating a locomotion.
"""

from __future__ import annotations

import asyncio
import re
import sys
from pathlib import Path

_SCRIPT_DIR = Path(__file__).resolve().parent
if str(_SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(_SCRIPT_DIR))

from uthana_common import fail, make_client  # noqa: E402

# ============================================================
# USER CONFIGURATION
# ============================================================

MOTION_NAME = "neutral_non_gendered-str3-ms1.4-deg0"
LIST_LIMIT = 500

# ============================================================
# IMPLEMENTATION
# ============================================================

_NAME_RE = re.compile(r"^[A-Za-z0-9_]+-str[1-5]-ms\d+(\.\d+)?-deg-?\d+(\.\d+)?$")


async def main() -> int:
    name = (sys.argv[1] if len(sys.argv) > 1 else MOTION_NAME).strip()
    if not _NAME_RE.match(name):
        fail(f"{name!r} is not in the style-str{{N}}-ms{{speed}}-deg{{angle}} format.")
    client = make_client()
    motions = await client.motions.list()
    for m in motions[:LIST_LIMIT]:
        if m.get("name") == name:
            print(f"match found: {m.get('name')} {m.get('id')}")
            return 0
    print("no match is found in the Uthana Organization for that motion requested")
    return 1


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main()))
