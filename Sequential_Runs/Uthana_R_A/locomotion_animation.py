#!/usr/bin/env python3
"""Standalone Uthana locomotion motion creation.

Edit the USER CONFIGURATION section, then run this file.
Prints motion id and name — pass the id into download_retarget_motion.py
together with CHARACTER_ID to download the animated GLB.
"""

from __future__ import annotations

import asyncio
import sys
from pathlib import Path

_SCRIPT_DIR = Path(__file__).resolve().parent
if str(_SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(_SCRIPT_DIR))

from uthana_common import fail, make_client  # noqa: E402

# ============================================================
# USER CONFIGURATION
# ============================================================
#
# API key is loaded from UTHANA_API_KEY in the repo-root .env
# (or the process environment). Do not paste the key here.

# From u_auto_rig.py (or any existing Uthana character).
CHARACTER_ID = "c4hnjNtFjqUg"

# Count of full stride pairs (left+right or right+left). Allowed 1–5.
STRIDES = 4

# Travel speed in m/s relative to the character (roughly 0.25–6.0; max 8.0).
MOVE_SPEED = 4.0

# From query_locomotion_styles.py (case-insensitive). Default neutral_male_a.
STYLE_ID = "neutral_male_a"

# Ground-plane travel direction in degrees, normalized to -180..180.
# 0 = forward, 90 = right, -90 = left, 180 / -180 = backward.
TRAVEL_ANGLE = 0.0

# ============================================================
# IMPLEMENTATION
# ============================================================


def _field(item: object, key: str) -> object:
    if isinstance(item, dict):
        return item.get(key)
    return getattr(item, key, None)


async def main() -> int:
    if not CHARACTER_ID or CHARACTER_ID == "your-character-id":
        fail("Set CHARACTER_ID to the id printed by u_auto_rig.py.")

    client = make_client()
    print(
        f"Creating locomotion for character_id={CHARACTER_ID} "
        f"(strides={STRIDES}, move_speed={MOVE_SPEED}, "
        f"style_id={STYLE_ID!r}, travel_angle={TRAVEL_ANGLE})..."
    )
    result = await client.motions.create_locomotion(
        CHARACTER_ID,
        strides=STRIDES,
        move_speed=MOVE_SPEED,
        style_id=STYLE_ID,
        travel_angle=TRAVEL_ANGLE,
    )
    motion_id = getattr(result, "motion_id", None)
    if isinstance(result, dict):
        motion_id = motion_id or result.get("motion_id")
    if not motion_id:
        fail(f"No motion_id in create_locomotion response: {result!r}")

    motion = await client.motions.get(motion_id)
    motion_name = _field(motion, "name")

    print(f"motion_id={motion_id}")
    print(f"motion_name={motion_name}")
    print("Copy motion_id + CHARACTER_ID into download_retarget_motion.py.")
    return 0


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main()))
