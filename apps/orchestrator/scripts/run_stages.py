"""Run an explicit list of pipeline stages on an existing project, in order.

    python apps/orchestrator/scripts/run_stages.py <project_id> <stage> [<stage> ...]

For re-running just the stages a change touched (e.g. interaction_design
socket_generation asset_contract scene_compile quest_build quest_runtime_verify)
without re-authoring worlds or regenerating code. Stops at the first failure.
"""

from __future__ import annotations

import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "apps" / "orchestrator"))

from ptv_orchestrator.pipeline.orchestrator import run_stage  # noqa: E402
from ptv_orchestrator.storage import ProjectStore  # noqa: E402
from ptv_orchestrator.settings import get_settings  # noqa: E402
from ptv_schemas import ProjectStage, ProjectStatus  # noqa: E402


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    project_id, stages = sys.argv[1], [ProjectStage(s) for s in sys.argv[2:]]
    settings = get_settings()
    store = ProjectStore(settings.projects_dir)
    for stage in stages:
        print(f"=== {stage.value}", flush=True)
        run_stage(project_id, stage, settings)
        state = store.load_state(project_id)
        rec = next((r for r in reversed(state.stages) if r.stage == stage), None)
        status = rec.status if rec else None
        print(f"    -> {getattr(status, 'value', status)}: {rec.message if rec else ''}",
              flush=True)
        if status == ProjectStatus.failed:
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
