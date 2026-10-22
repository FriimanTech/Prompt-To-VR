"""NPC population compiled into the scene: zones, templates, encounters."""

from __future__ import annotations

import math

from ptv_orchestrator.pipeline.encounters import fallback_spawn_zones


def test_fallback_zones_are_far_from_the_player_and_spread_out() -> None:
    regions = [
        {"id": "arrival", "min": [-8, 0, -37], "max": [8, 0, -18], "floor_y": 0.0},
        {"id": "yard", "min": [-15, 0, -10], "max": [15, 0, 15], "floor_y": 0.0},
        {"id": "north", "min": [-10, 0, 18], "max": [10, 0, 30], "floor_y": 0.5},
    ]
    spawn = [2.2, 0.05, -30.0]
    zones = fallback_spawn_zones(regions, spawn)
    assert 3 <= len(zones) <= 6
    for z in zones:
        assert math.hypot(z["position"][0] - spawn[0], z["position"][2] - spawn[2]) >= 12.0
    for i, a in enumerate(zones):
        for b in zones[i + 1:]:
            assert math.hypot(a["position"][0] - b["position"][0],
                              a["position"][2] - b["position"][2]) >= 8.0
    north = [z for z in zones if "north" in z["id"]]
    assert north and north[0]["position"][1] == 0.5


def test_no_regions_means_no_zones() -> None:
    assert fallback_spawn_zones([], [0, 0, 0]) == []
