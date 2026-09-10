"""Stage 1: Prompt -> BuildSpec via Claude Opus structured output."""

from __future__ import annotations

import re

from ptv_providers import ProviderConfigError
from ptv_schemas import BuildSpec, ProjectStage, ProjectStatus, TargetDevice

from .context import PipelineContext, StageResult
from .llm import make_llm
from .prompts import build_spec_system
from .target_device import sync_build_spec_device


def _prompt_id_from_text(prompt: str) -> str:
    s = re.sub(r"[^a-zA-Z0-9]+", "-", prompt.strip().lower()).strip("-")
    return (s[:48] or "prompt")


def _force_target_device(
    ctx: PipelineContext, spec: BuildSpec, target_device: TargetDevice
) -> BuildSpec:
    """The project's device wins over whatever the model returned."""
    was = spec.target_device
    spec, changed = sync_build_spec_device(spec, target_device)
    if changed:
        ctx.emit(
            ProjectStage.build_spec,
            "info",
            f"target_device: replaced the model's {was.value!r} with the "
            f"project's {target_device.value!r}",
        )
    return spec


def run(ctx: PipelineContext) -> StageResult:
    ctx.emit(ProjectStage.build_spec, "running", "Planning BuildSpec")
    state = ctx.store.load_state(ctx.project_id)
    ctx.store.record_stage(state, ProjectStage.build_spec, ProjectStatus.running)

    schema = BuildSpec.model_json_schema(by_alias=True)
    client = make_llm(ctx.settings)

    user_prompt = (
        f"User prompt (verbatim):\n---\n{state.prompt.strip()}\n---\n"
        f"Use prompt_id = {_prompt_id_from_text(state.prompt)!r}. "
        f"Use target_device = {state.target_device.value!r} (chosen by the "
        "user when the project was created; it is not yours to pick). "
        "Produce a complete BuildSpec."
    )
    if ctx.dry_run:
        raise ProviderConfigError(
            "BuildSpec dry-run requested: supply a canned BuildSpec via tests."
        )

    resp = client.generate_json(
        prompt=user_prompt,
        schema=schema,
        system=build_spec_system(state.target_device),
    )
    spec = BuildSpec.model_validate(resp.data)
    spec = _force_target_device(ctx, spec, state.target_device)
    ctx.build_spec = spec
    art = ctx.store.save_build_spec(ctx.project_id, spec)

    ctx.store.record_stage(
        state,
        ProjectStage.build_spec,
        ProjectStatus.succeeded,
        message=f"Produced BuildSpec for {spec.title!r}",
        artifacts=[art.path],
    )
    ctx.emit(ProjectStage.build_spec, "succeeded", spec.title)
    return StageResult(
        stage=ProjectStage.build_spec, artifacts=[art.path], data=spec
    )
