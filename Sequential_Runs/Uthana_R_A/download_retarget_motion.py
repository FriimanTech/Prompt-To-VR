#!/usr/bin/env python3
"""Download (and retarget) a Uthana motion as an in-place GLB.

Uthana's any-motion-to-any-bipedal path means the same download call works
whether CHARACTER_ID was the character used when the motion was created, or
a different character you want that motion retargeted onto.

Requires ``uthana>=0.5.1`` (``in_place`` was added in 0.5.x).

Edit the USER CONFIGURATION section, then run this file.
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

CHARACTER_ID = "cq9ZEWHpYmwD"
MOTION_ID = "mf6sNYUfWEYL"

# File path for the downloaded GLB (or a directory — then motion.glb is used).
OUTPUT_PATH = Path(r"C:\Users\micha\Documents\Prompt-To-VR\Sequential_Runs\Uthana_R_A")

# Fixed for this Sequential_Runs workflow (VR-friendly in-place clip).
OUTPUT_FORMAT = "glb"
IN_PLACE = True

# ============================================================
# IMPLEMENTATION
# ============================================================


def _resolve_output_path(path: Path) -> Path:
    resolved = path.expanduser().resolve()
    if resolved.is_dir() or resolved.suffix.lower() != f".{OUTPUT_FORMAT}":
        return resolved / f"motion.{OUTPUT_FORMAT}"
    return resolved


async def main() -> int:
    if not CHARACTER_ID or CHARACTER_ID == "your-character-id":
        fail("Set CHARACTER_ID (from u_auto_rig.py or text_to_motion.py).")
    if not MOTION_ID or MOTION_ID == "your-motion-id":
        fail(
            "Set MOTION_ID (from locomotion_animation.py, text_to_motion.py, "
            "or all_motions.py)."
        )

    out_path = _resolve_output_path(OUTPUT_PATH)
    out_path.parent.mkdir(parents=True, exist_ok=True)

    client = make_client()
    print(
        f"Downloading motion_id={MOTION_ID} for character_id={CHARACTER_ID} "
        f"(format={OUTPUT_FORMAT}, in_place={IN_PLACE})..."
    )
    glb_data = await client.motions.download(
        CHARACTER_ID,
        MOTION_ID,
        output_format=OUTPUT_FORMAT,
        in_place=IN_PLACE,
    )
    out_path.write_bytes(glb_data)
    print(f"Wrote {out_path} ({len(glb_data)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main()))
