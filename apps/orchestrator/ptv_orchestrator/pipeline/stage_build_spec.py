"""Stage 1: Prompt -> BuildSpec via Claude Opus structured output."""

from __future__ import annotations

import re

from ptv_providers import ProviderConfigError
from ptv_schemas import (
    BuildSpec,
    PlayerExperience,
    ProjectStage,
    ProjectStatus,
    TargetDevice,
)

from .context import PipelineContext, StageResult
from .llm import make_llm
from .player_experience import (
    strip_player_avatar_assets,
    sync_build_spec_experience,
)
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


def _force_player_experience(
    ctx: PipelineContext, spec: BuildSpec, experience: PlayerExperience
) -> BuildSpec:
    """The project's experience choice wins, and Hands Only has no body."""
    was = spec.player_experience
    spec, changed = sync_build_spec_experience(spec, experience)
    if changed and was != experience:
        ctx.emit(
            ProjectStage.build_spec,
            "info",
            f"player_experience: replaced the model's {was.value!r} with the "
            f"project's {experience.value!r}",
        )
    spec, dropped = strip_player_avatar_assets(spec)
    if dropped:
        ctx.emit(
            ProjectStage.build_spec,
            "info",
            f"Hands Only: dropped player-body asset(s) {dropped}",
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
        f"Use player_experience = {state.player_experience.value!r} (also the"
        " user's choice)"
        + (
            ": the player is a pair of hands -- set player_avatar.coverage to"
            " 'hands_only' and plan NO player-body asset. "
            if state.player_experience == PlayerExperience.hands_only
            else ": the player has a generated full-body avatar. "
        )
        + "Decide the `movement` block from the prompt. "
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
    spec = _force_player_experience(ctx, spec, state.player_experience)
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
