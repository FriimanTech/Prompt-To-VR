"""Stage 11: Engine promotion.

Runs after a successful ``cmake_build``. For every generated mechanic /
behaviour / dialogue-hook this project produced, we record an entry in
``engine/promoted/registry.json`` so the next pipeline run will see it
in the planner catalog as ``promoted.<sanitised id>``.

We deliberately do NOT copy the generated source files anywhere: the
files already live under ``engine/generated/<kind>/<project_id>/`` and
are linked into ``ptv_runtime`` via the engine CMake glob, so they're
already reusable across projects. The registry just gives the planner a
human-readable summary keyed by id.

The stage is best-effort:
* It never fails the pipeline (a malformed CodeTask sidecar just gets
  skipped with a warning).
* It is idempotent: re-running on the same project doesn't duplicate
  entries.
"""

from __future__ import annotations

import datetime as _dt
import json
import re
from pathlib import Path

from ptv_schemas import ProjectStage, ProjectStatus

from ..engine_catalog import promoted_registry_path
from .context import PipelineContext, StageResult


_KIND_DIRS = ("mechanics", "ai", "dialogue")


def _sanitise(raw: str) -> str:
    s = re.sub(r"[^a-zA-Z0-9_.-]+", "_", raw).strip("_.-")
    return s.lower()[:96] or "module"


def _read_codetask(side: Path) -> dict[str, object] | None:
    if not side.exists():
        return None
    try:
        data = json.loads(side.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None
    return data if isinstance(data, dict) else None


def _load_registry(path: Path) -> dict[str, object]:
    if not path.exists():
        return {"version": 1, "items": []}
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {"version": 1, "items": []}
    if not isinstance(data, dict):
        return {"version": 1, "items": []}
    if not isinstance(data.get("items"), list):
        data["items"] = []
    data.setdefault("version", 1)
    return data


def _write_registry(path: Path, data: dict[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")


def run(ctx: PipelineContext) -> StageResult:
    ctx.emit(ProjectStage.engine_promotion, "running", "Promoting generated mechanics")
    state = ctx.store.load_state(ctx.project_id)
    ctx.store.record_stage(state, ProjectStage.engine_promotion, ProjectStatus.running)

    repo_root: Path = ctx.settings.repo_root
    registry_path = promoted_registry_path(repo_root)
    registry = _load_registry(registry_path)

    existing_ids: set[str] = {
        str(it.get("id"))
        for it in registry["items"]  # type: ignore[index]
        if isinstance(it, dict) and it.get("id")
    }

    promoted_now: list[str] = []
    skipped: list[str] = []

    for kind in _KIND_DIRS:
        gen_dir = repo_root / "engine" / "generated" / kind / ctx.project_id
        if not gen_dir.exists():
            continue
        for side in sorted(gen_dir.glob("*.codetask.json")):
            cpp = side.with_suffix("").with_suffix(".cpp")
            if not cpp.exists():
                skipped.append(f"{side.name}: no .cpp")
                continue
            ct = _read_codetask(side)
            if not ct:
                skipped.append(f"{side.name}: malformed")
                continue
            task_id = str(ct.get("id") or "").strip()
            if not task_id:
                skipped.append(f"{side.name}: missing id")
                continue
            promoted_id = f"promoted.{_sanitise(task_id)}"
            if promoted_id in existing_ids:
                continue

            entry = {
                "id": promoted_id,
                "summary": str(
                    ct.get("description")
                    or ct.get("summary")
                    or f"Generated {kind} from project {ctx.project_id}"
                )[:600],
                "category": _category_for(kind),
                "kind": kind,
                "source_path": str(cpp.relative_to(repo_root)).replace("\\", "/"),
                "code_task_id": task_id,
                "project_id": ctx.project_id,
                "promoted_at": _dt.datetime.now(tz=_dt.timezone.utc).isoformat(),
                "params": ct.get("params") if isinstance(ct.get("params"), dict) else {},
            }
            registry["items"].append(entry)  # type: ignore[index]
            existing_ids.add(promoted_id)
            promoted_now.append(promoted_id)
            ctx.emit(
                ProjectStage.engine_promotion,
                "info",
                f"promoted {promoted_id}",
            )

    _write_registry(registry_path, registry)

    rel = str(registry_path.relative_to(repo_root)).replace("\\", "/")
    msg = (
        f"promoted {len(promoted_now)} (registry now {len(registry['items'])} entries)"
        + (f"; skipped {len(skipped)}" if skipped else "")
    )
    ctx.store.record_stage(
        state,
        ProjectStage.engine_promotion,
        ProjectStatus.succeeded,
        message=msg,
        artifacts=[rel],
    )
    ctx.emit(ProjectStage.engine_promotion, "succeeded", msg)
    return StageResult(
        stage=ProjectStage.engine_promotion,
        artifacts=[rel],
        message=msg,
    )


def _category_for(kind: str) -> str:
    return {
        "mechanics": "mechanic",
        "ai": "ai",
        "dialogue": "dialogue",
    }.get(kind, "mechanic")
