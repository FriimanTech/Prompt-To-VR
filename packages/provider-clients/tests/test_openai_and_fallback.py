"""Tests for OpenAIClient + LlmClient (Claude → ChatGPT) fallback."""

from __future__ import annotations

import pytest

from ptv_providers import (
    AnthropicClient,
    AnthropicConfig,
    LlmClient,
    OpenAIClient,
    OpenAIConfig,
    ProviderCallError,
    ProviderConfigError,
)
from ptv_providers.openai_client import is_reasoning_model


# ---------------------------------------------------------------------------
# Fake OpenAI SDK
# ---------------------------------------------------------------------------

class _FakeMessage:
    def __init__(self, content: str):
        self.content = content
        self.refusal = None


class _FakeChoice:
    def __init__(self, content: str, finish_reason: str = "stop"):
        self.message = _FakeMessage(content)
        self.finish_reason = finish_reason


class _FakeResp:
    def __init__(self, content: str, finish_reason: str = "stop"):
        self.choices = [_FakeChoice(content, finish_reason)]
        self.usage = {"prompt_tokens": 1, "completion_tokens": 2}


class _FakeChatCompletions:
    """Records every call and replays a scripted sequence of responses.

    ``script`` is a list of ``(content, finish_reason)`` tuples. When the
    test only needs one fixed response, the legacy positional args
    ``(content, finish_reason)`` are still supported -- those build a
    one-element script that is replayed for every call (so retry-loop
    tests get a stable response on every attempt).
    """

    def __init__(
        self,
        content: str,
        finish_reason: str = "stop",
        *,
        script: list[tuple[str, str]] | None = None,
    ):
        self._script = list(script) if script is not None else [(content, finish_reason)]
        self.last_kwargs: dict = {}
        self.call_kwargs: list[dict] = []
        self.calls = 0

    def create(self, **kwargs):
        self.last_kwargs = kwargs
        self.call_kwargs.append(kwargs)
        self.calls += 1
        # Replay the next scripted response, falling back to the last
        # entry once the script is exhausted (so length-loop tests don't
        # IndexError when the auto-retry doubles past the ceiling).
        idx = min(self.calls - 1, len(self._script) - 1)
        content, finish_reason = self._script[idx]
        return _FakeResp(content, finish_reason)


class _FakeOpenAISdk:
    """Mimics the bits of the ``openai`` module that the client touches."""

    def __init__(
        self,
        content: str,
        finish_reason: str = "stop",
        *,
        script: list[tuple[str, str]] | None = None,
    ):
        self._content = content
        self._finish_reason = finish_reason
        self._script = script
        self.last_instance: "_FakeOpenAISdk._Instance" | None = None

        outer = self

        class _Instance:
            def __init__(self, api_key=None):
                self.api_key = api_key
                self.chat = type(
                    "_Chat",
                    (),
                    {
                        "completions": _FakeChatCompletions(
                            outer._content,
                            outer._finish_reason,
                            script=outer._script,
                        )
                    },
                )()
                outer.last_instance = self

        self.OpenAI = _Instance


# ---------------------------------------------------------------------------
# OpenAIClient
# ---------------------------------------------------------------------------

def test_openai_missing_key_raises():
    c = OpenAIClient(OpenAIConfig(api_key=None))
    c._sdk = _FakeOpenAISdk('{"ok": true}')
    with pytest.raises(ProviderConfigError):
        c.generate_json(prompt="x", schema={"type": "object"})


def test_openai_generate_json_parses_and_embeds_schema():
    c = OpenAIClient(OpenAIConfig(api_key="sk-test", hard_model="gpt-5"))
    sdk = _FakeOpenAISdk('{"foo": 1}')
    c._sdk = sdk
    result = c.generate_json(
        prompt="hi",
        schema={"type": "object", "properties": {"foo": {"type": "integer"}}},
    )
    assert result.data == {"foo": 1}
    assert result.model == "gpt-5"
    kw = sdk.last_instance.chat.completions.last_kwargs
    assert kw["model"] == "gpt-5"
    assert kw["response_format"] == {"type": "json_object"}
    # The schema is embedded into the system prompt.
    system_msg = kw["messages"][0]
    assert system_msg["role"] == "system"
    assert '"foo"' in system_msg["content"]
    assert '"type": "integer"' in system_msg["content"]


