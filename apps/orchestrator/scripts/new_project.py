"""Create a project from a prompt and run the whole pipeline, without the UI.

    python apps/orchestrator/scripts/new_project.py "<title>" <prompt_file> [quest|pcvr] [hands_only|full_avatar]

Prints the project id first, then each stage's outcome. Settings come from
``.env`` / the environment as usual (``QUEST_RUNTIME_VERIFY=false`` skips the
on-headset pass, e.g. when several projects build at once and share one
headset).
"""

from __future__ import annotations

import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "apps" / "orchestrator"))

from ptv_orchestrator.pipeline.orchestrator import run_pipeline  # noqa: E402
from ptv_orchestrator.settings import get_settings  # noqa: E402
from ptv_orchestrator.storage import ProjectStore  # noqa: E402
from ptv_schemas import PlayerExperience, TargetDevice  # noqa: E402


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    title, prompt = sys.argv[1], Path(sys.argv[2]).read_text(encoding="utf-8")
    device = TargetDevice(sys.argv[3]) if len(sys.argv) > 3 else TargetDevice.quest
    experience = (PlayerExperience(sys.argv[4]) if len(sys.argv) > 4
                  else PlayerExperience.hands_only)
    settings = get_settings()
    store = ProjectStore(settings.projects_dir)
    state = store.create(title, prompt, target_device=device, player_experience=experience)
    print(f"project {state.project_id}", flush=True)
    error = None
    try:
        run_pipeline(state.project_id, settings)
    except Exception as exc:  # noqa: BLE001 - report the stages either way
        error = exc
    state = store.load_state(state.project_id)
    for rec in state.stages:
        print(f"  {rec.stage.value}: {getattr(rec.status, 'value', rec.status)} {rec.message or ''}"[:300],
              flush=True)
    if error is not None:
        print(f"FAILED: {type(error).__name__}: {str(error)[:1500]}", flush=True)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
