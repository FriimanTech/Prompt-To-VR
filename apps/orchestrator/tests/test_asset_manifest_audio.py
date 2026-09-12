"""The manifest planned three gunshots as meshes.

`pistol_shot_audio`, `rifle_shot_audio` and `target_impact_audio` were real
items in a real run: each one got a reference image of a muzzle flash, a paid
Tripo image-to-3D call, and a silent blob in the scene. The prompt now forbids
sound entries, but a prompt rule is advice and the fallback model may not take
it, so the drop is deterministic.

The interesting half of the rule is what it must NOT drop. A loudspeaker, an
alarm bell and a klaxon are props with silhouettes, and a wrong drop deletes
geometry the game needs -- strictly worse than a wasted call. So the matcher
is whole-token, and it stands down whenever the item also names a physical
sound-making object.
"""

from __future__ import annotations

from pathlib import Path
from types import SimpleNamespace

from ptv_orchestrator.events import EventBus
from ptv_orchestrator.pipeline import stage_asset_manifest
from ptv_orchestrator.pipeline.context import PipelineContext
from ptv_orchestrator.pipeline.stage_asset_manifest import (
    _partition,
    is_audio_item,
)
from ptv_orchestrator.settings import Settings
from ptv_orchestrator.storage import ProjectStore
from ptv_schemas import (
    AssetEntry,
    AssetManifest,
    BuildSpec,
    GameType,
    MechanicEntry,
    SceneEntry,
    TestEntry,
)
from ptv_schemas.asset_manifest import (
    AssetCategory,
    AssetItem,
    ReferenceImageRequest,
)


def _item(item_id: str, name: str, category: str = "fx") -> AssetItem:
    return AssetItem(
        id=item_id,
        name=name,
        category=AssetCategory(category),
        style_notes="whatever",
        reference=ReferenceImageRequest(prompt="a thing"),
    )


# --- what must be dropped ----------------------------------------------


def test_the_three_real_audio_items_are_dropped():
    for item_id, name in (
        ("pistol_shot_audio", "Pistol Shot Audio"),
        ("rifle_shot_audio", "Rifle Shot Audio"),
        ("target_impact_audio", "Target Impact Audio"),
    ):
        assert is_audio_item(_item(item_id, name)), item_id


def test_audio_is_dropped_whatever_category_it_hides_under():
    # The alien-shooter run filed nine of these as `ui`, which is how they
    # got past a category-only check.
    for category in ("fx", "ui", "prop", "character"):
        assert is_audio_item(_item("audio_ui_click", "Audio: UI Click", category))


def test_sound_packs_and_music_are_dropped():
    assert is_audio_item(_item("audio_sfx_music_pack", "Audio SFX and Music Pack"))
    assert is_audio_item(_item("combat_music_loop", "Combat Music Loop"))
    assert is_audio_item(_item("wind_ambience", "Alien Wind Ambience"))
    assert is_audio_item(_item("alien_vox", "Alien Vox Calls"))


# --- what must survive --------------------------------------------------


def test_a_thing_that_makes_a_sound_is_still_an_asset():
    # Each of these names a sound AND a shape. Keeping them is the point.
    assert not is_audio_item(_item("pa_speaker", "Range PA Speaker", "prop"))
    assert not is_audio_item(_item("alarm_bell", "Alarm Bell", "prop"))
    assert not is_audio_item(_item("radio_sfx_unit", "Radio Sound Unit", "prop"))


def test_near_miss_words_do_not_trigger_the_drop():
    # "stinger" is a missile, "ambient" is lighting, "soundproof" is a wall,
    # and none of them are excuses to delete an asset.
    assert not is_audio_item(_item("stinger_missile", "Stinger Missile", "prop"))
    assert not is_audio_item(_item("ambient_light_rig", "Ambient Light Rig", "prop"))
    assert not is_audio_item(_item("soundproof_panel", "Soundproof Wall Panel", "prop"))
    assert not is_audio_item(_item("audiotorium", "Audiotorium Seats", "prop"))


def test_ordinary_assets_are_untouched():
    for item_id, name in (
        ("range_pistol", "Range Pistol"),
        ("muzzle_flash_fx", "Quest-Optimized Muzzle Flash"),
        ("wrist_hud", "Left-Wrist Range HUD"),
    ):
        assert not is_audio_item(_item(item_id, name)), item_id


