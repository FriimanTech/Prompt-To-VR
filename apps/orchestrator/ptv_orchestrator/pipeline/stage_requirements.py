"""Stage: requirements -- the prompt as a ledger of atomic requirements.

Runs right after build_spec. One hard-tier structured call
(``requirements_ledger.extract``) plus a deterministic validator that keeps
only requirements quoted verbatim from the prompt. Every later planning
stage claims the requirements it satisfies; see ``requirements_ledger``.
"""

from __future__ import annotations

from ptv_schemas import ProjectStage, ProjectStatus, RequirementsLedger

from . import requirements_ledger as rl
from .context import PipelineContext, StageResult

_STAGE = ProjectStage.requirements


def run(ctx: PipelineContext) -> StageResult:
    from .llm import make_llm

    ctx.emit(_STAGE, "running", "Listing every requirement in the prompt")
    state = ctx.store.load_state(ctx.project_id)
    ctx.store.record_stage(state, _STAGE, ProjectStatus.running)
    build_spec = ctx.build_spec or ctx.store.load_build_spec(ctx.project_id)
    client = make_llm(ctx.settings)
    ledger: RequirementsLedger = rl.extract(
        state.prompt, str(getattr(build_spec, "prompt_id", "") or ctx.project_id),
        client, model=ctx.settings.claude_hard_model,
        build_spec_json=build_spec.model_dump_json(indent=1))
    if not ledger.requirements:
        msg = "the prompt produced no requirement quoted verbatim from it"
        ctx.store.record_stage(state, _STAGE, ProjectStatus.failed, message=msg)
        ctx.emit(_STAGE, "failed", msg)
        raise RuntimeError(msg)
    for note in ledger.notes:
        ctx.emit(_STAGE, "warning", note[:240])
    path = rl.save(ctx.project_dir, ledger)
    rel = str(path.relative_to(ctx.settings.repo_root)).replace("\\", "/")
    counts: dict[str, int] = {}
    for r in ledger.requirements:
        counts[r.kind.value] = counts.get(r.kind.value, 0) + 1
    msg = f"{len(ledger.requirements)} requirements (" + ", ".join(
        f"{k} {v}" for k, v in sorted(counts.items())) + ")"
    ctx.store.append_log(ctx.project_id, "requirements",
                         "\n".join(f"{r.id} [{r.kind.value}] {r.statement}"
                                   for r in ledger.requirements))
    ctx.store.record_stage(state, _STAGE, ProjectStatus.succeeded, message=msg, artifacts=[rel])
    ctx.emit(_STAGE, "succeeded", msg)
    return StageResult(stage=_STAGE, artifacts=[rel], data=ledger)
