"""Start a full run from a clean project.

"Run full pipeline" means: plan, generate and build this game again from
the prompt. Every stage, though, was written to make a Resume cheap -- the
codegen board keeps finished tasks, asset stages reuse any file already on
disk under the same asset id, ledgers keep entries for assets the plan no
longer has, and globs pick up scene files of scenes that no longer exist. On a
full run all of that is the previous game leaking into the new one.

So a full run first moves the previous run's output aside, into
``projects/<id>/previous_runs/<UTC stamp>/``, and starts from the prompt.
Resume, Resume from and Run stage never call this: they are the paths where
reuse is the point.

Kept in place: ``prompt.txt``, ``project_state.json`` (with its stage history
archived and cleared) and ``builds/`` (build caches; everything in them is
rebuilt from sources, and the Quest stage already rewrites its packaged
assets).
"""

from __future__ import annotations

import shutil
from datetime import datetime, timezone
from pathlib import Path
from typing import Iterable, Optional

from ptv_schemas import ProjectStage

from ..fsutil import replace_with_retry, rmtree_with_retry, unlink_with_retry
from .context import PipelineContext

ARCHIVE_DIR_NAME = "previous_runs"
#: Project files that belong to the project, not to a run.
_KEEP_FILES = {"prompt.txt", "project_state.json"}
#: Project folders a run fills from scratch.
_RUN_DIRS = ("logs", "images", "assets", "generated_code")
#: engine/generated/<kind>/<project_id>/ -- swept into every build by glob.
_GENERATED_KINDS = ("mechanics", "ai", "dialogue", "world")


class ArchiveError(RuntimeError):
    """Something from the previous run could not be moved aside."""


def _has_content(path: Path) -> bool:
    if path.is_file():
        return True
    return path.is_dir() and any(path.iterdir())


def _move(src: Path, dst: Path, moved: list[str]) -> None:
    dst.parent.mkdir(parents=True, exist_ok=True)
    try:
        replace_with_retry(src, dst)
    except OSError as exc:
        raise ArchiveError(
            f"could not move {src} aside ({exc}). Close anything holding files in "
            "it (an open GLB viewer, Explorer preview, a running build) and run again."
        ) from exc
    moved.append(str(src))


def archive_previous_run(ctx: PipelineContext, *, keep: Optional[int] = None,
                         label: str = "") -> Optional[Path]:
    """Move the previous run's output into ``previous_runs/<stamp>/``.

    Returns the archive folder, or None when there was nothing to archive (a
    brand-new project). Raises :class:`ArchiveError` before anything paid
    runs if a file stays locked."""

    project_dir: Path = ctx.project_dir
    repo_root: Path = ctx.settings.repo_root
    keep = ctx.settings.full_run_archive_keep if keep is None else keep
    stamp = datetime.now(tz=timezone.utc).strftime("%Y%m%d-%H%M%S")
    archive = project_dir / ARCHIVE_DIR_NAME / (f"{stamp}-{label}" if label else stamp)

    candidates: list[tuple[Path, Path]] = []
    for p in sorted(project_dir.iterdir()):
        if p.is_file() and p.name not in _KEEP_FILES:
            candidates.append((p, archive / p.name))
    for name in _RUN_DIRS:
        p = project_dir / name
        if _has_content(p):
            candidates.append((p, archive / name))
    gen_root = repo_root / "engine" / "generated"
    for kind in _GENERATED_KINDS:
        p = gen_root / kind / ctx.project_id
        if _has_content(p):
            candidates.append((p, archive / "engine_generated" / kind))

    state = ctx.store.load_state(ctx.project_id)
    if not candidates and not state.stages:
        return None

    moved: list[str] = []
    for src, dst in candidates:
        _move(src, dst, moved)
    for name in _RUN_DIRS:
        (project_dir / name).mkdir(parents=True, exist_ok=True)

    # The stage history belongs to the archived run. Left in place, Resume
    # would read the old run's "succeeded" for stages this run has not
    # reached and skip them.
    archive.mkdir(parents=True, exist_ok=True)
    (archive / "project_state.json").write_text(
        state.model_dump_json(indent=2, by_alias=True), encoding="utf-8")
    state.stages = []
    state.latest_build_exe = None
    state.latest_build_dir = None
    state.latest_scene_json = None
    state.latest_build_apk = None
    ctx.store.save_state(state)

    pruned = _prune_archives(project_dir / ARCHIVE_DIR_NAME, keep)
    note = (f"archived the previous run ({len(moved)} item(s)) to "
            f"{archive.relative_to(project_dir).as_posix()}")
    if pruned:
        note += f"; removed {len(pruned)} older archive(s)"
    ctx.emit(ProjectStage.build_spec, "info", note)
    ctx.store.append_log(ctx.project_id, "run", note)
    if keep <= 0:
        rmtree_with_retry(archive)
    return archive


def remove_stale_scene_files(project_dir: Path, prefix: str,
                             scene_ids: Iterable[str]) -> list[str]:
    """Delete ``<prefix>.<scene_id>.json`` for scenes the plan no longer has.

    Several stages glob these files (codegen context, world generation, the
    asset budget, the Quest packager), so a scene from an earlier plan would
    otherwise keep leaking into the current game."""
    keep = set(scene_ids)
    removed: list[str] = []
    for p in sorted(project_dir.glob(f"{prefix}.*.json")):
        scene_id = p.name[len(prefix) + 1:-len(".json")]
        if scene_id and scene_id not in keep and unlink_with_retry(p):
            removed.append(p.name)
    return removed


def _prune_archives(root: Path, keep: int) -> list[str]:
    """Keep the newest ``keep`` archives (names sort by time)."""
    if not root.is_dir():
        return []
    runs = sorted((p for p in root.iterdir() if p.is_dir()), key=lambda p: p.name)
    old = runs[:-keep] if keep > 0 else runs[:-1]
    removed: list[str] = []
    for p in old:
        if rmtree_with_retry(p):
            removed.append(p.name)
        else:
            shutil.rmtree(p, ignore_errors=True)
    return removed
