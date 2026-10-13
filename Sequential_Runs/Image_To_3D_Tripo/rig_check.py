#!/usr/bin/env python3
"""Standalone Tripo V3 riggability check.

Edit INPUT_GLB_PATH, then run this file. No credits are consumed.
"""

from __future__ import annotations

import sys
from pathlib import Path

_SCRIPT_DIR = Path(__file__).resolve().parent
if str(_SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(_SCRIPT_DIR))

from tripo_common import (
    TripoError,
    create_task,
    fail,
    poll_task,
    require_file,
    upload_file,
)

# ============================================================
# USER CONFIGURATION
# ============================================================
#
# API key is loaded from TRIPO_API_KEY in the repo-root .env
# (or the process environment). Do not paste the key here.

INPUT_GLB_PATH = Path(r"C:\\Users\\micha\\Downloads\\Tranformaer picture.glb")

# ============================================================
# IMPLEMENTATION
# ============================================================

_MODEL_MAX_BYTES = 150 * 1024 * 1024
_RIG_TYPES = (
    "biped",
    "quadruped",
    "hexapod",
    "octopod",
    "avian",
    "serpentine",
    "aquatic",
)


def main() -> int:
    glb = require_file(INPUT_GLB_PATH, kind="Input GLB")
    if glb.suffix.lower() != ".glb":
        fail(f"Rig-check expects a .glb file, got {glb.suffix}")
    size = glb.stat().st_size
    if size > _MODEL_MAX_BYTES:
        fail(f"GLB is {size} bytes; Tripo model uploads must be <= 150 MB.")

    file_token = upload_file(glb, label="GLB")
    task_id = create_task("/animations/rig-check", {"input": file_token})
    task = poll_task(task_id)

    output = task.get("output")
    output = output if isinstance(output, dict) else {}
    riggable = bool(output.get("riggable"))
    rig_type = str(output.get("rig_type") or "").strip().lower() or "(none)"

    print()
    print(f"Riggable: {riggable}")
    print(f"Recommended rig_type: {rig_type}")
    if rig_type not in _RIG_TYPES and riggable:
        print(f"(Unexpected rig_type; known values: {', '.join(_RIG_TYPES)})")
    print(f"Rig-check task ID: {task_id}")
    return 0 if riggable else 2


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except TripoError as exc:
        fail(str(exc))
