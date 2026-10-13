#!/usr/bin/env python3
"""Download a Uthana motion as animation-only GLB (no character mesh).

Same retarget-on-download flow as download_retarget_motion.py, but with
``no_mesh=True`` so the file contains skeleton + animation only.

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

CHARACTER_ID = "cs4HMGRjRPiE"
MOTION_ID = "mf6sNYUfWEYL"

# File path for the downloaded GLB (or a directory — then motion-only.glb).
OUTPUT_PATH = Path(r"C:\Users\micha\Documents\Prompt-To-VR\Sequential_Runs\Uthana_R_A")

# Fixed for this Sequential_Runs workflow (animation data without mesh).
OUTPUT_FORMAT = "glb"
NO_MESH = True

# ============================================================
# IMPLEMENTATION
# ============================================================


def _resolve_output_path(path: Path) -> Path:
    resolved = path.expanduser().resolve()
    if resolved.is_dir() or resolved.suffix.lower() != f".{OUTPUT_FORMAT}":
        return resolved / f"motion-only.{OUTPUT_FORMAT}"
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
        f"(format={OUTPUT_FORMAT}, no_mesh={NO_MESH})..."
    )
    glb_data = await client.motions.download(
        CHARACTER_ID,
        MOTION_ID,
        output_format=OUTPUT_FORMAT,
        no_mesh=NO_MESH,
    )
    out_path.write_bytes(glb_data)
    print(f"Wrote {out_path} ({len(glb_data)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main()))