def test_openai_generate_json_rejects_bad_json():
    c = OpenAIClient(OpenAIConfig(api_key="sk-test"))
    c._sdk = _FakeOpenAISdk("not json")
    with pytest.raises(ProviderCallError):
        c.generate_json(prompt="x", schema={"type": "object"})


def test_openai_generate_code_uses_light_model_when_passed():
    c = OpenAIClient(OpenAIConfig(api_key="sk-test", light_model="gpt-5-mini"))
    sdk = _FakeOpenAISdk("// some code")
    c._sdk = sdk
    r = c.generate_code(prompt="write cpp", model=c.config.light_model)
    assert r.model == "gpt-5-mini"
    kw = sdk.last_instance.chat.completions.last_kwargs
    assert kw["model"] == "gpt-5-mini"
    assert "response_format" not in kw


def test_openai_omits_temperature_for_reasoning_models():
    """gpt-5 et al. reject ``temperature != 1`` with HTTP 400. The client
    must OMIT the kwarg entirely for these models."""
    c = OpenAIClient(OpenAIConfig(api_key="sk-test", hard_model="gpt-5"))
    sdk = _FakeOpenAISdk('{"ok": true}')
    c._sdk = sdk
    c.generate_json(prompt="hi", schema={"type": "object"}, temperature=0.0)
    kw = sdk.last_instance.chat.completions.last_kwargs
    assert "temperature" not in kw, (
        "temperature must not be passed to reasoning models (gpt-5 family)"
    )


def test_openai_keeps_temperature_for_chat_latest():
    c = OpenAIClient(OpenAIConfig(api_key="sk-test", hard_model="gpt-5-chat-latest"))
    sdk = _FakeOpenAISdk('{"ok": true}')
    c._sdk = sdk
    c.generate_json(prompt="hi", schema={"type": "object"}, temperature=0.2)
    kw = sdk.last_instance.chat.completions.last_kwargs
    assert kw.get("temperature") == 0.2


def test_openai_generate_code_also_omits_temperature_for_reasoning():
    c = OpenAIClient(OpenAIConfig(api_key="sk-test", hard_model="gpt-5"))
    sdk = _FakeOpenAISdk("// code")
    c._sdk = sdk
    c.generate_code(prompt="write cpp", temperature=0.1)
    kw = sdk.last_instance.chat.completions.last_kwargs
    assert "temperature" not in kw


def test_openai_reports_length_truncation_after_exhausting_ceiling():
    """When EVERY retry attempt also truncates, the client surfaces a
    clear error that mentions the ceiling and the env knob, instead of
    looping forever."""
    c = OpenAIClient(
        OpenAIConfig(
            api_key="sk-test",
            max_tokens=4096,
            max_tokens_ceiling=8192,
        )
    )
    sdk = _FakeOpenAISdk("", finish_reason="length")
    c._sdk = sdk
    with pytest.raises(ProviderCallError, match="truncated"):
        c.generate_json(prompt="x", schema={"type": "object"})

    cc = sdk.last_instance.chat.completions
    # We expected attempts at 4096 then 8192 (doubled, clamped to
    # ceiling), then we raise -- so exactly 2 SDK calls.
    assert cc.calls == 2, f"expected 2 retries, got {cc.calls}"
    caps = [k.get("max_completion_tokens") for k in cc.call_kwargs]
    assert caps == [4096, 8192], caps


def test_openai_retries_on_length_then_succeeds():
    """If the FIRST attempt truncates but a doubled retry succeeds, the
    caller should get the parsed result transparently -- no error."""
    c = OpenAIClient(
        OpenAIConfig(
            api_key="sk-test",
            max_tokens=8192,
            max_tokens_ceiling=65536,
        )
    )
    sdk = _FakeOpenAISdk(
        "",  # ignored when script is set
        script=[
            ("", "length"),  # first attempt: truncated
            ('{"ok": true}', "stop"),  # retry succeeds
        ],
    )
    c._sdk = sdk
    result = c.generate_json(prompt="x", schema={"type": "object"})

    assert result.data == {"ok": True}
    cc = sdk.last_instance.chat.completions
    assert cc.calls == 2
    caps = [k.get("max_completion_tokens") for k in cc.call_kwargs]
    assert caps == [8192, 16384], caps  # doubled on retry


