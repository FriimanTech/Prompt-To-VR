"""Generation pipeline stages.

Each module exposes a single ``run(ctx)`` callable that:

* reads the artifacts it needs,
* writes the artifacts it produces to disk via ``ctx.store``,
* appends a ``StageRecord`` via ``ctx.store.record_stage``,
* publishes progress via ``ctx.bus``.

All stages are synchronous and safe to call in a background thread.
"""
from .context import PipelineContext, StageResult
from .orchestrator import run_pipeline, resume_pipeline, run_stage

__all__ = [
    "PipelineContext",
    "StageResult",
    "run_pipeline",
    "resume_pipeline",
    "run_stage",
]
