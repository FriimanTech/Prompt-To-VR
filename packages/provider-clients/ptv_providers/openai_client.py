"""OpenAIClient: ChatGPT fallback for Claude.

Verified September 2026:
    * Both tiers are ``gpt-5.6-sol``: the GPT-5.6 flagship, ~1.05M context
      and 128K max output. The tiers differ by REASONING EFFORT rather than
      by model -- ``hard_reasoning_effort`` for the Opus-equivalent path and
      ``light_reasoning_effort`` for the Sonnet-equivalent one.
    * ``reasoning.effort`` on 5.6 accepts ``none | low | medium (default) |
      high | xhigh | max``. ``minimal`` was valid on gpt-5 and is GONE in
      5.6, so passing it now returns a 400.
    * Python SDK: ``openai`` (``pip install openai``).
    * Structured outputs: we use ``response_format={"type": "json_object"}``
      plus the JSON Schema embedded in the system prompt. The caller
      re-validates with Pydantic, which matches how the Anthropic client
      behaves.

This client talks to **Chat Completions**, which is fine for 5.6 because we
send no tools. OpenAI's 5.6 guidance is that reasoning combined with
*function tools* requires the Responses API; the tool-using stages
(world generation, codegen) go through the Agents SDK, which uses Responses.

Reasoning models (``gpt-5``, ``gpt-5-mini``, ``gpt-5-nano``, ``o1``, ``o3``,
``o4*``, and ``gpt-5-codex``) REJECT the following sampling parameters when
called through Chat Completions: ``temperature``, ``top_p``, ``presence_penalty``,
``frequency_penalty``, ``logprobs``. The OpenAI API returns HTTP 400 with
``Unsupported value: 'temperature' does not support X with this model. Only
the default (1) value is supported.`` if any of these are present. We detect
reasoning model names and OMIT those kwargs automatically so calling code can
keep passing ``temperature=0.0`` uniformly (as it does for Claude) without
caring about the target model.

The wrapper mirrors :class:`AnthropicClient` so a facade can swap them.
"""

from __future__ import annotations

import json
import os
import re
from dataclasses import dataclass
from typing import Any, Mapping

from .anthropic_client import StructuredCall
from .errors import ProviderCallError, ProviderConfigError


# Regex that matches every OpenAI model family that rejects sampling params
# on Chat Completions. Matches at the START of the model id, so dated
# snapshots (e.g. ``gpt-5-mini-2025-08-07``) and Azure deployments named after
# the base model work too. ``gpt-5-chat-latest`` is intentionally EXCLUDED --
# it is the non-reasoning GPT-5 variant and DOES accept temperature/top_p.
_REASONING_MODEL_RE = re.compile(
    r"^(?:"
    r"o1|o3|o4"                    # o-series reasoning models
    r"|gpt-5(?!-chat)"             # gpt-5, gpt-5-mini, gpt-5-nano, gpt-5-codex, gpt-5.x
    r")",
    re.IGNORECASE,
)


def is_reasoning_model(model: str | None) -> bool:
    """Return True if the given OpenAI model rejects sampling params.

    Reasoning models (gpt-5 family sans ``gpt-5-chat*``, plus o1/o3/o4) only
    support the default ``temperature=1`` / ``top_p=1``. Everything else on
    Chat Completions is fine to pass.
    """
    if not model:
        return False
    return bool(_REASONING_MODEL_RE.match(model.strip()))


