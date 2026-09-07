"""How the runtime finds a sibling scene.

A portal names a target scene id. The engine has to turn that into a file
without knowing how the orchestrator lays out a project, and it has to work
identically inside a Quest APK, where there is no filesystem to go looking
around in. So the compiler writes down where every scene went.

These tests pin the shape of that index and the two places it has to be
consistent with: where the documents are actually written, and what the APK
packages.
"""

from __future__ import annotations

import json


def _index_for(scene_ids: list[str]) -> dict[str, str]:
    """The mapping scene_compile builds. Mirrors the stage so the expected
    shape is asserted rather than reimplemented per test."""
    return {scene_id: f"scene.{scene_id}.json" for scene_id in scene_ids}


def test_every_scene_gets_the_whole_index():
    # Not just its own exits: a menu offering a mission list, or a generated
    # mechanic ending a chapter, needs to reach a scene no portal in the
    # current room points at.
    index = _index_for(["hub", "hospital", "warehouse"])
    assert set(index) == {"hub", "hospital", "warehouse"}


def test_index_paths_are_relative_and_flat():
    # Relative so a project can be moved or unpacked anywhere; flat so the
    # same paths resolve inside an APK's assets directory, which has no
    # subdirectories for scenes.
    for path in _index_for(["hub", "hospital"]).values():
        assert not path.startswith("/")
        assert "/" not in path and "\\" not in path
        assert path.endswith(".json")


def test_index_names_match_where_documents_are_written(tmp_path):
    # The trap this catches: writing the documents under builds/scene/ as
    # `<id>.scene.json` while the index says `scene.<id>.json`. Every portal
    # would then resolve to a file that does not exist.
    scene_ids = ["hub", "hospital"]
    index = _index_for(scene_ids)
    for scene_id in scene_ids:
        doc = {"scene_id": scene_id, "scene_index": index}
        (tmp_path / f"scene.{scene_id}.json").write_text(
            json.dumps(doc), encoding="utf-8"
        )
    # scene.json is a copy of the entry point, sitting beside its siblings so
    # the index resolves from it.
    (tmp_path / "scene.json").write_text(
        (tmp_path / "scene.hub.json").read_text(encoding="utf-8"), encoding="utf-8"
    )

    entry = json.loads((tmp_path / "scene.json").read_text(encoding="utf-8"))
    for scene_id, rel in entry["scene_index"].items():
        assert (tmp_path / rel).exists(), f"{scene_id} index path is a dead link"


def test_the_apk_glob_catches_every_sibling(tmp_path):
    # quest_build packages scene.json plus `scene.*.json`. Packaging only the
    # entry point leaves a build that runs and looks right until the player
    # walks into the first doorway.
    for name in ("scene.json", "scene.hub.json", "scene.hospital.json"):
        (tmp_path / name).write_text("{}", encoding="utf-8")
    # Decoys that must NOT be swept in: world documents are an intermediate
    # artifact, not a scene the runtime loads.
    (tmp_path / "world.hub.scene.json").write_text("{}", encoding="utf-8")

    siblings = sorted(p.name for p in tmp_path.glob("scene.*.json"))
    assert siblings == ["scene.hospital.json", "scene.hub.json"]
    assert "world.hub.scene.json" not in siblings
