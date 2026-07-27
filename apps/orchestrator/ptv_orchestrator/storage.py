"""Filesystem project storage.

Layout per project::

    projects/<project_id>/
        prompt.txt
        project_state.json
        build_spec.json
        asset_manifest.json
        mechanic_plan.json
        scene_spec.json
        acceptance_test_plan.json
        images/
        assets/
        generated_code/
        logs/
        builds/
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import threading
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable

from pydantic import BaseModel
from ptv_schemas import (
    AcceptanceTestPlan,
    ArtifactKind,
    ArtifactMetadata,
    AssetManifest,
    BuildSpec,
    MechanicPlan,
    ProjectStage,
    ProjectState,
    ProjectStatus,
    SceneSpec,
    StageRecord,
    TargetDevice,
)

_lock = threading.Lock()


def _now() -> datetime:
    return datetime.now(tz=timezone.utc)


def _slug(title: str) -> str:
    s = re.sub(r"[^a-zA-Z0-9]+", "-", title.strip().lower()).strip("-")
    return s[:48] or "project"


def _sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def _sha256_text(s: str) -> str:
    return hashlib.sha256(s.encode("utf-8")).hexdigest()


class ProjectStore:
    def __init__(self, projects_dir: Path) -> None:
        self.projects_dir = Path(projects_dir)
        self.projects_dir.mkdir(parents=True, exist_ok=True)

    # ------------------------------------------------------------------
    # IDs / paths
    # ------------------------------------------------------------------
    def new_id(self, title: str) -> str:
        stamp = _now().strftime("%Y%m%d-%H%M%S")
        return f"{stamp}-{_slug(title)}"

    def dir_for(self, project_id: str) -> Path:
        return self.projects_dir / project_id

    # ------------------------------------------------------------------
    # List / load / save ProjectState
    # ------------------------------------------------------------------
    def list_projects(self) -> list[ProjectState]:
        out: list[ProjectState] = []
        if not self.projects_dir.exists():
            return out
        for p in sorted(self.projects_dir.iterdir(), reverse=True):
            if not p.is_dir():
                continue
            state_path = p / "project_state.json"
            if state_path.exists():
                try:
                    out.append(self.load_state(p.name))
                except Exception as exc:
                    # Don't silently hide a broken project: log to stderr so
                    # the operator sees why their project vanished from the
                    # UI. A broken state.json (bad enum, bad datetime, etc.)
                    # will now show up in the orchestrator logs instead of
                    # just an empty list.
                    import sys as _sys

                    print(
                        f"[list_projects] skipping {p.name}: {type(exc).__name__}: {exc}",
                        file=_sys.stderr,
                    )
                    continue
        return out

    def load_state(self, project_id: str) -> ProjectState:
        path = self.dir_for(project_id) / "project_state.json"
        if not path.exists():
            raise FileNotFoundError(f"Project {project_id!r} not found.")
        data = json.loads(path.read_text(encoding="utf-8"))
        return ProjectState.model_validate(data)

    def save_state(self, state: ProjectState) -> None:
        proj_dir = self.dir_for(state.project_id)
        proj_dir.mkdir(parents=True, exist_ok=True)
        state.updated_at = _now()
        path = proj_dir / "project_state.json"
        with _lock:
            path.write_text(
                state.model_dump_json(indent=2, by_alias=True),
                encoding="utf-8",
            )

    # ------------------------------------------------------------------
    # Create
    # ------------------------------------------------------------------
    def create(
        self,
        title: str,
        prompt: str,
        target_device: TargetDevice = TargetDevice.pcvr,
    ) -> ProjectState:
        pid = self.new_id(title)
        proj_dir = self.dir_for(pid)
        for sub in ("images", "assets", "generated_code", "logs", "builds"):
            (proj_dir / sub).mkdir(parents=True, exist_ok=True)
        (proj_dir / "prompt.txt").write_text(prompt, encoding="utf-8")
        state = ProjectState(
            project_id=pid,
            title=title[:128] or "Untitled",
            prompt=prompt,
            target_device=target_device,
        )
        self.save_state(state)
        return state

    # ------------------------------------------------------------------
    # Artifact helpers
    # ------------------------------------------------------------------
    def write_model(
        self,
        project_id: str,
        filename: str,
        model: BaseModel,
    ) -> ArtifactMetadata:
        path = self.dir_for(project_id) / filename
        path.parent.mkdir(parents=True, exist_ok=True)
        text = model.model_dump_json(indent=2, by_alias=True)
        path.write_text(text, encoding="utf-8")
        return ArtifactMetadata(
            id=_artifact_id(filename),
            kind=ArtifactKind.json,
            path=str(path.relative_to(self.projects_dir.parent)).replace("\\", "/"),
            size_bytes=len(text.encode("utf-8")),
            sha256=_sha256_text(text),
            stage=filename.rsplit(".", 1)[0],
        )

    def list_artifacts(self, project_id: str) -> list[ArtifactMetadata]:
        root = self.dir_for(project_id)
        if not root.exists():
            return []
        items: list[ArtifactMetadata] = []
        for p in _walk(root):
            rel_from_repo = p.relative_to(self.projects_dir.parent).as_posix()
            try:
                items.append(
                    ArtifactMetadata(
                        id=_artifact_id(p.name),
                        kind=_kind_for(p),
                        path=rel_from_repo,
                        size_bytes=p.stat().st_size,
                        sha256=_sha256(p),
                        stage=_stage_for(p, root),
                        description=None,
                    )
                )
            except Exception as exc:
                # One unlistable file (e.g. a vcpkg-emitted ``_features.hpp``
                # before the id sanitizer was strict enough, a locked build
                # intermediate, or a file that disappeared mid-walk) must
                # never crash the whole /artifacts endpoint. Log and skip.
                import sys as _sys

                print(
                    f"[list_artifacts] skipping {rel_from_repo}: "
                    f"{type(exc).__name__}: {exc}",
                    file=_sys.stderr,
                )
                continue
        return items

    # ------------------------------------------------------------------
    # Schema helpers
    # ------------------------------------------------------------------
    def save_build_spec(self, project_id: str, spec: BuildSpec) -> ArtifactMetadata:
        return self.write_model(project_id, "build_spec.json", spec)

    def save_asset_manifest(
        self, project_id: str, manifest: AssetManifest
    ) -> ArtifactMetadata:
        return self.write_model(project_id, "asset_manifest.json", manifest)

    def save_mechanic_plan(
        self, project_id: str, plan: MechanicPlan
    ) -> ArtifactMetadata:
        return self.write_model(project_id, "mechanic_plan.json", plan)

    def save_scene_spec(
        self, project_id: str, scene: SceneSpec, filename: str | None = None
    ) -> ArtifactMetadata:
        return self.write_model(
            project_id, filename or "scene_spec.json", scene
        )

    def save_acceptance_plan(
        self, project_id: str, plan: AcceptanceTestPlan
    ) -> ArtifactMetadata:
        return self.write_model(project_id, "acceptance_test_plan.json", plan)

    def load_build_spec(self, project_id: str) -> BuildSpec:
        return _load_json_model(
            self.dir_for(project_id) / "build_spec.json", BuildSpec
        )

    def load_asset_manifest(self, project_id: str) -> AssetManifest:
        return _load_json_model(
            self.dir_for(project_id) / "asset_manifest.json", AssetManifest
        )

    def load_mechanic_plan(self, project_id: str) -> MechanicPlan:
        return _load_json_model(
            self.dir_for(project_id) / "mechanic_plan.json", MechanicPlan
        )

    def load_scene_spec(
        self, project_id: str, filename: str = "scene_spec.json"
    ) -> SceneSpec:
        return _load_json_model(self.dir_for(project_id) / filename, SceneSpec)

    # ------------------------------------------------------------------
    # Stage transitions
    # ------------------------------------------------------------------
    def record_stage(
        self,
        state: ProjectState,
        stage: ProjectStage,
        status: ProjectStatus,
        message: str = "",
        artifacts: Iterable[str] = (),
    ) -> None:
        record = StageRecord(
            stage=stage,
            status=status,
            started_at=_now() if status == ProjectStatus.running else None,
            ended_at=(
                _now() if status in (ProjectStatus.succeeded, ProjectStatus.failed) else None
            ),
            message=message,
            artifacts=list(artifacts),
        )
        state.stages.append(record)
        state.current_stage = stage
        state.status = status
        self.save_state(state)

    def append_log(self, project_id: str, name: str, line: str) -> None:
        path = self.dir_for(project_id) / "logs" / f"{name}.log"
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("a", encoding="utf-8") as f:
            f.write(line.rstrip("\n") + "\n")


# Build-intermediate directories that must never be treated as project
# artifacts. They contain thousands of transient files (compiler ``.obj``
# outputs, vcpkg installs, CMake bookkeeping) -- some of them locked by an
# in-flight MSBuild, which made ``list_artifacts`` spam
# ``PermissionError: [Errno 13]`` while hashing e.g.
# ``builds/cmake-current/runtime/ptv_runtime.dir/Release/*.obj``. Pruning
# these directories keeps the final binaries (``.exe``/``.dll``/``.pdb`` that
# live directly under a ``<config>/`` output dir, not under a ``*.dir``) while
# skipping the intermediates entirely -- faster and no spurious errors.
_PRUNED_ARTIFACT_DIR_NAMES = frozenset(
    {
        "CMakeFiles",
        "vcpkg_installed",
        "__pycache__",
        ".cmake",
        "Testing",
    }
)


def _is_pruned_artifact_dir(name: str) -> bool:
    # Per-target intermediate dirs are named ``<target>.dir`` (this is where
    # the locked ``*.obj`` files live).
    return name in _PRUNED_ARTIFACT_DIR_NAMES or name.endswith(".dir")


def _walk(root: Path):
    for dirpath, dirnames, filenames in os.walk(root):
        # Prune in place so os.walk never descends into build intermediates.
        dirnames[:] = [d for d in dirnames if not _is_pruned_artifact_dir(d)]
        base = Path(dirpath)
        for fname in filenames:
            p = base / fname
            if p.is_file():
                yield p


def _kind_for(p: Path) -> ArtifactKind:
    s = p.suffix.lower()
    if s == ".json":
        return ArtifactKind.json
    if s in (".png", ".jpg", ".jpeg", ".webp"):
        return ArtifactKind.image
    if s in (".glb", ".gltf"):
        return ArtifactKind.glb
    if s in (".cpp", ".cc", ".cxx"):
        return ArtifactKind.cpp
    if s in (".h", ".hpp"):
        return ArtifactKind.header
    if s == ".log":
        return ArtifactKind.log
    if s in (".exe", ".dll", ".pdb"):
        return ArtifactKind.binary
    return ArtifactKind.other


def _stage_for(p: Path, root: Path) -> str:
    try:
        rel = p.relative_to(root)
    except ValueError:
        return "other"
    parts = rel.parts
    if len(parts) == 1:
        return p.stem
    return parts[0]


def _artifact_id(name: str) -> str:
    r"""Produce an ``IdStr``-compatible id for an artifact filename.

    ``IdStr``'s regex (``^[a-z0-9][a-z0-9_.\-]{0,63}$``) requires the first
    character to be alphanumeric. Real build outputs routinely violate that
    (vcpkg emits headers like ``_features.hpp``; CMake drops
    ``.ninja_deps`` files), so we:

    1. lowercase,
    2. replace every disallowed character with ``-``,
    3. strip leading characters that are not ``[a-z0-9]`` (so ``_features.hpp``
       -> ``features.hpp`` and ``.ninja_deps`` -> ``ninja_deps``),
    4. clip to 64 chars,
    5. fall back to a stable ``artifact`` id if every char was stripped.
    """

    stem = re.sub(r"[^a-z0-9._-]+", "-", name.lower()).strip("-")
    stem = re.sub(r"^[^a-z0-9]+", "", stem)
    return stem[:64] or "artifact"


def _load_json_model(path: Path, model_cls: type[BaseModel]):
    if not path.exists():
        raise FileNotFoundError(path)
    return model_cls.model_validate(json.loads(path.read_text(encoding="utf-8")))
