"""The walk/run speed-per-metre factors must agree in all three places.

Uthana motions are generated at ``factor x height`` so that the engine's
playback-rate scaling (LocomotionClipSelector) and the scene compiler's
``animation_speeds`` describe the same clip. If one copy drifts, generated
gaits play too fast or too slow and the feet skate.
"""

from __future__ import annotations

import re
from pathlib import Path

from ptv_orchestrator.pipeline.stage_scene_compile import _SPEED_PER_METER_OF_HEIGHT
from ptv_providers.animation_catalog import RUN_SPEED_PER_M, WALK_SPEED_PER_M

_HEADER = (
    Path(__file__).resolve().parents[3]
    / "engine" / "runtime" / "src" / "mechanics" / "builtin" / "LocomotionClipSelector.h"
)


def _cpp_factor(name: str) -> float:
    text = _HEADER.read_text(encoding="utf-8")
    m = re.search(rf"{name}\s*=\s*([0-9.]+)f", text)
    assert m, f"{name} not found in {_HEADER}"
    return float(m.group(1))


def test_walk_and_run_factors_match_everywhere():
    assert _cpp_factor("kWalkSpeedPerMeterOfHeight") == WALK_SPEED_PER_M
    assert _cpp_factor("kRunSpeedPerMeterOfHeight") == RUN_SPEED_PER_M
    assert _SPEED_PER_METER_OF_HEIGHT["walk"] == WALK_SPEED_PER_M
    assert _SPEED_PER_METER_OF_HEIGHT["run"] == RUN_SPEED_PER_M
