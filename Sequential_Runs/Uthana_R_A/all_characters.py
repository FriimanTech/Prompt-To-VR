#!/usr/bin/env python3
"""List characters in your Uthana account.

Edit LIST_LIMIT, then run this file. Copy a character id into
locomotion_animation.py, download_retarget_motion.py, or motion_only.py.

Docs: https://uthana.com/docs/api/capabilities/asset-management#list-your-characters
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

# How many characters to print (from the start of the list). Use None for all.
LIST_LIMIT: int | None = 10

# ============================================================
# IMPLEMENTATION
# ============================================================


def _field(item: object, key: str) -> object:
    if isinstance(item, dict):
        return item.get(key)
    return getattr(item, key, None)


async def main() -> int:
    client = make_client()
    characters = await client.characters.list()
    if not characters:
        print("No characters returned.")
        return 0

    subset = characters if LIST_LIMIT is None else characters[:LIST_LIMIT]
    print(f"Showing {len(subset)} of {len(characters)} character(s):")
    for character in subset:
        print(_field(character, "id"), _field(character, "name"))
    print("Copy a character id into CHARACTER_ID in the next script.")
    return 0


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main()))
