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
    # The in-flight run this context belongs to, when there is one. Stages
    # call ``raise_if_cancelled()`` at loop boundaries so Abort takes effect
    # without waiting for a twenty-minute build to finish. ``None`` in tests
    # and in any caller that runs a stage outside a registered run, where
    # cancellation is simply a no-op.
    run: Any = None
    # caches filled as stages run
    build_spec: Any = None
    asset_manifest: Any = None
    npc_plan: Any = None
    mechanic_plan: Any = None
    scene_specs: list[Any] = field(default_factory=list)

    @property
    def project_dir(self) -> Path:
        return self.store.dir_for(self.project_id)

    def raise_if_cancelled(self) -> None:
        """Cooperative cancellation point. Raises ``RunCancelled``."""
        if self.run is not None:
            self.run.raise_if_cancelled()

    def register_process(self, proc: Any) -> None:
        """Track a child process so Abort can kill its whole tree."""
        if self.run is not None:
            self.run.register_process(proc)

    def forget_process(self, proc: Any) -> None:
        if self.run is not None:
            self.run.forget_process(proc)

    def emit(self, stage: ProjectStage, status: str, message: str = "") -> None:
        # Progress reporting must never fail the stage it reports on.
        try:
            self.bus.publish(
                PipelineEvent(
                    project_id=self.project_id,
                    stage=stage.value,
                    status=status,
                    message=message,
                )
            )
        except Exception:  # noqa: BLE001
            pass
