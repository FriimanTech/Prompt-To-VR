#!/usr/bin/env python3
"""List every Uthana locomotion style_id you can pass to create_locomotion.

Edit nothing — just run this file and copy a style_id into
locomotion_animation.py.
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
#
# No other knobs — this script only lists styles.

# ============================================================
# IMPLEMENTATION
# ============================================================


async def main() -> int:
    client = make_client()
    styles = await client.motions.list_locomotion_styles()
    if not styles:
        print("No locomotion styles returned.")
        return 0

    print(f"Found {len(styles)} locomotion style(s):")
    for style_id in styles:
        print(style_id)
    print("Copy a style_id into locomotion_animation.py STYLE_ID.")
    return 0


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main()))
