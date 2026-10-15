#!/usr/bin/env python3
"""List your motions: every motion in the Uthana organisation (first 500).

Edit nothing -- run this file. Prints ``<motion id> <motion name>`` per line.
The VR pipeline uses the same listing (``uthana_motion_list_limit``) before
it decides whether a motion has to be generated.
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

LIST_LIMIT = 500

# ============================================================
# IMPLEMENTATION
# ============================================================


async def main() -> int:
    client = make_client()
    motions = await client.motions.list()
    for m in motions[:LIST_LIMIT]:
        print(m.get("id"), m.get("name"))
    if len(motions) > LIST_LIMIT:
        print(f"(showing {LIST_LIMIT} of {len(motions)}; raise LIST_LIMIT to see all)")
    return 0


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main()))
