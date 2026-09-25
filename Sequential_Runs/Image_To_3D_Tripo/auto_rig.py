#!/usr/bin/env python3
"""Standalone Tripo V3 Auto Rig.

Edit the USER CONFIGURATION section, then run this file.
Prints the rig task ID — you need that ID for animation_retarget.py.
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
    download_task_models,
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

INPUT_GLB_PATH = Path(r"path\to\model.glb")
OUTPUT_DIRECTORY = Path(r"path\to\output\folder")

# Docs recommend running rig-check first (free) so you do not spend ~30
# credits on a prop that cannot be rigged.
RUN_RIG_CHECK_FIRST = True
USE_RECOMMENDED_RIG_TYPE = True

# Used when RUN_RIG_CHECK_FIRST is False, or when the check returns no type.
#   biped | quadruped | hexapod | octopod | avian | serpentine | aquatic
RIG_TYPE = "biped"

# Leave None to pick the matching checkpoint from RIG_TYPE:
#   biped     -> v1.0-20240301  (90+ animation presets)
#   otherwise -> v2.5-20260210
RIG_MODEL: str | None = None

# "tripo" is required if you will call /animations/retarget next.
# "mixamo" is Unity/Unreal-friendly but Tripo retarget rejects it (error 1004).
SPEC = "tripo"

OUT_FORMAT = "glb"  # glb | fbx

# ============================================================
# IMPLEMENTATION
# ============================================================

_MODEL_MAX_BYTES = 150 * 1024 * 1024
_RIG_TYPES = {
    "biped",
    "quadruped",
    "hexapod",
    "octopod",
    "avian",
    "serpentine",
    "aquatic",
}


def rig_model_for_type(rig_type: str) -> str:
    key = rig_type.strip().lower()
    if key not in _RIG_TYPES:
        fail(f"Unknown RIG_TYPE {rig_type!r}. Expected one of: {', '.join(sorted(_RIG_TYPES))}.")
    return "v1.0-20240301" if key == "biped" else "v2.5-20260210"


def run_rig_check(file_token: str) -> tuple[bool, str]:
    print()
    print("Running rig-check first (free)...")
    task_id = create_task("/animations/rig-check", {"input": file_token})
    task = poll_task(task_id)
    output = task.get("output")
    output = output if isinstance(output, dict) else {}
    riggable = bool(output.get("riggable"))
    recommended = str(output.get("rig_type") or "").strip().lower()
    print(f"Riggable: {riggable}")
    print(f"Recommended rig_type: {recommended or '(none)'}")
    return riggable, recommended


def main() -> int:
    glb = require_file(INPUT_GLB_PATH, kind="Input GLB")
    if glb.suffix.lower() not in {".glb", ".gltf", ".fbx", ".obj", ".stl"}:
        fail(f"Unsupported model type {glb.suffix}. Use GLB, GLTF, FBX, OBJ, or STL.")
    size = glb.stat().st_size
    if size > _MODEL_MAX_BYTES:
        fail(f"Model is {size} bytes; Tripo model uploads must be <= 150 MB.")

    file_token = upload_file(glb, label="GLB")
    rig_type = RIG_TYPE.strip().lower()

    if RUN_RIG_CHECK_FIRST:
        riggable, recommended = run_rig_check(file_token)
        if not riggable:
            fail(
                "Tripo says this model is not riggable. Aborting auto-rig so "
                "you do not spend credits. Try a full character mesh."
            )
        if USE_RECOMMENDED_RIG_TYPE and recommended:
            rig_type = recommended
            print(f"Using recommended rig_type: {rig_type}")

    model = (RIG_MODEL or rig_model_for_type(rig_type)).strip()
    body = {
        "input": file_token,
        "model": model,
        "rig_type": rig_type,
        "spec": SPEC,
        "out_format": OUT_FORMAT,
    }

    print()
    print(f"Submitting auto-rig (model={model}, rig_type={rig_type}, spec={SPEC})...")
    task_id = create_task("/animations/rig", body)
    task = poll_task(task_id)

    download_task_models(
        task,
        OUTPUT_DIRECTORY,
        filename_stem=f"{glb.stem}_rigged",
    )

    print()
    print("=" * 60)
    print("RIG TASK ID (paste this into animation_retarget.py):")
    print(task_id)
    print("=" * 60)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except TripoError as exc:
        fail(str(exc))
