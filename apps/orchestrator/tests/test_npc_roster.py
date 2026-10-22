"""The NPC roster decides how many unique NPC models a game gets."""

from __future__ import annotations

from ptv_orchestrator.pipeline.stage_asset_manifest import (
    missing_roster_assets,
    normalise_roster_items,
)
from ptv_orchestrator.pipeline.stage_build_spec import roster_violations
from ptv_schemas import AssetManifest, BuildSpec, NpcFamily


def _spec(roster, extra_assets=()):
    assets = [
        {"id": "laser_blaster", "name": "Blaster", "category": "player_weapon",
         "summary": "a blaster"},
    ] + [
        {"id": a, "name": a, "category": "enemy_character", "summary": a}
        for a in extra_assets
    ]
    return BuildSpec.model_validate({
        "prompt_id": "goblins", "title": "Goblins", "pitch": "p",
        "game_type": "shooter",
        "scenes": [{"id": "arena", "name": "Arena", "summary": "s"}],
        "mechanics": [{"id": "shoot", "name": "Shoot", "summary": "s"}],
        "assets": assets,
        "acceptance_tests": [{"id": "t", "description": "d"}],
        "npc_roster": roster,
    })


def _family(fid, reading, variants, **extra):
    return {
        "family_id": fid, "role": "enemy", "count_reading": reading,
        "variants": [{"asset_id": a, "name": a, "physique": p} for a, p in variants],
        **extra,
    }


def test_plural_unspecified_needs_exactly_three_distinct_variants() -> None:
    one = _family("goblins", "plural_unspecified", [("goblin_raider", "standard")])
    problems = roster_violations(_spec([one], ["goblin_raider"]))
    assert any("exactly 3 variant" in p for p in problems)

    three = _family("goblins", "plural_unspecified", [
        ("goblin_regular", "standard"), ("goblin_tall", "tall_lean"),
        ("goblin_brute", "big_bulky")])
    assert roster_violations(
        _spec([three], ["goblin_regular", "goblin_tall", "goblin_brute"])) == []


def test_variants_need_their_own_assets_and_different_bodies() -> None:
    fam = _family("goblins", "plural_unspecified", [
        ("goblin_a", "standard"), ("goblin_b", "standard"), ("goblin_c", "standard")])
    problems = roster_violations(_spec([fam], ["goblin_a", "goblin_b"]))
    assert any("goblin_c" in p and "no AssetEntry" in p for p in problems)
    assert any("share physique" in p for p in problems)


def test_singular_and_single_type_get_one_variant() -> None:
    ally = _family("lantern", "singular", [("lantern_ally", "standard")])
    zombies = _family("zombies", "single_type_requested", [("zombie", "standard")])
    assert roster_violations(_spec([ally, zombies], ["lantern_ally", "zombie"])) == []
    two = _family("lantern", "singular", [("a", "standard"), ("b", "tall_lean")])
    assert roster_violations(_spec([two], ["a", "b"]))


def test_explicit_counts() -> None:
    fam = NpcFamily.model_validate(_family(
        "drones", "explicit_types", [("d1", "standard")], explicit_count=4))
    assert fam.required_variant_count() == 4
    fam = NpcFamily.model_validate(_family(
        "goblins", "explicit_number", [("g", "standard")], instance_hint=2))
    assert fam.required_variant_count() == 2
    fam = NpcFamily.model_validate(_family(
        "goblins", "explicit_number", [("g", "standard")], instance_hint=9))
    assert fam.required_variant_count() == 3


def _plan(npcs, encounters=None):
    from ptv_schemas import NpcPlan
    return NpcPlan.model_validate({
        "prompt_id": "goblins",
        "npcs": npcs,
        **({"encounters": encounters} if encounters else {}),
    })


def _npc(asset_id, role="enemy", **extra):
    return {"asset_id": asset_id, "role": role, **extra}


def test_npc_plan_population_rules_and_repairs() -> None:
    from ptv_orchestrator.pipeline.npc_population import (
        npc_plan_violations,
        repair_npc_plan,
    )
    spec = _spec([
        _family("goblins", "plural_unspecified", [
            ("goblin_regular", "standard"), ("goblin_tall", "tall_lean"),
            ("goblin_brute", "big_bulky")]),
        {"family_id": "lantern", "role": "ally", "count_reading": "singular",
         "variants": [{"asset_id": "lantern_ally", "name": "Lantern"}]},
    ], ["goblin_regular", "goblin_tall", "goblin_brute", "lantern_ally"])

    plan = _plan(
        [_npc("goblin_regular", spawn={"waves": True}),
         _npc("goblin_tall", spawn={"waves": True}),
         _npc("lantern_ally", role="ally")],
        {"waves": [{"groups": [{"asset_id": "goblin_regular", "count": 3},
                               {"asset_id": "lantern_ally", "count": 1}]}],
         "max_concurrent": 30},
    )
    problems = npc_plan_violations(plan, spec)
    assert any("goblin_brute" in p for p in problems)
    assert any("lantern_ally" in p and "companion" in p for p in problems)

    fixed, notes = repair_npc_plan(plan, spec)
    ally = next(n for n in fixed.npcs if n.asset_id == "lantern_ally")
    assert ally.persistent_count == 1
    assert "lantern_ally" not in fixed.spawned_asset_ids()
    assert fixed.encounters.max_concurrent == 11  # 12 budget - 1 companion
    assert notes


def test_default_progression_when_waves_were_left_out() -> None:
    from ptv_orchestrator.pipeline.npc_population import repair_npc_plan
    spec = _spec([_family("goblins", "plural_unspecified", [
        ("goblin_brute", "big_bulky"), ("goblin_regular", "standard"),
        ("goblin_tall", "tall_lean")])],
        ["goblin_regular", "goblin_tall", "goblin_brute"])
    plan = _plan([
        _npc("goblin_brute", spawn={"waves": True}),
        _npc("goblin_regular", spawn={"waves": True, "initial_count": 3}),
        _npc("goblin_tall", spawn={"waves": True}),
    ])
    fixed, _ = repair_npc_plan(plan, spec)
    waves = fixed.encounters.waves
    assert [g.asset_id for g in waves[0].groups] == ["goblin_regular"]
    assert waves[-1].groups[0].asset_id == "goblin_brute"
    assert set(fixed.spawned_asset_ids()) == {"goblin_regular", "goblin_tall", "goblin_brute"}


def test_manifest_coverage_and_normalisation() -> None:
    spec = _spec([_family("goblins", "plural_unspecified", [
        ("goblin_regular", "standard"), ("goblin_tall", "tall_lean"),
        ("goblin_brute", "big_bulky")])],
        ["goblin_regular", "goblin_tall", "goblin_brute"])
    manifest = AssetManifest.model_validate({
        "prompt_id": "goblins",
        "items": [
            {"id": "goblin_regular", "name": "g", "category": "character",
             "character_role": "npc", "style_notes": "s", "reference": {"prompt": "p"}},
            {"id": "goblin_tall", "name": "t", "category": "prop",
             "style_notes": "s", "reference": {"prompt": "p"}},
        ],
    })
    assert missing_roster_assets(manifest, spec) == ["goblin_brute"]
    fixed_manifest, fixed = normalise_roster_items(manifest, spec)
    assert fixed == ["goblin_tall"]
    tall = next(i for i in fixed_manifest.items if i.id == "goblin_tall")
    assert tall.category.value == "character" and tall.character_role.value == "npc"
