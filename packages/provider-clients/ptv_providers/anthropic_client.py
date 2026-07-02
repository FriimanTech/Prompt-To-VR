"""AnthropicClient: structured-output wrapper used for every Claude call.

Verified April 2026:
    * Models: ``claude-opus-4-7`` (hard) and ``claude-sonnet-4-6`` (light).
    * Structured outputs are GA on the Claude API. Pass the schema via
      ``output_config={"format": {"type": "json_schema", "schema": ...}}``.
      No beta headers are required.
    * The Python SDK exposes the Messages API on ``anthropic.Anthropic``; the
      structured-output params are plain kwargs on ``messages.create``.

The wrapper only handles the request/response plumbing + parsing; caller is
responsible for storing artifacts and validating against Pydantic schemas.
"""

from __future__ import annotations

import json
import os
from dataclasses import dataclass
from typing import Any, Iterable, Mapping

from .errors import ProviderCallError, ProviderConfigError


@dataclass(frozen=True)
class AnthropicConfig:
    api_key: str | None = None
    hard_model: str = "claude-opus-4-7"
    light_model: str = "claude-sonnet-4-6"

    # ``max_tokens`` on Claude caps visible output ONLY (no hidden
    # reasoning budget, unlike OpenAI reasoning models). Large structured
    # outputs -- e.g. a MechanicPlan for a 945-line BuildSpec generating
    # 25+ mechanics with detailed CodeTasks -- routinely require more
    # than the previous 8K default. Claude Opus 4.7 supports up to 32K
    # output tokens out of the box, which is the new default.
    max_tokens: int = 32_000

    # Per-request timeout in seconds, passed to ``anthropic.Anthropic(
    # timeout=...)``. The SDK default is 600s (10 minutes); we cap it
    # lower so a stalled connection cannot silently wedge a pipeline
    # stage. Override via ``CLAUDE_REQUEST_TIMEOUT_SECONDS``.
    request_timeout_seconds: float = 300.0

    default_system: str = (
        "You are the backend model for a local-first prompt-to-native-VR"
        " pipeline. Always obey the supplied JSON Schema exactly."
    )

    @classmethod
    def from_env(cls) -> "AnthropicConfig":
        return cls(
            api_key=os.environ.get("ANTHROPIC_API_KEY"),
            hard_model=os.environ.get("CLAUDE_HARD_MODEL", "claude-opus-4-7"),
            light_model=os.environ.get("CLAUDE_LIGHT_MODEL", "claude-sonnet-4-6"),
            max_tokens=_int_env("CLAUDE_MAX_TOKENS", 32_000),
            request_timeout_seconds=_float_env(
                "CLAUDE_REQUEST_TIMEOUT_SECONDS", 300.0
            ),
        )


def _int_env(name: str, default: int) -> int:
    """Parse an integer environment variable, falling back to ``default``
    on missing / empty / non-numeric values."""
    raw = os.environ.get(name)
    if raw is None or raw.strip() == "":
        return default
    try:
        return int(raw)
    except ValueError:
        return default


def _float_env(name: str, default: float) -> float:
    """Parse a float environment variable, falling back to ``default``
    on missing / empty / non-numeric values."""
    raw = os.environ.get(name)
    if raw is None or raw.strip() == "":
        return default
    try:
        return float(raw)
    except ValueError:
        return default


@dataclass
class StructuredCall:
    """Result of a structured-output call."""

    data: Any
    raw_text: str
    model: str
    usage: Mapping[str, Any]


