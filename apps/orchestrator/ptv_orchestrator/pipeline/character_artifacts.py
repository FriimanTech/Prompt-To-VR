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
from typing import Any, Optional

from ptv_schemas import CharacterRoutingPlan, UthanaMotionPlan

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
    path.write_text(json.dumps(payload, indent=2, sort_keys=True), encoding="utf-8")


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
    path.write_text(plan.model_dump_json(indent=2), encoding="utf-8")
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
    path.write_text(plan.model_dump_json(indent=2), encoding="utf-8")
    return path


def rel(repo_root: Path, path: Path) -> str:
    try:
        return str(path.relative_to(repo_root)).replace("\\", "/")
    except ValueError:
        return str(path).replace("\\", "/")
