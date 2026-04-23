"""LlmClient: transparent Claude → ChatGPT fallback.

The pipeline stages use this facade instead of talking to Anthropic or OpenAI
directly. Behaviour:

1. Try Anthropic (Claude). If the call succeeds, return its result.
2. If Anthropic raises :class:`ProviderConfigError` or
   :class:`ProviderCallError` AND the OpenAI client ``is_available()``,
   log a warning and retry against OpenAI, transparently mapping
   ``claude-opus-4-7`` → ``OPENAI_HARD_MODEL`` and
   ``claude-sonnet-4-6`` → ``OPENAI_LIGHT_MODEL``.
3. Otherwise, re-raise the original error.

This keeps the call site stable: stage code always passes Claude model
names; the facade rewrites them if we fall over to OpenAI.
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
    # Model name translation
    # ------------------------------------------------------------------
    def _openai_model_for(self, anthropic_model: str | None) -> str | None:
        """Translate a Claude model id into an OpenAI model id.

        * If the caller explicitly asked for the configured Anthropic light
          model, use the OpenAI light model.
        * Otherwise, use the OpenAI hard model.
        * If the caller passed ``None``, pass ``None`` through so OpenAI's
          own default (hard model) applies.
        """
        if anthropic_model is None:
            return None
        if anthropic_model == self.anthropic.config.light_model:
            return self.openai.config.light_model
        return self.openai.config.hard_model

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
            log.warning("Claude unavailable (%s); falling back to OpenAI.", e)
            return self.openai.generate_json(
                prompt=prompt,
                schema=schema,
                system=system,
                model=self._openai_model_for(model),
                max_tokens=max_tokens,
                temperature=temperature,
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
            log.warning(
                "Claude unavailable (%s); falling back to OpenAI for code gen.", e
            )
            return self.openai.generate_code(
                prompt=prompt,
                system=system,
                model=self._openai_model_for(model),
                max_tokens=max_tokens,
                temperature=temperature,
            )


def build_default_llm_client(
    *,
    anthropic_api_key: str | None,
    openai_api_key: str | None,
    claude_hard_model: str,
    claude_light_model: str,
    openai_hard_model: str,
    openai_light_model: str,
) -> LlmClient:
    """Helper used by the orchestrator to build an LlmClient from settings."""
    return LlmClient(
        anthropic=AnthropicClient(
            AnthropicConfig(
                api_key=anthropic_api_key,
                hard_model=claude_hard_model,
                light_model=claude_light_model,
            )
        ),
        openai=OpenAIClient(
            OpenAIConfig(
                api_key=openai_api_key,
                hard_model=openai_hard_model,
                light_model=openai_light_model,
            )
        ),
    )
