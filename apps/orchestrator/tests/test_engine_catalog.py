"""The catalog is the only thing the mechanic planner can see.

A built-in that exists in C++ but not here is invisible to the planner, so it
never gets used and the pipeline regenerates something the engine already
ships. The reverse is worse: an id here with no C++ behind it produces a scene
binding that logs "no mechanic registered" and silently does nothing.

So these tests pin the catalog against the registrations in the engine source
rather than against a hand-maintained list, which is the only version that
cannot drift.
"""

from __future__ import annotations

import re
from pathlib import Path

from ptv_orchestrator.engine_catalog import all_known_ids, builtin_ids, entry
from ptv_schemas.mechanic_plan import BUILTIN_MECHANIC_IDS

REPO_ROOT = Path(__file__).resolve().parents[3]
BUILTIN_SRC = REPO_ROOT / "engine" / "runtime" / "src" / "mechanics" / "builtin"

_REGISTER = re.compile(r'PTV_REGISTER_MECHANIC\(\s*"([^"]+)"')


def _registered_in_cpp() -> set[str]:
    found: set[str] = set()
    for path in BUILTIN_SRC.rglob("*.cpp"):
        found.update(_REGISTER.findall(path.read_text(encoding="utf-8")))
    return found


def test_every_cpp_builtin_is_in_the_planner_catalog():
    registered = _registered_in_cpp()
    assert registered, "found no PTV_REGISTER_MECHANIC calls at all"
    known = all_known_ids()
    missing = sorted(registered - known)
    assert not missing, (
        "these built-ins are registered in C++ but invisible to the mechanic "
        f"planner: {missing}"
    )


def test_no_catalog_entry_without_cpp_behind_it():
    registered = _registered_in_cpp()
    # Canonical ids only: aliases are deliberately not separate registrations.
    orphans = sorted(set(builtin_ids()) - registered)
    assert not orphans, (
        "these catalog ids have no PTV_REGISTER_MECHANIC in the engine, so a "
        f"scene binding one would silently do nothing: {orphans}"
    )


def test_schema_id_set_matches_the_catalog():
    # Two lists of the same thing drift; this is the guard. The schema set is
    # canonical ids plus the legacy short aliases scenes may still carry.
    assert set(builtin_ids()) <= BUILTIN_MECHANIC_IDS


def test_new_builtins_document_their_params():
    # A catalog entry with no params tells the planner nothing, and it will
    # then pick "generate" over reuse.
    for mechanic_id in ("builtin.equip", "builtin.avatar_body",
                        "builtin.mirror_surface"):
        found = entry(mechanic_id)
        assert found is not None, f"{mechanic_id} missing from the catalog"
        assert found.params, f"{mechanic_id} documents no params"
        assert found.summary


def test_equip_documents_independent_equip_and_unequip():
    # The single most important thing about equip's contract: the two
    # triggers are independent, which is what makes equip-only and
    # unequip-only games pure configuration instead of new code.
    equip = entry("builtin.equip")
    assert equip is not None
    assert "equip_on" in equip.params
    assert "unequip_on" in equip.params
    # And the flag split that replaced the old overloaded "lock".
    assert "passive_grab_protection" in equip.params
    assert "removable" in equip.params
    assert "re_equippable" in equip.params
    assert "lock" not in equip.params