@dataclass(frozen=True)
class OpenAIConfig:
    api_key: str | None = None
    hard_model: str = "gpt-5.6-sol"
    light_model: str = "gpt-5.6-sol"

    # Per-tier reasoning effort. With both tiers on the same model this is
    # what actually distinguishes them, so it is not optional bookkeeping.
    # ``light`` is ``medium`` rather than ``low`` because the light tier
    # serves asset_manifest, the call that decides which assets get rigged
    # and animated -- i.e. the one that spends Tripo credits.
    hard_reasoning_effort: str = "high"
    light_reasoning_effort: str = "medium"

    # Reasoning models (gpt-5, o1/o3/o4, gpt-5-codex, ...) spend tokens on
    # invisible "reasoning" output BEFORE they emit the final answer.
    # ``max_completion_tokens`` caps the TOTAL of reasoning + visible
    # output, so a low cap silently truncates large structured outputs
    # (e.g. a MechanicPlan for a 945-line BuildSpec).
    #
    # OpenAI explicitly recommends reserving at least 25K tokens for any
    # reasoning-model call. We default to 96K so the typical large
    # BuildSpec / MechanicPlan / SceneSpec call has plenty of headroom
    # for both reasoning AND the visible JSON. For plain non-reasoning
    # models this is just a high ceiling that is almost never hit.
    max_tokens: int = 96_000

    # Hard upper bound on the auto-retry doubling logic in
    # ``OpenAIClient._call_with_length_retry``. 128K is exactly
    # ``gpt-5.6-sol``'s documented max output, so the ceiling guards against
    # runaway costs and matches the model's hard limit.
    max_tokens_ceiling: int = 128_000

    # Per-request timeout in seconds, passed to ``openai.OpenAI(timeout=...)``.
    # A reasoning model on a complex schema can legitimately take 60-90s
    # so the default is generous. The default SDK timeout (10 minutes)
    # is what made a stalled scene_spec call appear hung indefinitely:
    # with no progress feedback in the UI, 10 minutes of waiting reads
    # as "frozen". Override via ``OPENAI_REQUEST_TIMEOUT_SECONDS`` env
    # var when running on a flaky network or behind a slow proxy.
    request_timeout_seconds: float = 300.0

    # Number of times the OpenAI SDK silently re-sends a request on a
    # timeout / transient error. The SDK default is 2, which means a
    # single ``request_timeout_seconds`` timeout is amplified into THREE
    # attempts (e.g. 3 x 300s = ~15 minutes of apparent hang before the
    # error finally surfaces -- exactly the scene_spec symptom). We
    # default to 0 so a timeout fails fast and the calling stage can
    # decide how to recover. Override via ``OPENAI_MAX_RETRIES``.
    max_retries: int = 0

    default_system: str = (
        "You are the backend model for a local-first prompt-to-native-VR"
        " pipeline. Always obey the supplied JSON Schema exactly."
    )

    @classmethod
    def from_env(cls) -> "OpenAIConfig":
        return cls(
            api_key=os.environ.get("OPENAI_API_KEY"),
            hard_model=os.environ.get("OPENAI_HARD_MODEL", "gpt-5.6-sol"),
            light_model=os.environ.get("OPENAI_LIGHT_MODEL", "gpt-5.6-sol"),
            hard_reasoning_effort=os.environ.get(
                "OPENAI_HARD_REASONING_EFFORT", "high"
            ),
            light_reasoning_effort=os.environ.get(
                "OPENAI_LIGHT_REASONING_EFFORT", "medium"
            ),
            max_tokens=_int_env("OPENAI_MAX_TOKENS", 96_000),
            max_tokens_ceiling=_int_env(
                "OPENAI_MAX_TOKENS_CEILING", 128_000
            ),
            request_timeout_seconds=_float_env(
                "OPENAI_REQUEST_TIMEOUT_SECONDS", 300.0
            ),
            max_retries=_int_env("OPENAI_MAX_RETRIES", 0),
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


class OpenAIClient:
    """Thin wrapper around ``openai.OpenAI`` focused on JSON outputs."""

    def __init__(self, config: OpenAIConfig | None = None) -> None:
        self.config = config or OpenAIConfig.from_env()
        self._sdk = None  # lazily imported

    # ------------------------------------------------------------------
    # Public API (matches AnthropicClient)
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
        reasoning_effort: str | None = None,
    ) -> StructuredCall:
        """Run a JSON-mode call; schema is embedded in the system prompt.

        Caller is expected to re-validate the returned ``data`` against a
        Pydantic model, which catches any drift in practice.

        ``reasoning_effort`` (on gpt-5.6: ``none`` | ``low`` | ``medium`` |
        ``high`` | ``xhigh`` | ``max``) is passed through only for reasoning
        models that accept it. A lower effort dramatically cuts wall-clock on
        mapping-style tasks (where the model does not need deep
        chain-of-thought), which is the main lever against timeouts on large
        structured outputs.
        """

        self._ensure_sdk()
        client = self._make_client()
        used_model = model or self.config.hard_model

        base_system = system or self.config.default_system
        composed_system = _with_schema_hint(base_system, schema)

        kwargs: dict[str, Any] = {
            "model": used_model,
            "messages": [
                {"role": "system", "content": composed_system},
                {"role": "user", "content": prompt},
            ],
            "response_format": {"type": "json_object"},
        }
        # Reasoning models (gpt-5 family + o-series) reject sampling params.
        # Only pass temperature when the target model supports it.
        if not is_reasoning_model(used_model):
            kwargs["temperature"] = temperature
        elif reasoning_effort:
            # Only reasoning models accept ``reasoning_effort`` on Chat
            # Completions. Non-reasoning models would 400 on it.
            kwargs["reasoning_effort"] = reasoning_effort

        resp = self._call_with_length_retry(
            client,
            base_kwargs=kwargs,
            initial_cap=max_tokens or self.config.max_tokens,
        )

        text = _extract_text(resp)
        try:
            data = json.loads(text)
        except json.JSONDecodeError as e:
            raise ProviderCallError(
                f"OpenAI returned non-JSON under JSON mode: {e}: {text[:200]!r}"
            ) from e
        usage = dict(getattr(resp, "usage", {}) or {})
        return StructuredCall(data=data, raw_text=text, model=used_model, usage=usage)

    def generate_code(
        self,
        *,
        prompt: str,
        system: str | None = None,
        model: str | None = None,
        max_tokens: int | None = None,
        temperature: float = 0.1,
        reasoning_effort: str | None = None,
    ) -> StructuredCall:
        """Free-form text / code call (no schema).

        ``reasoning_effort`` behaves exactly as in :meth:`generate_json`.
        It matters more here, not less: writing code is the workload that
        benefits most from reasoning, and omitting the parameter leaves the
        model on its own default.
        """

        self._ensure_sdk()
        client = self._make_client()
        used_model = model or self.config.hard_model

        kwargs: dict[str, Any] = {
            "model": used_model,
            "messages": [
                {"role": "system", "content": system or self.config.default_system},
                {"role": "user", "content": prompt},
            ],
        }
        if not is_reasoning_model(used_model):
            kwargs["temperature"] = temperature
        elif reasoning_effort:
            kwargs["reasoning_effort"] = reasoning_effort

        resp = self._call_with_length_retry(
            client,
            base_kwargs=kwargs,
            initial_cap=max_tokens or self.config.max_tokens,
        )

        text = _extract_text(resp)
        usage = dict(getattr(resp, "usage", {}) or {})
        return StructuredCall(data=None, raw_text=text, model=used_model, usage=usage)

    # ------------------------------------------------------------------
    # Introspection helpers
    # ------------------------------------------------------------------
    def is_available(self) -> bool:
        """True if both SDK and API key look usable (cheap check, no network)."""
        if not self.config.api_key:
            return False
        try:
            self._ensure_sdk()
        except ProviderConfigError:
            return False
        return True

    # ------------------------------------------------------------------
    # Internals
    # ------------------------------------------------------------------
    def _require_key(self) -> str:
        if not self.config.api_key:
            raise ProviderConfigError(
                "OPENAI_API_KEY is not set. Fill it in .env."
            )
        return self.config.api_key

    def _ensure_sdk(self) -> None:
        if self._sdk is not None:
            return
        try:
            import openai  # type: ignore
        except Exception as e:  # pragma: no cover - import error
            raise ProviderConfigError(
                "The `openai` Python package is not installed."
                " Run `pip install openai` or install ptv-provider-clients."
            ) from e
        self._sdk = openai

    def _make_client(self) -> Any:
        """Construct an ``openai.OpenAI`` client with the configured
        per-request timeout AND retry budget. The SDK's defaults are
        600s (10 minutes) timeout and 2 retries, which together let a
        single stalled call hang for ~30 minutes with no feedback. We
        lower the timeout and set ``max_retries`` (default 0) so a
        stalled connection fails fast instead of silently re-sending
        the same expensive request. Callers can override via
        ``OPENAI_REQUEST_TIMEOUT_SECONDS`` / ``OPENAI_MAX_RETRIES``.
        """

        timeout = float(self.config.request_timeout_seconds)
        max_retries = int(self.config.max_retries)
        try:
            return self._sdk.OpenAI(
                api_key=self._require_key(),
                timeout=timeout,
                max_retries=max_retries,
            )
        except TypeError:
            # Very old SDK versions don't accept ``timeout`` / ``max_retries``
            # here. Fall back to the default (no timeout) so the call site
            # still works; the user gets the slow-hang behavior, which is the
            # pre-fix status quo rather than a crash.
            return self._sdk.OpenAI(api_key=self._require_key())

    # ------------------------------------------------------------------
    # Length-aware retry
    # ------------------------------------------------------------------
    def _call_with_length_retry(
        self,
        client: Any,
        *,
        base_kwargs: dict[str, Any],
        initial_cap: int,
    ) -> Any:
        """Call ``client.chat.completions.create`` with automatic retry on
        ``finish_reason='length'``.

        Reasoning models (gpt-5 family, o-series) spend tokens on hidden
        reasoning output BEFORE emitting visible text. When the
        ``max_completion_tokens`` cap is hit by the combined reasoning +
        output budget, the response comes back with ``finish_reason=
        'length'`` and (usually) empty visible text. The legacy behaviour
        was to raise immediately; that meant a single under-sized cap
        wedged the entire pipeline.

        The new behaviour: on truncation we DOUBLE the cap and retry, up
        to ``OpenAIConfig.max_tokens_ceiling`` (default 128K, the
        per-call max for ``gpt-5``). If the ceiling itself truncates,
        we raise a clear error that points the user at
        ``OPENAI_MAX_TOKENS`` / ``OpenAIConfig.max_tokens`` and the
        schema-complexity knob.

        ``content_filter`` finish reasons always raise (no retry).
        """

        ceiling = max(initial_cap, int(self.config.max_tokens_ceiling))
        cap = max(1024, min(int(initial_cap), ceiling))

        while True:
            kwargs = {**base_kwargs, "max_completion_tokens": cap}
            try:
                resp = client.chat.completions.create(**kwargs)
            except Exception as e:  # pragma: no cover - network error
                raise ProviderCallError(f"OpenAI request failed: {e}") from e

            reason = _finish_reason(resp)
            if reason == "content_filter":
                raise ProviderCallError(
                    "OpenAI response blocked by content filter."
                )
            if reason != "length":
                # Either 'stop' (clean finish), 'tool_calls', or some other
                # non-truncation reason -- return and let the caller parse.
                return resp

            # Truncated. Retry with a bigger budget if we still have
            # headroom; otherwise surface a clear error.
            if cap >= ceiling:
                raise ProviderCallError(
                    "OpenAI response was truncated at "
                    f"max_completion_tokens={cap} (the configured "
                    "ceiling). Raise OpenAIConfig.max_tokens_ceiling or "
                    "OPENAI_MAX_TOKENS_CEILING above "
                    f"{ceiling}, raise OPENAI_MAX_TOKENS, or simplify "
                    "the prompt / schema. Reasoning models like gpt-5 "
                    "spend reasoning tokens from this budget BEFORE "
                    "emitting visible output, which is why even a "
                    "large cap can run out on a complex schema."
                )
            cap = min(cap * 2, ceiling)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _with_schema_hint(system: str, schema: Mapping[str, Any]) -> str:
    """Append the JSON Schema to the system prompt so the model obeys it.

    JSON mode (``response_format={"type": "json_object"}``) guarantees valid
    JSON. The schema embedded here tells the model what shape to produce.
    The caller re-validates with Pydantic, which catches drift.
    """

    pretty = json.dumps(schema, indent=2, sort_keys=True)
    return (
        f"{system.strip()}\n\n"
        "You MUST reply with a single JSON object that conforms EXACTLY to"
        " the following JSON Schema. Do not include any prose, markdown"
        " fences, or commentary. Only output the JSON.\n\n"
        f"```json\n{pretty}\n```"
    )