def test_openai_no_retry_when_first_attempt_succeeds():
    """Sanity: the retry loop never re-calls when the first attempt
    finishes cleanly, so we don't waste tokens / latency."""
    c = OpenAIClient(OpenAIConfig(api_key="sk-test", max_tokens=8192))
    sdk = _FakeOpenAISdk('{"ok": true}')  # default finish_reason="stop"
    c._sdk = sdk
    c.generate_json(prompt="x", schema={"type": "object"})

    cc = sdk.last_instance.chat.completions
    assert cc.calls == 1
    assert cc.call_kwargs[0]["max_completion_tokens"] == 8192


def test_openai_per_call_max_tokens_overrides_config():
    """Stages that need an unusually large budget can pass
    ``max_tokens=...`` per call."""
    c = OpenAIClient(OpenAIConfig(api_key="sk-test", max_tokens=8192))
    sdk = _FakeOpenAISdk('{"ok": true}')
    c._sdk = sdk
    c.generate_json(
        prompt="x",
        schema={"type": "object"},
        max_tokens=42_000,
    )
    cc = sdk.last_instance.chat.completions
    assert cc.call_kwargs[0]["max_completion_tokens"] == 42_000


def test_openai_content_filter_does_not_retry():
    """``content_filter`` is a policy decision -- retrying with more
    tokens won't help, so we raise immediately without burning more
    calls."""
    c = OpenAIClient(OpenAIConfig(api_key="sk-test"))
    sdk = _FakeOpenAISdk("", finish_reason="content_filter")
    c._sdk = sdk
    with pytest.raises(ProviderCallError, match="content filter"):
        c.generate_json(prompt="x", schema={"type": "object"})
    assert sdk.last_instance.chat.completions.calls == 1


def test_openai_default_config_has_generous_max_tokens():
    """Regression guard: the default cap MUST be high enough for
    reasoning models. OpenAI explicitly recommends >= 25K, and our
    large-prompt MechanicPlan calls easily blow past 16K. The previous
    16384 default broke real 900+ line user prompts in production."""
    cfg = OpenAIConfig()
    assert cfg.max_tokens >= 64_000, (
        f"default max_tokens={cfg.max_tokens} too low for gpt-5 "
        f"reasoning models -- raise to at least 64K"
    )
    assert cfg.max_tokens_ceiling >= cfg.max_tokens
    assert cfg.max_tokens_ceiling >= 96_000


@pytest.mark.parametrize(
    "model,expected",
    [
        ("gpt-5", True),
        ("gpt-5-mini", True),
        ("gpt-5-nano", True),
        ("gpt-5-2025-08-07", True),
        ("gpt-5-mini-2025-08-07", True),
        ("gpt-5-codex", True),
        ("o1", True),
        ("o1-mini", True),
        ("o3", True),
        ("o4-mini", True),
        ("gpt-5-chat-latest", False),
        ("gpt-4o", False),
        ("gpt-4o-mini", False),
        ("gpt-4.1", False),
        (None, False),
        ("", False),
    ],
)
def test_is_reasoning_model_detection(model, expected):
    assert is_reasoning_model(model) is expected


def test_openai_is_available_requires_key_and_sdk():
    c = OpenAIClient(OpenAIConfig(api_key=None))
    c._sdk = _FakeOpenAISdk("x")
    assert c.is_available() is False

    c2 = OpenAIClient(OpenAIConfig(api_key="sk"))
    c2._sdk = _FakeOpenAISdk("x")
    assert c2.is_available() is True


# ---------------------------------------------------------------------------
# LlmClient fallback
# ---------------------------------------------------------------------------


