"""Shared per-run pipeline context."""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from ptv_schemas import ProjectStage

from ..events import EventBus, PipelineEvent
from ..settings import Settings
from ..storage import ProjectStore


@dataclass
class StageResult:
    stage: ProjectStage
    artifacts: list[str] = field(default_factory=list)
    message: str = ""
    data: Any = None


@dataclass
class PipelineContext:
    project_id: str
    settings: Settings
    store: ProjectStore
    bus: EventBus
    dry_run: bool = False
    # caches filled as stages run
    build_spec: Any = None
    asset_manifest: Any = None
    mechanic_plan: Any = None
    scene_specs: list[Any] = field(default_factory=list)

    @property
    def project_dir(self) -> Path:
        return self.store.dir_for(self.project_id)

    def emit(self, stage: ProjectStage, status: str, message: str = "") -> None:
        self.bus.publish(
            PipelineEvent(
                project_id=self.project_id,
                stage=stage.value,
                status=status,
                message=message,
            )
        )
