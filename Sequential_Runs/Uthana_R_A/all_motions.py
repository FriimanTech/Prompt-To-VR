#!/usr/bin/env python3
"""List motions in your Uthana account.

Edit LIST_LIMIT, then run this file. Copy a motion id into
download_retarget_motion.py together with a character_id.
"""

from __future__ import annotations

import asyncio
import sys
from pathlib import Path

_SCRIPT_DIR = Path(__file__).resolve().parent
if str(_SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(_SCRIPT_DIR))

from uthana_common import make_client  # noqa: E402

# ============================================================
# USER CONFIGURATION
# ============================================================
#
# API key is loaded from UTHANA_API_KEY in the repo-root .env
# (or the process environment). Do not paste the key here.

# How many motions to print (from the start of the list). Use None for all.
LIST_LIMIT: int | None = 10000

# ============================================================
# IMPLEMENTATION
# ============================================================


def _field(item: object, key: str) -> object:
    if isinstance(item, dict):
        return item.get(key)
    return getattr(item, key, None)


async def main() -> int:
    client = make_client()
    motions = await client.motions.list()
    if not motions:
        print("No motions returned.")
        return 0

    subset = motions if LIST_LIMIT is None else motions[:LIST_LIMIT]
    print(f"Showing {len(subset)} of {len(motions)} motion(s):")
    for motion in subset:
        print(_field(motion, "id"), _field(motion, "name"))
    print("Copy a motion id into download_retarget_motion.py MOTION_ID.")
    return 0


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main()))
