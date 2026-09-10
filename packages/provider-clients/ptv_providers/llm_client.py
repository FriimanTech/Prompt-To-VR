"""LlmClient: transparent Claude → ChatGPT fallback.

The pipeline stages use this facade instead of talking to Anthropic or OpenAI
directly. Behaviour:

1. Try Anthropic (Claude). If the call succeeds, return its result.
2. If Anthropic raises :class:`ProviderConfigError` or
   :class:`ProviderCallError` AND the OpenAI client ``is_available()``,
   log a warning and retry against OpenAI, transparently mapping
   ``claude-opus-4-7`` → the hard tier and
   ``claude-sonnet-4-6`` → the light tier.
3. Otherwise, re-raise the original error.

This keeps the call site stable: stage code always passes Claude model
names; the facade rewrites them if we fall over to OpenAI.

A "tier" is a ``(model, reasoning_effort)`` pair, not just a model name.
Both tiers now run ``gpt-5.6-sol``, so effort is the ONLY thing that
distinguishes the Opus-equivalent path from the Sonnet-equivalent one --
which is why this facade has to forward it. It previously translated the
model name and dropped the effort on the floor, leaving every fallback call
on whatever the model's own default happened to be.
"""

from __future__ import annotations

import logging
from typing import Any, Mapping

from .anthropic_client import AnthropicClient, AnthropicConfig, StructuredCall
from .errors import ProviderCallError, ProviderConfigError
from .openai_client import OpenAIClient, OpenAIConfig


log = logging.getLogger(__name__)


class LlmClient:
    def __init__(
        self,
        anthropic: AnthropicClient | None = None,
        openai: OpenAIClient | None = None,
    ) -> None:
        self.anthropic = anthropic or AnthropicClient()
        self.openai = openai or OpenAIClient()

    # ------------------------------------------------------------------
    # Tier translation
    # ------------------------------------------------------------------
    def _openai_tier_for(
        self, anthropic_model: str | None
    ) -> tuple[str | None, str]:
        """Translate a Claude model id into an OpenAI ``(model, effort)``.

        * If the caller explicitly asked for the configured Anthropic light
          model, use the light tier.
        * Otherwise, use the hard tier.
        * If the caller passed ``None``, pass ``None`` for the model so
          OpenAI's own default (hard model) applies -- but still send the
          hard tier's effort, because "default model" and "no reasoning
          configuration" are not the same thing.
        """
        if anthropic_model is not None and (
            anthropic_model == self.anthropic.config.light_model
        ):
            return (
                self.openai.config.light_model,
                self.openai.config.light_reasoning_effort,
            )
        model = None if anthropic_model is None else self.openai.config.hard_model
        return model, self.openai.config.hard_reasoning_effort

    def _openai_model_for(self, anthropic_model: str | None) -> str | None:
        """Model half of :meth:`_openai_tier_for`. Kept for callers that
        only care which model a Claude id maps to."""
        return self._openai_tier_for(anthropic_model)[0]

    # ------------------------------------------------------------------
    # Public API (matches AnthropicClient / OpenAIClient)
    # ------------------------------------------------------------------
    def generate_json(
        self,
        *,
        prompt: str,
        schema: Mapping[str, Any],
        system: str | None = None,
        model: str | None = None,
        max_tokens: int | None = None,
        temperature: float = 0.0,
    ) -> StructuredCall:
        try:
            return self.anthropic.generate_json(
                prompt=prompt,
                schema=schema,
                system=system,
                model=model,
                max_tokens=max_tokens,
                temperature=temperature,
            )
        except (ProviderConfigError, ProviderCallError) as e:
            if not self.openai.is_available():
                raise
            openai_model, effort = self._openai_tier_for(model)
            log.warning(
                "Claude unavailable (%s); falling back to OpenAI (%s, effort=%s).",
                e,
                openai_model or self.openai.config.hard_model,
                effort,
            )
            return self.openai.generate_json(
                prompt=prompt,
                schema=schema,
                system=system,
                model=openai_model,
                max_tokens=max_tokens,
                temperature=temperature,
                reasoning_effort=effort,
            )

    def generate_code(
        self,
        *,
        prompt: str,
        system: str | None = None,
        model: str | None = None,
        max_tokens: int | None = None,
        temperature: float = 0.1,
    ) -> StructuredCall:
        try:
            return self.anthropic.generate_code(
                prompt=prompt,
                system=system,
                model=model,
                max_tokens=max_tokens,
                temperature=temperature,
            )
        except (ProviderConfigError, ProviderCallError) as e:
            if not self.openai.is_available():
                raise
            openai_model, effort = self._openai_tier_for(model)
            log.warning(
                "Claude unavailable (%s); falling back to OpenAI for code gen "
                "(%s, effort=%s).",
                e,
                openai_model or self.openai.config.hard_model,
                effort,
            )
            return self.openai.generate_code(
                prompt=prompt,
                system=system,
                model=openai_model,
                max_tokens=max_tokens,
                temperature=temperature,
                reasoning_effort=effort,
            )


def build_default_llm_client(
    *,
    anthropic_api_key: str | None,
    openai_api_key: str | None,
    claude_hard_model: str,
    claude_light_model: str,
    openai_hard_model: str,
    openai_light_model: str,
    openai_hard_reasoning_effort: str | None = None,
    openai_light_reasoning_effort: str | None = None,
    claude_max_tokens: int | None = None,
    openai_max_tokens: int | None = None,
    openai_max_tokens_ceiling: int | None = None,
    claude_request_timeout_seconds: float | None = None,
    openai_request_timeout_seconds: float | None = None,
) -> LlmClient:
    """Helper used by the orchestrator to build an LlmClient from settings.

    The ``*_max_tokens*`` knobs flow into the underlying provider
    configs. When omitted, the per-provider dataclass defaults apply
    (96K for OpenAI, 32K for Claude, 128K ceiling for the OpenAI
    auto-retry).

    ``*_request_timeout_seconds`` cap each HTTP request so a stalled
    connection cannot wedge a pipeline stage. The SDK defaults are
    600s (10 min); we use 300s by default. ``None`` means "use the
    provider config default" (which itself respects the environment
    variable when present).
    """

    anthropic_kwargs: dict[str, Any] = {
        "api_key": anthropic_api_key,
        "hard_model": claude_hard_model,
        "light_model": claude_light_model,
    }
    if claude_max_tokens is not None:
        anthropic_kwargs["max_tokens"] = int(claude_max_tokens)
    if claude_request_timeout_seconds is not None:
        anthropic_kwargs["request_timeout_seconds"] = float(
            claude_request_timeout_seconds
        )

    openai_kwargs: dict[str, Any] = {
        "api_key": openai_api_key,
        "hard_model": openai_hard_model,
        "light_model": openai_light_model,
    }
    if openai_hard_reasoning_effort is not None:
        openai_kwargs["hard_reasoning_effort"] = openai_hard_reasoning_effort
    if openai_light_reasoning_effort is not None:
        openai_kwargs["light_reasoning_effort"] = openai_light_reasoning_effort
    if openai_max_tokens is not None:
        openai_kwargs["max_tokens"] = int(openai_max_tokens)
    if openai_max_tokens_ceiling is not None:
        openai_kwargs["max_tokens_ceiling"] = int(openai_max_tokens_ceiling)
    if openai_request_timeout_seconds is not None:
        openai_kwargs["request_timeout_seconds"] = float(
            openai_request_timeout_seconds
        )

    return LlmClient(
        anthropic=AnthropicClient(AnthropicConfig(**anthropic_kwargs)),
        openai=OpenAIClient(OpenAIConfig(**openai_kwargs)),
    )
