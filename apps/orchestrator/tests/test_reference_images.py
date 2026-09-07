"""The reference-image stage had zero test coverage before this file.

The decision it makes is not cosmetic: the pose baked into the reference
image is inherited by the 3D asset, and a character generated mid-stride is
effectively unusable outside this pipeline, because rigging, retargeting and
every external 3D tool expect a neutral spread.

The case that matters most is the one that is easy to get wrong in the
convenient direction: a duck. The SAME creature is a T-pose actor when it
chases the player and a natural-pose prop when it sits on a shelf, and
nothing about its category distinguishes them.
"""

from __future__ import annotations

import json

from ptv_orchestrator.pipeline.stage_reference_images import (
    _npc_asset_ids,
    compose_negative_prompt,
    compose_prompt,
    resolve_pose,
)
from ptv_schemas.asset_manifest import (
    AssetCategory,
    AssetItem,
    CharacterRole,
    ReferenceCoverage,
    ReferenceImageRequest,
    ReferencePose,
)


def _duck(role: CharacterRole = CharacterRole.none, **ref) -> AssetItem:
    return AssetItem(
        id="duck",
        name="Duck",
        category=AssetCategory.character,
        style_notes="Cartoon duck, soft shading.",
        character_role=role,
        reference=ReferenceImageRequest(prompt="a cartoon duck", **ref),
    )


# --- the pose decision -------------------------------------------------


def test_the_same_duck_is_t_posed_as_an_npc_and_natural_as_a_prop():
    actor = _duck(CharacterRole.npc)
    prop = _duck(CharacterRole.none)
    assert resolve_pose(actor, set()) == ReferencePose.t_pose
    assert resolve_pose(prop, set()) == ReferencePose.natural


def test_a_duck_named_in_the_npc_plan_is_t_posed_even_without_a_role():
    # Older manifests carry no character_role, so the NpcPlan is the other
    # source of truth and has to be honoured on its own.
    plain = _duck(CharacterRole.none)
    assert resolve_pose(plain, {"duck"}) == ReferencePose.t_pose


def test_the_player_avatar_is_t_posed():
    # NpcPlan deliberately ignores the player avatar -- it plans NON-player
    # characters -- so nothing but character_role would catch this.
    avatar = _duck(CharacterRole.player_avatar)
    assert resolve_pose(avatar, set()) == ReferencePose.t_pose


def test_a_quadruped_actor_keeps_its_shape_hint():
    # A T-pose is meaningless for something with four legs, so the agent's
    # shape hint survives the reconciliation.
    beast = _duck(CharacterRole.npc, pose=ReferencePose.neutral_quadruped)
    assert resolve_pose(beast, set()) == ReferencePose.neutral_quadruped


def test_a_quadruped_prop_is_still_natural():
    # The shape hint must not smuggle a character pose onto a prop.
    beast = _duck(CharacterRole.none, pose=ReferencePose.neutral_quadruped)
    assert resolve_pose(beast, set()) == ReferencePose.natural


def test_a_slide_only_npc_is_still_t_posed():
    # An NPC the game never animates still gets a T-pose: the point is
    # reusing the GLB in other 3D software, not this game's animation.
    slider = _duck(CharacterRole.npc)
    assert resolve_pose(slider, {"duck"}) == ReferencePose.t_pose


def test_props_are_never_posed():
    prop = AssetItem(
        id="crate",
        name="Crate",
        category=AssetCategory.prop,
        style_notes="Wooden crate.",
        reference=ReferenceImageRequest(prompt="a wooden crate"),
    )
    assert resolve_pose(prop, set()) == ReferencePose.natural


# --- prompt composition ------------------------------------------------


def test_the_t_pose_clause_is_appended_without_losing_art_direction():
    text = compose_prompt(_duck(CharacterRole.npc), ReferencePose.t_pose)
    assert "a cartoon duck" in text
    assert "Style: Cartoon duck, soft shading." in text
    assert "T-pose" in text
    assert "arms stretched straight out to the sides" in text.lower()


def test_a_prop_prompt_gains_no_pose_clause():
    text = compose_prompt(_duck(CharacterRole.none), ReferencePose.natural)
    assert "T-pose" not in text
    assert "orthographic" not in text


def test_the_quadruped_clause_differs_from_the_t_pose_clause():
    text = compose_prompt(
        _duck(CharacterRole.npc, pose=ReferencePose.neutral_quadruped),
        ReferencePose.neutral_quadruped,
    )
    assert "four legs" in text
    assert "T-pose" not in text


def test_neck_down_coverage_removes_the_head_and_says_so_in_the_negatives():
    item = _duck(
        CharacterRole.player_avatar, coverage=ReferenceCoverage.neck_down
    )
    text = compose_prompt(item, ReferencePose.t_pose)
    negatives = compose_negative_prompt(item, ReferencePose.t_pose)
    assert "feet up to the neck" in text
    assert "No head" in text
    assert negatives is not None and "head" in negatives


def test_coverage_is_ignored_for_a_prop():
    # A crate has no neck to cut at, so the clause must not leak onto it.
    item = _duck(CharacterRole.none, coverage=ReferenceCoverage.neck_down)
    assert "neck" not in compose_prompt(item, ReferencePose.natural)


def test_negatives_are_combined_once_and_keep_the_authors_own():
    item = _duck(CharacterRole.npc, negative_prompt="blurry, watermark")
    negatives = compose_negative_prompt(item, ReferencePose.t_pose)
    assert negatives is not None
    assert negatives.startswith("blurry, watermark")
    assert "A-pose" in negatives
    # Appended exactly once, not per clause.
    assert negatives.count("A-pose") == 1


def test_a_natural_pose_leaves_negatives_untouched():
    item = _duck(CharacterRole.none, negative_prompt="blurry")
    assert compose_negative_prompt(item, ReferencePose.natural) == "blurry"


def test_no_negatives_at_all_stays_none():
    # The provider client treats None as "no avoid-list"; an empty string
    # would fold a stray "Avoid in the image:" line into every prompt.
    assert compose_negative_prompt(_duck(), ReferencePose.natural) is None


# --- npc_plan reading --------------------------------------------------


def test_npc_ids_are_read_from_the_plan(tmp_path):
    class _Ctx:
        project_dir = tmp_path

    (tmp_path / "npc_plan.json").write_text(
        json.dumps({"npcs": [{"asset_id": "duck"}, {"asset_id": "alien"}]}),
        encoding="utf-8",
    )
    assert _npc_asset_ids(_Ctx()) == {"duck", "alien"}


def test_a_missing_or_broken_npc_plan_does_not_fail_the_stage(tmp_path):
    class _Ctx:
        project_dir = tmp_path

    # Missing: a game with no characters emits no plan at all.
    assert _npc_asset_ids(_Ctx()) == set()
    # Malformed: best-effort enrichment must never take the stage down.
    (tmp_path / "npc_plan.json").write_text("{not json", encoding="utf-8")
    assert _npc_asset_ids(_Ctx()) == set()
