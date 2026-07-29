"""Tests for the device-aware performance budget policy.

These lock in the guarantees the pipeline depends on: tiers escalate with
content volume, small scenes are never distance-culled, every asset keeps a
usable triangle floor no matter how large the project gets, and PC VR gets
substantially more headroom than Quest.
"""

from __future__ import annotations

from ptv_schemas import (
    AssetBudgetRequest,
    AssetImportance,
    PerformanceTier,
    TargetDevice,
    allocate_asset_budgets,
    classify_tier,
    derive_profile,
)
from ptv_schemas.performance import UNBOUNDED_CULL_M


def test_tier_escalates_with_content_volume():
    assert classify_tier(5, 20) == PerformanceTier.compact
    assert classify_tier(20, 80) == PerformanceTier.standard
    assert classify_tier(40, 200) == PerformanceTier.large
    assert classify_tier(48, 600) == PerformanceTier.massive


def test_heavier_of_the_two_signals_wins():
    # Few unique assets but placed hundreds of times is still a big scene.
    assert classify_tier(4, 400) == PerformanceTier.massive


def test_small_scenes_are_not_distance_culled():
    """A compact game must still render its whole environment."""
    for count in [(5, 20), (20, 80)]:
        profile = derive_profile(
            target_device=TargetDevice.quest,
            asset_count=count[0],
            mesh_entity_count=count[1],
        )
        assert profile.cull_distance_m >= UNBOUNDED_CULL_M


def test_large_scenes_enable_culling_and_foveation():
    profile = derive_profile(
        target_device=TargetDevice.quest, asset_count=40, mesh_entity_count=200
    )
    assert profile.cull_distance_m < UNBOUNDED_CULL_M
    assert profile.foveation_level >= 2
    assert profile.msaa_samples <= 2


def test_massive_scene_enables_eviction():
    profile = derive_profile(
        target_device=TargetDevice.quest, asset_count=48, mesh_entity_count=900
    )
    assert profile.evict_when_over_budget is True


def test_frame_budget_stays_within_meta_guidance():
    """Meta publishes 1.3M-1.8M tris/frame for Quest 3; stay at/below it."""
    for entities in (20, 80, 200, 900):
        profile = derive_profile(
            target_device=TargetDevice.quest,
            asset_count=24,
            mesh_entity_count=entities,
        )
        assert profile.frame_triangle_budget <= 1_300_000
        assert profile.draw_call_budget <= 300


def test_pcvr_gets_more_headroom_than_quest():
    quest = derive_profile(
        target_device=TargetDevice.quest, asset_count=40, mesh_entity_count=200
    )
    pcvr = derive_profile(
        target_device=TargetDevice.pcvr, asset_count=40, mesh_entity_count=200
    )
    assert pcvr.frame_triangle_budget > quest.frame_triangle_budget
    assert pcvr.foveation_level == 0
    assert pcvr.evict_when_over_budget is False


def test_every_asset_keeps_a_triangle_floor_in_huge_projects():
    """No asset count may starve an asset below the quality floor."""
    profile = derive_profile(
        target_device=TargetDevice.quest, asset_count=48, mesh_entity_count=900
    )
    requests = {
        f"asset_{i}": AssetBudgetRequest(
            importance=AssetImportance.background, target_scale_meters=0.3
        )
        for i in range(48)
    }
    budgets = allocate_asset_budgets(profile, requests)
    assert len(budgets) == 48
    assert all(v >= profile.min_triangles_per_asset for v in budgets.values())
    assert all(v <= profile.max_triangles_per_asset for v in budgets.values())


def test_hero_assets_outrank_background_assets_even_with_spare_budget():
    """Importance must matter regardless of how much budget is available."""
    profile = derive_profile(
        target_device=TargetDevice.quest, asset_count=40, mesh_entity_count=200
    )
    requests = {
        "hero": AssetBudgetRequest(
            importance=AssetImportance.hero, target_scale_meters=2.0
        ),
        "background": AssetBudgetRequest(
            importance=AssetImportance.background, target_scale_meters=2.0
        ),
    }
    budgets = allocate_asset_budgets(profile, requests)
    assert budgets["hero"] > budgets["background"]


def test_never_upsamples_past_the_source_mesh():
    """A 500-triangle prop must stay 500 triangles, not be inflated."""
    profile = derive_profile(
        target_device=TargetDevice.quest, asset_count=3, mesh_entity_count=5
    )
    requests = {
        "tiny": AssetBudgetRequest(
            importance=AssetImportance.hero, source_triangles=500
        )
    }
    assert allocate_asset_budgets(profile, requests)["tiny"] == 500


def test_explicit_max_triangles_override_is_respected():
    profile = derive_profile(
        target_device=TargetDevice.quest, asset_count=3, mesh_entity_count=5
    )
    requests = {
        "capped": AssetBudgetRequest(
            importance=AssetImportance.hero, max_triangles=2500
        )
    }
    assert allocate_asset_budgets(profile, requests)["capped"] == 2500


def test_per_asset_budget_shrinks_as_project_grows():
    """The same hero asset gets less detail in a massive project."""

    def hero_budget(assets: int, entities: int) -> int:
        profile = derive_profile(
            target_device=TargetDevice.quest,
            asset_count=assets,
            mesh_entity_count=entities,
        )
        requests = {
            f"a{i}": AssetBudgetRequest(importance=AssetImportance.standard)
            for i in range(assets)
        }
        requests["hero"] = AssetBudgetRequest(importance=AssetImportance.hero)
        return allocate_asset_budgets(profile, requests)["hero"]

    assert hero_budget(5, 20) > hero_budget(40, 200) > hero_budget(48, 900)


def test_allocator_handles_empty_input():
    profile = derive_profile(
        target_device=TargetDevice.quest, asset_count=1, mesh_entity_count=1
    )
    assert allocate_asset_budgets(profile, {}) == {}