# --- the partition the stage actually applies ---------------------------


def test_partition_splits_environment_and_audio_apart():
    items = [
        _item("range_pistol", "Range Pistol", "prop"),
        _item("the_range", "Indoor Range", "environment"),
        _item("pistol_shot_audio", "Pistol Shot Audio"),
    ]
    kept, environment, audio = _partition(items)
    assert [i.id for i in kept] == ["range_pistol"]
    assert [i.id for i in environment] == ["the_range"]
    assert [i.id for i in audio] == ["pistol_shot_audio"]


def test_an_environment_item_is_never_double_counted_as_audio():
    # Category wins first, so "Ambience Dome" filed as environment lands in
    # exactly one bucket and the stage message stays honest.
    kept, environment, audio = _partition(
        [
            _item("keep_me", "Crate", "prop"),
            _item("ambience_dome", "Ambience Dome", "environment"),
        ]
    )
    assert [i.id for i in environment] == ["ambience_dome"]
    assert audio == []
    assert [i.id for i in kept] == ["keep_me"]


# --- the stage ----------------------------------------------------------


def _build_spec() -> BuildSpec:
    return BuildSpec(
        prompt_id="range-ready",
        title="Range Ready",
        pitch="A shooting range you can actually run around in.",
        game_type=GameType.shooter,
        scenes=[
            SceneEntry(
                id="shooting_range",
                name="Shooting Range",
                summary="An outdoor range with lanes and paper targets.",
                entry_point=True,
            )
        ],
        mechanics=[
            MechanicEntry(
                id="shoot", name="Shoot", summary="Right trigger fires."
            )
        ],
        assets=[
            AssetEntry(
                id="pistol", name="Pistol", category="prop", summary="A pistol."
            )
        ],
        acceptance_tests=[TestEntry(id="t1", description="the pistol fires")],
    )


class _ListBus(EventBus):
    def __init__(self) -> None:
        super().__init__()
        self.events: list[tuple[str, str]] = []

    def publish(self, event) -> None:  # type: ignore[override]
        self.events.append((getattr(event, "status", ""), getattr(event, "message", "")))


def _run_stage(tmp_path: Path, monkeypatch, returns: AssetManifest):
    settings = Settings(repo_root=tmp_path, projects_dir=tmp_path / "projects")
    store = ProjectStore(settings.projects_dir)
    state = store.create("Range", "a prompt long enough to pass validation")
    bus = _ListBus()
    ctx = PipelineContext(
        project_id=state.project_id, settings=settings, store=store, bus=bus
    )
    ctx.build_spec = _build_spec()

    class _Fake:
        def generate_json(self, **kwargs):
            return SimpleNamespace(data=returns.model_dump(mode="json"))

    monkeypatch.setattr(stage_asset_manifest, "make_llm", lambda _s: _Fake())
    result = stage_asset_manifest.run(ctx)
    return store, state.project_id, bus, result


def test_the_stage_drops_audio_before_anything_is_billed(tmp_path, monkeypatch):
    # Nothing downstream re-checks this: whatever survives here gets a
    # reference image and a Tripo call.
    planned = AssetManifest(
        prompt_id="range-ready",
        items=[
            _item("range_pistol", "Range Pistol", "prop"),
            _item("pistol_shot_audio", "Pistol Shot Audio"),
            _item("target_impact_audio", "Target Impact Audio"),
        ],
    )
    store, pid, bus, result = _run_stage(tmp_path, monkeypatch, planned)

    saved = store.load_asset_manifest(pid)
    assert [i.id for i in saved.items] == ["range_pistol"]
    assert [i.id for i in result.data.items] == ["range_pistol"]
    # And it says so, naming names, rather than silently shrinking.
    assert any("pistol_shot_audio" in m for _, m in bus.events)


def test_an_all_audio_manifest_is_kept_rather_than_emptied(tmp_path, monkeypatch):
    # AssetManifest requires >=1 item, so dropping everything would turn a
    # wasted call into a crashed pipeline. Wrong in the recoverable
    # direction on purpose.
    planned = AssetManifest(
        prompt_id="range-ready",
        items=[_item("pistol_shot_audio", "Pistol Shot Audio")],
    )
    store, pid, _bus, _result = _run_stage(tmp_path, monkeypatch, planned)
    assert [i.id for i in store.load_asset_manifest(pid).items] == [
        "pistol_shot_audio"
    ]
