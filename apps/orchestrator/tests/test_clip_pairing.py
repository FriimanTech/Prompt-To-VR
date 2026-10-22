"""Tripo clips are paired to intents by NAME, not by the order they came back."""

from __future__ import annotations

from ptv_orchestrator.pipeline.stage_asset_animation import (
    _name_key,
    _pair_intents_to_clips,
)


def test_goblin_clips_pair_by_name_not_position() -> None:
    # Requested run, slash, hurt, fall; Tripo returned them alphabetically.
    presets = ["preset:biped:run", "preset:biped:slash",
               "preset:biped:hurt", "preset:biped:fall"]
    mapping = {
        "run": "preset:biped:run",
        "attack": "preset:biped:slash",
        "hurt": "preset:biped:hurt",
        "die": "preset:biped:fall",
    }
    out = _pair_intents_to_clips(mapping, presets, ["fall", "hurt", "run", "slash"])
    assert out == {"run": "run", "attack": "slash", "hurt": "hurt", "die": "fall"}


def test_armature_style_names_still_match() -> None:
    out = _pair_intents_to_clips(
        {"walk": "preset:biped:walk", "idle": "preset:biped:idle"},
        ["preset:biped:walk", "preset:biped:idle"],
        ["Armature|preset_idle", "Armature|preset_walk"],
    )
    assert out == {"walk": "Armature|preset_walk", "idle": "Armature|preset_idle"}


def test_unnamed_clips_fall_back_to_order() -> None:
    out = _pair_intents_to_clips(
        {"walk": "preset:quadruped:walk"},
        ["preset:quadruped:walk"],
        ["animation_0"],
    )
    assert out == {"walk": "animation_0"}


def test_missing_clip_is_left_out() -> None:
    out = _pair_intents_to_clips(
        {"run": "preset:biped:run", "attack": "preset:biped:slash"},
        ["preset:biped:run", "preset:biped:slash"],
        ["run"],
    )
    assert out == {"run": "run"}


def test_name_key_normalises() -> None:
    assert _name_key("preset:biped:hit_to_head") == "hittohead"
    assert _name_key("Armature|preset_Hit_To_Head") == "hittohead"
