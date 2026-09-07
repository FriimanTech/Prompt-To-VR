"""Entity ids emitted by world generation must be stable across a rebuild.

This is the highest-severity correctness property in the multi-environment
work, and it used to be silently wrong. Ids were a positional counter
(``env_0``, ``env_1``, ...), so inserting a single ``spawn_primitive`` call
renumbered every entity after it. Since those ids are the keys saved state is
written under, a regenerated world would restore the wrong objects -- an open
door becoming an open window -- with nothing anywhere reporting a problem.

These tests pin the slug rules the host implements, so a refactor of
``base_entity`` cannot quietly reintroduce positional ids.
"""

from __future__ import annotations

import re

# Mirrors SceneBuilderContext::slugify + base_entity in
# engine/worldgen/src/WorldGen.cpp. Kept as a readable reference
# implementation so the intended contract is testable without a C++ build.


def slugify(name: str) -> str:
    out: list[str] = []
    last_us = False
    for ch in name:
        # ASCII alphanumerics only. Non-ASCII input is multi-byte UTF-8 in
        # the C++ host, where isalnum on those bytes is locale-dependent, so
        # letting it through would put raw bytes into an id that serves as
        # both a JSON key and a persistence key.
        if ch.isascii() and ch.isalnum():
            out.append(ch.lower())
            last_us = False
        elif out and not last_us:
            out.append("_")
            last_us = True
    return "".join(out).rstrip("_")


def assign_ids(names: list[str]) -> list[str]:
    counts: dict[str, int] = {}
    ids: list[str] = []
    for name in names:
        base = slugify(name) or "entity"
        counts[base] = counts.get(base, 0) + 1
        seen = counts[base]
        ids.append(base if seen == 1 else f"{base}_{seen}")
    return ids


ID_PATTERN = re.compile(r"^[a-z0-9][a-z0-9_]*$")


def test_ids_come_from_names_not_positions():
    ids = assign_ids(["Ground", "North Wall", "Front Door"])
    assert ids == ["ground", "north_wall", "front_door"]
    assert not any(i.startswith("env_") for i in ids)


def test_inserting_an_entity_does_not_renumber_the_others():
    # The exact failure the old scheme had. Every id present in the first
    # world must survive unchanged in the second.
    before = assign_ids(["Ground", "North Wall", "Front Door"])
    after = assign_ids(["Ground", "A New Crate", "North Wall", "Front Door"])
    for name, ident in zip(["Ground", "North Wall", "Front Door"], before):
        assert ident in after, (
            f"'{name}' changed id when an unrelated entity was inserted; "
            "saved state keyed on it would now point at the wrong object"
        )


def test_repeated_names_get_deterministic_suffixes():
    ids = assign_ids(["Wall", "Wall", "Wall"])
    assert ids == ["wall", "wall_2", "wall_3"]
    assert len(set(ids)) == 3


def test_the_first_occurrence_is_never_suffixed():
    # So the common case of one uniquely-named object reads cleanly, and an
    # author's chosen name is exactly what they get.
    assert assign_ids(["Front Door"]) == ["front_door"]


def test_ids_are_safe_identifiers():
    ids = assign_ids(
        ["Ground!!", "  spaced  out  ", "Door #3", "café-window", "___"]
    )
    for ident in ids:
        assert ID_PATTERN.match(ident), f"unsafe id: {ident!r}"


def test_an_unnameable_entity_still_gets_a_usable_id():
    # Punctuation-only and empty names must not collapse to "" and collide.
    ids = assign_ids(["", "!!!", "###"])
    assert all(i for i in ids)
    assert len(set(ids)) == 3


def test_names_differing_only_by_punctuation_do_not_silently_collide():
    # "Front Door" and "Front-Door" slug identically, which is correct, but
    # they must still end up with DISTINCT ids rather than one overwriting
    # the other's saved state.
    ids = assign_ids(["Front Door", "Front-Door"])
    assert ids[0] != ids[1]
    assert ids == ["front_door", "front_door_2"]
