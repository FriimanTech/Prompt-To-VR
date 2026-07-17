"""Stage 3: BuildSpec + AssetManifest -> MechanicPlan (OpenAI Agents SDK).

The planner is an agent (gpt-5.5 + WebSearchTool), not a single LLM call.
It is reuse-first: at runtime we append the engine catalog (built-in
mechanics + mechanics promoted from past games) so the agent classifies
each mechanic against that catalog before flagging anything as "generate".

Classification is four-way:

* ``builtin``                    -- a catalog entry satisfies as-is.
* ``builtin+config``             -- satisfied by parameter tuning only.
* ``builtin+config_code_edit``   -- a catalog entry is the right foundation
  but params cannot satisfy the prompt; the code stage copies the built-in's
  source and edits it into a NEW generated mechanic.
* ``generate``                   -- a brand-new mechanic.

The agent also assigns a controller binding to every input-driven mechanic;
a deterministic validator (``binding_validator``) then normalizes
face-button hands (A/B -> right, X/Y -> left) and emits advisory warnings
before the plan is saved. Sharing a control across mechanics is allowed and
is not rejected.

See ``apps/orchestrator/ptv_orchestrator/engine_catalog.py`` for the catalog
source of truth and ``builtin_source.py`` for the code-edit source resolver.
"""

from __future__ import annotations

import os
from typing import Optional

from ptv_schemas import MechanicClassification, MechanicPlan, ProjectStage, ProjectStatus

from ..engine_catalog import as_planner_text
from .binding_validator import validate_bindings
from .builtin_source import editable_builtins_note
from .context import PipelineContext, StageResult
from .mechanic_plan_agent import (
    MechanicPlanAgentInput,
    MechanicPlanner,
    build_planner_message,
)
from .prompts import MECHANIC_PLAN_SYSTEM


def run(ctx: PipelineContext) -> StageResult:
    """Standard stage entry point."""
    return run_with_planner(ctx, planner=None)


def run_with_planner(
    ctx: PipelineContext, planner: Optional[MechanicPlanner] = None
) -> StageResult:
    """Run the mechanic-plan stage with an optional injected planner (tests
    pass a stub so the OpenAI Agents SDK is never touched)."""

    ctx.emit(ProjectStage.mechanic_plan, "running", "Planning mechanics")
    state = ctx.store.load_state(ctx.project_id)
    ctx.store.record_stage(state, ProjectStage.mechanic_plan, ProjectStatus.running)

    build_spec = ctx.build_spec or ctx.store.load_build_spec(ctx.project_id)
    manifest = ctx.asset_manifest or ctx.store.load_asset_manifest(ctx.project_id)

    catalog_text = as_planner_text(ctx.settings.repo_root)

    # Bridge OPENAI_API_KEY into os.environ for the Agents SDK (BaseSettings
    # does not export it automatically).
    if ctx.settings.openai_api_key and not os.environ.get("OPENAI_API_KEY"):
        os.environ["OPENAI_API_KEY"] = ctx.settings.openai_api_key

    if planner is None:
        planner = MechanicPlanner(
            instructions=MECHANIC_PLAN_SYSTEM,
            model=ctx.settings.mechanic_plan_model,
            reasoning_effort=ctx.settings.mechanic_plan_reasoning_effort,
            max_turns=ctx.settings.mechanic_plan_agent_max_turns,
            session_id=f"ptv-mechanic-plan-{ctx.project_id}",
        )

    inp = MechanicPlanAgentInput(
        build_spec_json=build_spec.model_dump_json(indent=2),
        asset_manifest_json=manifest.model_dump_json(indent=2),
        catalog_text=catalog_text,
        editable_builtins_note=editable_builtins_note(),
        extra_instructions=[
            "Emit one MechanicPlanItem per BuildSpec mechanic (1:1 by id).",
            "Reuse catalog entries (built-in OR promoted) whenever the request"
            " is covered; escalate params -> code_edit -> generate only as"
            " needed.",
            "For hub-type BuildSpec mechanics, generate the hub and set"
            " depends_on so branch mechanics are built after it.",
        ],
    )

    message = build_planner_message(inp)
    plan: MechanicPlan = planner.run(message)

    # Deterministic controller-binding normalization (normalizes face-button
    # hands in place; emits advisory warnings only -- sharing a control is
    # allowed, so this does not reject the plan).
    binding_result = validate_bindings(plan)
    for warn in binding_result.warnings:
        ctx.emit(ProjectStage.mechanic_plan, "warn", warn[:240])
    if not binding_result.ok:
        detail = "; ".join(binding_result.errors)
        ctx.store.record_stage(
            state,
            ProjectStage.mechanic_plan,
            ProjectStatus.failed,
            message=f"controller binding conflicts: {detail}",
        )
        ctx.emit(ProjectStage.mechanic_plan, "failed", detail[:240])
        raise ValueError(f"MechanicPlan binding conflicts: {detail}")

    ctx.mechanic_plan = plan
    art = ctx.store.save_mechanic_plan(ctx.project_id, plan)

    # Emit a reuse-vs-generate summary so the UI can highlight engine growth.
    counts: dict[str, int] = {}
    for it in plan.items:
        key = it.classification.value
        counts[key] = counts.get(key, 0) + 1
    gen_count = counts.get("generate", 0)
    code_edit_count = counts.get(
        MechanicClassification.builtin_config_code_edit.value, 0
    )
    reuse_count = counts.get("builtin", 0) + counts.get("builtin+config", 0)
    # code_edit and generate both produce a CodeTask.
    codegen_count = gen_count + code_edit_count
    summary = (
        f"reuse={reuse_count} (builtin/builtin+config), "
        f"code_edit={code_edit_count}, generate={gen_count}"
    )
    ctx.emit(ProjectStage.mechanic_plan, "info", summary)

    ctx.store.record_stage(
        state,
        ProjectStage.mechanic_plan,
        ProjectStatus.succeeded,
        message=f"{codegen_count} mechanics require generated C++ ({summary})",
        artifacts=[art.path],
    )
    ctx.emit(
        ProjectStage.mechanic_plan,
        "succeeded",
        f"{codegen_count} to generate, {reuse_count} reused",
    )
    return StageResult(stage=ProjectStage.mechanic_plan, artifacts=[art.path], data=plan)
