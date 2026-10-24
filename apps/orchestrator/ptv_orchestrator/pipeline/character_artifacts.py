"""Load/save the per-project character artifacts shared by several stages.

* ``character_routing.json`` -- who rigs and animates each character.
* ``tripo_rig_check.json``   -- cached (free) Tripo rig-check answers.
* ``uthana_rig.json``        -- Uthana character ids and rig results.
* ``uthana_motion_plan.json``-- every planned Uthana motion.
* ``uthana_animation.json``  -- motion ledger (created/reused/library, files).

All are plain JSON beside the other artifacts. A missing or unreadable file
reads as "nothing recorded yet", which is what a first run and an old
project both look like.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
from typing import Any, Iterable, Optional

from ptv_schemas import CharacterRoutingPlan, UthanaMotionPlan

from ..fsutil import atomic_write_text

ROUTING_NAME = "character_routing.json"
RIG_CHECK_NAME = "tripo_rig_check.json"
UTHANA_RIG_NAME = "uthana_rig.json"
MOTION_PLAN_NAME = "uthana_motion_plan.json"
UTHANA_LEDGER_NAME = "uthana_animation.json"
ASSET_ANIMATION_NAME = "asset_animation.json"

UTHANA_RIGGED_GLB = "model_uthana_rigged.glb"
UTHANA_MOTIONS_DIR = "uthana_motions"


def load_json(path: Path) -> dict[str, Any]:
    if not path.is_file():
        return {}
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}
    return data if isinstance(data, dict) else {}


def save_json(path: Path, payload: dict[str, Any]) -> None:
    atomic_write_text(path, json.dumps(payload, indent=2, sort_keys=True))


def manifest_asset_ids(ctx: Any) -> list[str]:
    """Asset ids in the current manifest; empty when there is none (then
    nothing is pruned)."""
    try:
        manifest = ctx.asset_manifest or ctx.store.load_asset_manifest(ctx.project_id)
    except Exception:  # noqa: BLE001 - no manifest means no pruning
        return []
    return [it.id for it in manifest.items]


def prune_to_assets(ledger: dict[str, Any], asset_ids: Iterable[str]) -> list[str]:
    """Drop entries for assets the current manifest no longer has.

    Ledgers keyed by asset id outlive the manifest that wrote them; an entry
    for an asset from an earlier plan is at best noise and at worst reused.
    Mutates ``ledger``; returns the removed ids."""
    keep = set(asset_ids)
    if not keep:
        return []
    stale = sorted(k for k in ledger if k not in keep)
    for k in stale:
        del ledger[k]
    return stale


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def load_routing(project_dir: Path) -> Optional[CharacterRoutingPlan]:
    data = load_json(project_dir / ROUTING_NAME)
    if not data:
        return None
    try:
        return CharacterRoutingPlan.model_validate(data)
    except ValueError:
        return None


def save_routing(project_dir: Path, plan: CharacterRoutingPlan) -> Path:
    path = project_dir / ROUTING_NAME
    atomic_write_text(path, plan.model_dump_json(indent=2))
    return path


def load_motion_plan(project_dir: Path) -> Optional[UthanaMotionPlan]:
    data = load_json(project_dir / MOTION_PLAN_NAME)
    if not data:
        return None
    try:
        return UthanaMotionPlan.model_validate(data)
    except ValueError:
        return None


def save_motion_plan(project_dir: Path, plan: UthanaMotionPlan) -> Path:
    path = project_dir / MOTION_PLAN_NAME
    atomic_write_text(path, plan.model_dump_json(indent=2))
    return path


def rel(repo_root: Path, path: Path) -> str:
    try:
        return str(path.relative_to(repo_root)).replace("\\", "/")
    except ValueError:
        return str(path).replace("\\", "/")
