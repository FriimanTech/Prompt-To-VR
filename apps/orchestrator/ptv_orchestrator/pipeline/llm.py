"""Pipeline-wide helper for constructing the shared LLM facade.

Every LLM-using stage should build its client through :func:`make_llm` so
the Claude → ChatGPT fallback is applied uniformly.
"""

from __future__ import annotations

from ptv_providers import LlmClient, build_default_llm_client

from ..settings import Settings


def make_llm(settings: Settings) -> LlmClient:
    """Return an :class:`LlmClient` wired from the orchestrator settings.

    The ``*_max_tokens*`` knobs ensure long user prompts (e.g. a
    945-line BuildSpec for a complex VR game) do not silently truncate
    when downstream stages produce large structured artifacts
    (MechanicPlan, SceneSpec, CodeTask).

    The ``openai_*_reasoning_effort`` knobs are what distinguish the two
    fallback tiers, since both now run ``gpt-5.6-sol``.
    """
    return build_default_llm_client(
        anthropic_api_key=settings.anthropic_api_key,
        openai_api_key=settings.openai_api_key,
        claude_hard_model=settings.claude_hard_model,
        claude_light_model=settings.claude_light_model,
        openai_hard_model=settings.openai_hard_model,
        openai_light_model=settings.openai_light_model,
        openai_hard_reasoning_effort=settings.openai_hard_reasoning_effort,
        openai_light_reasoning_effort=settings.openai_light_reasoning_effort,
        claude_max_tokens=settings.claude_max_tokens,
        openai_max_tokens=settings.openai_max_tokens,
        openai_max_tokens_ceiling=settings.openai_max_tokens_ceiling,
        claude_request_timeout_seconds=settings.claude_request_timeout_seconds,
        openai_request_timeout_seconds=settings.openai_request_timeout_seconds,
    )