class AnthropicClient:
    """Thin wrapper around ``anthropic.Anthropic`` focused on JSON outputs."""

    def __init__(self, config: AnthropicConfig | None = None) -> None:
        self.config = config or AnthropicConfig.from_env()
        self._sdk = None  # lazily imported

    # ------------------------------------------------------------------
    # Public API
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
        """Run a structured JSON call constrained to ``schema``.

        ``model`` defaults to the configured hard model. Use ``model=self
        .config.light_model`` for routine transformations.
        """

        self._ensure_sdk()
        client = self._make_client()

        used_model = model or self.config.hard_model
        try:
            msg = client.messages.create(
                model=used_model,
                max_tokens=max_tokens or self.config.max_tokens,
                system=system or self.config.default_system,
                messages=[{"role": "user", "content": prompt}],
                temperature=temperature,
                output_config={
                    "format": {
                        "type": "json_schema",
                        "schema": schema,
                    }
                },
            )
        except ProviderCallError:
            raise
        except Exception as e:  # pragma: no cover - network/auth/rate-limit
            # Any runtime failure from the SDK (auth error, rate limit, 5xx,
            # network blip, connection timeout, etc.) becomes a
            # ProviderCallError so LlmClient can trigger the ChatGPT fallback.
            raise ProviderCallError(f"Claude request failed: {e}") from e

        text = _concat_text_blocks(msg.content)
        try:
            data = json.loads(text)
        except json.JSONDecodeError as e:
            raise ProviderCallError(
                f"Claude returned non-JSON under structured outputs: {e}: {text[:200]!r}"
            ) from e
        usage = dict(getattr(msg, "usage", {}) or {})
        return StructuredCall(data=data, raw_text=text, model=used_model, usage=usage)

    def generate_code(
        self,
        *,
        prompt: str,
        system: str | None = None,
        model: str | None = None,
        max_tokens: int | None = None,
        temperature: float = 0.1,
    ) -> StructuredCall:
        """Run a free-form text / code call (no schema)."""

        self._ensure_sdk()
        client = self._make_client()

        used_model = model or self.config.hard_model
        try:
            msg = client.messages.create(
                model=used_model,
                max_tokens=max_tokens or self.config.max_tokens,
                system=system or self.config.default_system,
                messages=[{"role": "user", "content": prompt}],
                temperature=temperature,
            )
        except Exception as e:  # pragma: no cover - network/auth/rate-limit
            raise ProviderCallError(f"Claude request failed: {e}") from e
        text = _concat_text_blocks(msg.content)
        usage = dict(getattr(msg, "usage", {}) or {})
        return StructuredCall(data=None, raw_text=text, model=used_model, usage=usage)

    # ------------------------------------------------------------------
    # Internals
    # ------------------------------------------------------------------
    def _require_key(self) -> str:
        if not self.config.api_key:
            raise ProviderConfigError(
                "ANTHROPIC_API_KEY is not set. Fill it in .env."
            )
        return self.config.api_key

    def _ensure_sdk(self) -> None:
        if self._sdk is not None:
            return
        try:
            import anthropic  # type: ignore
        except Exception as e:  # pragma: no cover - import error
            raise ProviderConfigError(
                "The `anthropic` Python package is not installed."
                " Run `pip install anthropic` or install ptv-provider-clients."
            ) from e
        self._sdk = anthropic

    def _make_client(self) -> Any:
        """Construct an ``anthropic.Anthropic`` client with the configured
        per-request timeout. The SDK's default is 600s (10 minutes); we
        lower it so a stalled connection surfaces quickly instead of
        wedging a pipeline stage.
        """

        timeout = float(self.config.request_timeout_seconds)
        try:
            return self._sdk.Anthropic(
                api_key=self._require_key(), timeout=timeout
            )
        except TypeError:
            # Older SDK without ``timeout`` kwarg; fall back gracefully.
            return self._sdk.Anthropic(api_key=self._require_key())


def _concat_text_blocks(blocks: Iterable[Any]) -> str:
    """Join all text blocks from an Anthropic response content list."""
    out: list[str] = []
    for b in blocks or []:
        t = getattr(b, "type", None) or (b.get("type") if isinstance(b, dict) else None)
        if t == "text":
            txt = getattr(b, "text", None) or (b.get("text") if isinstance(b, dict) else None)
            if txt:
                out.append(txt)
    return "".join(out)
