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


#: Extra BuildSpec calls allowed to fix a roster that breaks its rules.
_ROSTER_RETRIES = 2

_NPC_ASSET_CATEGORIES = ("npc", "enemy", "character", "creature", "ally", "boss")


def roster_violations(spec: BuildSpec) -> list[str]:
    """Every way the NPC roster breaks its rules, in words the planner can
    act on. Empty when it is sound."""

    problems: list[str] = []
    assets = {a.id: a for a in spec.assets}
    seen: dict[str, str] = {}
    for fam in spec.npc_roster:
        want = fam.required_variant_count()
        have = len(fam.variants)
        if have != want:
            problems.append(
                f"family '{fam.family_id}' is {fam.count_reading.value} and needs"
                f" exactly {want} variant(s), not {have}"
            )
        if fam.count_reading.value == "explicit_types" and not fam.explicit_count:
            problems.append(
                f"family '{fam.family_id}' is explicit_types but has no explicit_count"
            )
        for v in fam.variants:
            if v.asset_id in seen:
                problems.append(
                    f"asset '{v.asset_id}' is a variant of both '{seen[v.asset_id]}'"
                    f" and '{fam.family_id}'; every variant is its own model"
                )
            seen[v.asset_id] = fam.family_id
            asset = assets.get(v.asset_id)
            if asset is None:
                problems.append(
                    f"variant '{v.asset_id}' of '{fam.family_id}' has no AssetEntry"
                )
            elif not any(c in asset.category.lower() for c in _NPC_ASSET_CATEGORIES):
                problems.append(
                    f"variant '{v.asset_id}' has AssetEntry category"
                    f" '{asset.category}', not an NPC/character category"
                )
        if have > 1 and len({v.physique for v in fam.variants}) == 1 and \
                fam.variants[0].physique.value != "custom":
            problems.append(
                f"family '{fam.family_id}' variants all share physique"
                f" '{fam.variants[0].physique.value}'; make them different bodies"
            )
    return problems


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

    system = build_spec_system(state.target_device)
    resp = client.generate_json(prompt=user_prompt, schema=schema, system=system)
    spec = BuildSpec.model_validate(resp.data)
    # The NPC roster decides how many unique NPC models get made, which is
    # both the game's variety and a real cost, so its rules are checked
    # rather than trusted.
    for attempt in range(_ROSTER_RETRIES):
        problems = roster_violations(spec)
        if not problems:
            break
        ctx.emit(
            ProjectStage.build_spec,
            "warning",
            "NPC roster needs revising: " + "; ".join(problems)[:600],
        )
        retry_prompt = (
            user_prompt
            + "\n\nYour previous BuildSpec broke the NPC ROSTER rules:\n"
            + "\n".join(f"  - {p}" for p in problems)
            + "\nReturn the complete BuildSpec again with these fixed (add"
            " the missing variant AssetEntries; keep everything else)."
        )
        resp = client.generate_json(prompt=retry_prompt, schema=schema, system=system)
        spec = BuildSpec.model_validate(resp.data)
    else:
        problems = roster_violations(spec)
        if problems:
            ctx.emit(
                ProjectStage.build_spec,
                "warning",
                "NPC roster still breaks its rules after retries; continuing: "
                + "; ".join(problems)[:600],
            )
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
