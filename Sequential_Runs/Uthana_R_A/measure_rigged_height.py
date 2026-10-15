#!/usr/bin/env python3
"""Measure the world-space height of a Uthana-rigged character GLB.

Edit the USER CONFIGURATION section (or pass a path on the command line),
then run this file. Offline only -- no Uthana API calls.

Prints the bind (raw POSITION) span, the world mesh AABB height in metres,
the skeleton toe-to-head height, and the height to feed into the Uthana
``move_speed`` calculation (walk = 0.78 x height, run = 2.2 x height, the
engine's own factors).

The measurement logic is the pipeline's own
(``ptv_orchestrator.pipeline.uthana.glb_measure``), so this script and the
``uthana_motion_plan`` stage always agree.
"""

from __future__ import annotations

import sys
from pathlib import Path

_SCRIPT_DIR = Path(__file__).resolve().parent
_REPO = _SCRIPT_DIR.parent.parent
sys.path.insert(0, str(_REPO / "apps" / "orchestrator"))

# ============================================================
# USER CONFIGURATION
# ============================================================

GLB_PATH = _SCRIPT_DIR / "rigged_character.glb"

# The asset's target_scale_meters from asset_manifest.json, or None.
EXPECTED_TARGET_SCALE_METERS: float | None = 1.8

# ============================================================
# IMPLEMENTATION
# ============================================================


def main() -> int:
    from ptv_orchestrator.pipeline.uthana.glb_measure import main as measure_main

    argv = sys.argv[1:]
    if not argv:
        argv = [str(GLB_PATH)]
        if EXPECTED_TARGET_SCALE_METERS is not None:
            argv += ["--expected-target", str(EXPECTED_TARGET_SCALE_METERS)]
    return measure_main(argv)


if __name__ == "__main__":
    raise SystemExit(main())