def _finish_reason(resp: Any) -> str | None:
    """Return the ``finish_reason`` of the first choice (or ``None`` if
    the response shape is unrecognised).

    The retry helper uses this to decide whether to widen the
    ``max_completion_tokens`` budget. Unlike the prior ``_check_finish``
    helper, this does NOT raise on truncation -- truncation handling
    lives in :py:meth:`OpenAIClient._call_with_length_retry`.
    """

    try:
        choice = resp.choices[0]
    except Exception:
        return None
    return getattr(choice, "finish_reason", None)


def _extract_text(resp: Any) -> str:
    """Pull the single-message text out of a Chat Completion response."""
    try:
        choice = resp.choices[0]
        msg = choice.message
        if getattr(msg, "refusal", None):
            raise ProviderCallError(f"OpenAI refused: {msg.refusal}")
        content = getattr(msg, "content", None)
        if isinstance(content, str):
            return content
        if isinstance(content, list):
            parts: list[str] = []
            for part in content:
                if isinstance(part, dict) and part.get("type") == "text":
                    parts.append(part.get("text", ""))
                else:
                    t = getattr(part, "text", None)
                    if isinstance(t, str):
                        parts.append(t)
            return "".join(parts)
    except ProviderCallError:
        raise
    except Exception as e:  # pragma: no cover - unexpected shape
        raise ProviderCallError(f"Unexpected OpenAI response shape: {e}") from e
    raise ProviderCallError("Empty OpenAI response")