class _FakeAnthropicSdk:
    """Minimal fake that lets us simulate missing / broken Claude."""

    def __init__(self, response_text: str | None = None, *, raise_on_create: Exception | None = None):
        self._text = response_text or "{}"
        self._raise = raise_on_create
        self.calls = 0

        parent = self

        class _Block:
            def __init__(self, text):
                self.type = "text"
                self.text = text

        class _Msg:
            def __init__(self, text):
                self.content = [_Block(text)]
                self.usage = {"input_tokens": 1, "output_tokens": 2}

        class _Messages:
            def create(self, **kwargs):
                parent.calls += 1
                if parent._raise is not None:
                    raise parent._raise
                return _Msg(parent._text)

        class _Instance:
            def __init__(self, api_key=None):
                self.api_key = api_key
                self.messages = _Messages()

        self.Anthropic = _Instance


def _llm(
    *,
    claude_text: str | None = None,
    claude_raises: Exception | None = None,
    claude_key: str | None = "sk-ant",
    openai_text: str = '{"ok": true}',
    openai_key: str | None = "sk-test",
) -> tuple[LlmClient, _FakeAnthropicSdk, _FakeOpenAISdk]:
    a_sdk = _FakeAnthropicSdk(claude_text, raise_on_create=claude_raises)
    o_sdk = _FakeOpenAISdk(openai_text)
    a = AnthropicClient(AnthropicConfig(api_key=claude_key, hard_model="claude-opus-4-7", light_model="claude-sonnet-4-6"))
    a._sdk = a_sdk
    o = OpenAIClient(OpenAIConfig(api_key=openai_key, hard_model="gpt-5", light_model="gpt-5-mini"))
    o._sdk = o_sdk
    return LlmClient(anthropic=a, openai=o), a_sdk, o_sdk


def test_llm_uses_claude_when_available():
    llm, a_sdk, o_sdk = _llm(claude_text='{"answer": 42}')
    r = llm.generate_json(prompt="x", schema={"type": "object"})
    assert r.data == {"answer": 42}
    assert r.model == "claude-opus-4-7"
    # Anthropic called exactly once, OpenAI not at all.
    assert a_sdk.calls == 1


def test_llm_falls_back_to_openai_when_claude_key_missing():
    llm, a_sdk, _ = _llm(claude_key=None, openai_text='{"answer": "gpt"}')
    r = llm.generate_json(prompt="x", schema={"type": "object"})
    assert r.data == {"answer": "gpt"}
    assert r.model == "gpt-5"
    # Anthropic was never hit because the config check short-circuits.
    assert a_sdk.calls == 0


def test_llm_falls_back_when_claude_sdk_raises_auth_error():
    """If Claude's SDK raises (bad key, rate limit, 5xx, network), we must
    still transparently retry against OpenAI rather than crash."""

    class _FakeAuthError(Exception):
        pass

    llm, a_sdk, o_sdk = _llm(
        claude_raises=_FakeAuthError("401 invalid_api_key"),
        openai_text='{"answer": "gpt"}',
    )
    r = llm.generate_json(prompt="x", schema={"type": "object"})
    assert r.data == {"answer": "gpt"}
    assert r.model == "gpt-5"
    # Anthropic WAS invoked once, then the exception was converted into a
    # ProviderCallError, caught by LlmClient, and retried on OpenAI.
    assert a_sdk.calls == 1
    assert o_sdk.last_instance is not None


def test_llm_reraises_when_openai_also_unavailable():
    llm, _, _ = _llm(claude_key=None, openai_key=None)
    with pytest.raises(ProviderConfigError):
        llm.generate_json(prompt="x", schema={"type": "object"})


def test_llm_translates_light_model_on_fallback():
    llm, _, o_sdk = _llm(claude_key=None, openai_text='{"ok": true}')
    llm.generate_json(
        prompt="x",
        schema={"type": "object"},
        model=llm.anthropic.config.light_model,  # "claude-sonnet-4-6"
    )
    kw = o_sdk.last_instance.chat.completions.last_kwargs
    assert kw["model"] == "gpt-5-mini"  # light model translation


