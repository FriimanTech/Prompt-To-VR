#!/usr/bin/env python3
"""Standalone Tripo V3 Animation Retarget.

``input`` MUST be the task_id of a successful Auto Rig job (starts with
``task_``). A raw local GLB cannot be sent here — re-run auto_rig.py if you
lost the id.

Edit the USER CONFIGURATION section, then run this file.
"""

from __future__ import annotations

import sys
from pathlib import Path
from typing import Any

_SCRIPT_DIR = Path(__file__).resolve().parent
if str(_SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(_SCRIPT_DIR))

from tripo_common import (
    TripoError,
    create_task,
    download_task_models,
    fail,
    poll_task,
)

# ============================================================
# USER CONFIGURATION
# ============================================================
#
# API key is loaded from TRIPO_API_KEY in the repo-root .env
# (or the process environment). Do not paste the key here.

# Task ID printed by auto_rig.py after a successful rig. Not a file path.
RIGGED_MODEL_TASK_ID = "task_xxxxxxxxx"

OUTPUT_DIRECTORY = Path(r"path\to\output\folder")

# Set exactly one of these. If ANIMATIONS is non-empty it is sent as
# ``animations`` and ANIMATION is ignored. Do not send both in one request.
#
# v2.5-20260210 biped:
#   preset:idle, preset:walk, preset:run, preset:dive, preset:climb,
#   preset:jump, preset:slash, preset:shoot, preset:hurt, preset:fall,
#   preset:turn
# v1.0-20240301 biped (90+):  preset:biped:idle, preset:biped:walk, ...
# Creatures (v2.5): preset:quadruped:walk, preset:hexapod:walk,
#   preset:octopod:walk, preset:serpentine:march, preset:aquatic:march
ANIMATION = "preset:idle"
ANIMATIONS: list[str] = []
# Example multi-clip request (uncomment and leave ANIMATION unused):
# ANIMATIONS = [
#     "preset:idle",
#     "preset:walk",
#     "preset:run",
# ]

OUTPUT_FORMAT = "glb"  # glb | fbx
BAKE_ANIMATION = True  # only effective for glb
EXPORT_WITH_GEOMETRY = True
ANIMATE_IN_PLACE = False  # Tripo default. True = no baked root motion.

# ============================================================
# IMPLEMENTATION
# ============================================================


def animation_payload() -> dict[str, Any]:
    clips = [item.strip() for item in ANIMATIONS if item and item.strip()]
    single = (ANIMATION or "").strip()
    if clips and single:
        print(
            "Note: ANIMATION and ANIMATIONS are mutually exclusive. "
            "Sending ANIMATIONS and ignoring ANIMATION."
        )
    if clips:
        return {"animations": clips}
    if single:
        return {"animation": single}
    fail("Set ANIMATION to one preset, or ANIMATIONS to a non-empty list.")


def main() -> int:
    task_id = (RIGGED_MODEL_TASK_ID or "").strip()
    if not task_id or task_id == "task_xxxxxxxxx":
        fail(
            "Set RIGGED_MODEL_TASK_ID to the task_id printed by auto_rig.py. "
            "Animation Retarget does not accept a local GLB path."
        )
    if not task_id.startswith("task_"):
        fail(
            f"RIGGED_MODEL_TASK_ID should start with 'task_', got {task_id!r}. "
            "Use the Auto Rig task id, not a file_token or a generation task."
        )

    body: dict[str, Any] = {
        "input": task_id,
        "out_format": OUTPUT_FORMAT,
        "bake_animation": BAKE_ANIMATION,
        "export_with_geometry": EXPORT_WITH_GEOMETRY,
        "animate_in_place": ANIMATE_IN_PLACE,
    }
    body.update(animation_payload())

    which = body.get("animations") or body.get("animation")
    print(f"Retargeting rig task {task_id}")
    print(f"Animation(s): {which}")

    retarget_id = create_task("/animations/retarget", body)
    task = poll_task(retarget_id)

    clips = body.get("animations")
    if isinstance(clips, list) and len(clips) > 1:
        stem = "retargeted"
    else:
        label = str(body.get("animation") or "animation")
        stem = "retargeted_" + label.replace(":", "_")

    download_task_models(task, OUTPUT_DIRECTORY, filename_stem=stem)

    print()
    print(f"Retarget task ID: {retarget_id}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except TripoError as exc:
        fail(str(exc))
