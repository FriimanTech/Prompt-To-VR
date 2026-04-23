"""Pipeline-wide helper for constructing the shared LLM facade.

Every LLM-using stage should build its client through :func:`make_llm` so
the Claude → ChatGPT fallback is applied uniformly.
"""

from __future__ import annotations

from ptv_providers import LlmClient, build_default_llm_client

from ..settings import Settings


def make_llm(settings: Settings) -> LlmClient:
    """Return an :class:`LlmClient` wired from the orchestrator settings."""
    return build_default_llm_client(
        anthropic_api_key=settings.anthropic_api_key,
        openai_api_key=settings.openai_api_key,
        claude_hard_model=settings.claude_hard_model,
        claude_light_model=settings.claude_light_model,
        openai_hard_model=settings.openai_hard_model,
        openai_light_model=settings.openai_light_model,
    )