def test_llm_translates_hard_model_on_fallback():
    llm, _, o_sdk = _llm(claude_key=None, openai_text='{"ok": true}')
    llm.generate_json(
        prompt="x",
        schema={"type": "object"},
        model=llm.anthropic.config.hard_model,  # "claude-opus-4-7"
    )
    kw = o_sdk.last_instance.chat.completions.last_kwargs
    assert kw["model"] == "gpt-5"


def test_llm_generate_code_fallback_path():
    llm, _, o_sdk = _llm(claude_key=None, openai_text="// plain code")
    r = llm.generate_code(prompt="write", model=llm.anthropic.config.light_model)
    assert r.model == "gpt-5-mini"
    assert r.raw_text == "// plain code"
    kw = o_sdk.last_instance.chat.completions.last_kwargs
    # generate_code does NOT constrain response_format.
    assert "response_format" not in kw


# ---------------------------------------------------------------------------
# Configuration: env-var parsing + build_default_llm_client
# ---------------------------------------------------------------------------


def test_openai_max_tokens_loaded_from_env(monkeypatch):
    """``OPENAI_MAX_TOKENS`` / ``OPENAI_MAX_TOKENS_CEILING`` override the
    dataclass defaults via :py:meth:`OpenAIConfig.from_env`."""
    monkeypatch.setenv("OPENAI_API_KEY", "sk-test")
    monkeypatch.setenv("OPENAI_MAX_TOKENS", "70000")
    monkeypatch.setenv("OPENAI_MAX_TOKENS_CEILING", "120000")
    cfg = OpenAIConfig.from_env()
    assert cfg.max_tokens == 70_000
    assert cfg.max_tokens_ceiling == 120_000


def test_openai_max_tokens_falls_back_when_env_garbage(monkeypatch):
    monkeypatch.setenv("OPENAI_MAX_TOKENS", "not-a-number")
    cfg = OpenAIConfig.from_env()
    assert cfg.max_tokens == 96_000  # dataclass default


def test_anthropic_max_tokens_loaded_from_env(monkeypatch):
    monkeypatch.setenv("CLAUDE_MAX_TOKENS", "48000")
    cfg = AnthropicConfig.from_env()
    assert cfg.max_tokens == 48_000


def test_anthropic_default_config_has_generous_max_tokens():
    """Regression guard: the previous 8K default truncated MechanicPlans
    for complex prompts on Claude as well as OpenAI."""
    assert AnthropicConfig().max_tokens >= 16_000


def test_build_default_llm_client_propagates_max_tokens():
    """The orchestrator-side helper must forward the per-provider
    ``max_tokens`` knobs to the underlying configs, otherwise the
    ``Settings`` overrides silently do nothing."""
    from ptv_providers import build_default_llm_client

    client = build_default_llm_client(
        anthropic_api_key="sk-ant",
        openai_api_key="sk-test",
        claude_hard_model="claude-opus-4-7",
        claude_light_model="claude-sonnet-4-6",
        openai_hard_model="gpt-5",
        openai_light_model="gpt-5-mini",
        claude_max_tokens=50_000,
        openai_max_tokens=70_000,
        openai_max_tokens_ceiling=110_000,
    )
    assert client.anthropic.config.max_tokens == 50_000
    assert client.openai.config.max_tokens == 70_000
    assert client.openai.config.max_tokens_ceiling == 110_000


def test_build_default_llm_client_keeps_dataclass_defaults_when_unset():
    """Omitting the kwargs preserves the per-provider dataclass
    defaults so callers don't have to keep two sets of numbers in sync."""
    from ptv_providers import build_default_llm_client

    client = build_default_llm_client(
        anthropic_api_key="sk-ant",
        openai_api_key="sk-test",
        claude_hard_model="claude-opus-4-7",
        claude_light_model="claude-sonnet-4-6",
        openai_hard_model="gpt-5",
        openai_light_model="gpt-5-mini",
    )
    assert client.anthropic.config.max_tokens == AnthropicConfig().max_tokens
    assert client.openai.config.max_tokens == OpenAIConfig().max_tokens
    assert (
        client.openai.config.max_tokens_ceiling
        == OpenAIConfig().max_tokens_ceiling
    )
