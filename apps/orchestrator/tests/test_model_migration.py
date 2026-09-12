"""Every OpenAI call in the pipeline runs gpt-5.6-sol at a chosen effort.

The model rename was the easy part of this migration. The trap was that
:class:`agents.Agent` back-fills ``model_settings`` from the model NAME when
a caller passes none::

    if self.model is not None and self.model_settings == get_default_model_settings():
        self.model_settings = _initial_model_settings_for_model(self.model)

and the SDK's table maps ``gpt-5`` to effort ``low`` but ``gpt-5.6-sol`` to
``none``. The codegen stage passed no model settings, so renaming its model
would have silently stripped reasoning from the hardest C++ generation in
the pipeline -- no error, no log line, just worse code.

So these tests do not check that a string was changed. They construct the
REAL ``Agent`` each stage builds and assert the effort that actually reaches
the SDK. Building an Agent makes no network call.
"""

from __future__ import annotations

from pathlib import Path
from typing import Any, get_args

import pytest

from ptv_orchestrator.settings import (
    REASONING_EFFORTS,
    VERBOSITY_LEVELS,
    Settings,
)

agents = pytest.importorskip("agents")

EXPECTED_MODEL = "gpt-5.6-sol"


def _settings(tmp_path: Path, **overrides: Any) -> Settings:
    """Settings with the repo's DEFAULTS, ignoring the developer's .env.

    Reading `.env` here would make the test assert whatever the machine
    happens to be configured with rather than what ships.
    """
    return Settings(
        _env_file=None,
        projects_dir=tmp_path / "projects",
        engine_dir=tmp_path / "engine",
        samples_dir=tmp_path / "samples",
        repo_root=tmp_path,
        **overrides,
    )


# Every setting that names an OpenAI model, and the effort that goes with it.
MODEL_FIELDS = (
    ("openai_hard_model", "openai_hard_reasoning_effort"),
    ("openai_light_model", "openai_light_reasoning_effort"),
    ("codegen_model", "codegen_reasoning_effort"),
    ("mechanic_plan_model", "mechanic_plan_reasoning_effort"),
    ("npc_plan_model", "npc_plan_reasoning_effort"),
    ("world_model", "world_reasoning_effort"),
    ("scene_spec_model", "scene_spec_reasoning_effort"),
)


# ---------------------------------------------------------------------------
# The configured values
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("model_field,effort_field", MODEL_FIELDS)
def test_every_openai_path_is_on_sol(
    tmp_path: Path, model_field: str, effort_field: str
) -> None:
    s = _settings(tmp_path)
    assert getattr(s, model_field) == EXPECTED_MODEL, model_field
    assert getattr(s, effort_field) in REASONING_EFFORTS, effort_field


def test_configured_models_are_known_to_the_installed_openai_sdk(
    tmp_path: Path,
) -> None:
    """Catches a typo'd or hallucinated model id without spending a call."""
    from openai.types.shared.chat_model import ChatModel

    known = set(get_args(ChatModel))
    s = _settings(tmp_path)
    for model_field, _ in MODEL_FIELDS:
        assert getattr(s, model_field) in known, model_field


def test_minimal_effort_is_rejected(tmp_path: Path) -> None:
    """``minimal`` was valid on gpt-5 and dropped in 5.6, so a leftover
    value in .env must fail at startup rather than as a 400 mid-run."""
    assert "minimal" not in REASONING_EFFORTS
    with pytest.raises(Exception, match="unknown reasoning effort"):
        _settings(tmp_path, codegen_reasoning_effort="minimal")


def test_verbosity_is_validated(tmp_path: Path) -> None:
    assert set(VERBOSITY_LEVELS) == {"low", "medium", "high"}
    with pytest.raises(Exception, match="unknown verbosity"):
        _settings(tmp_path, world_verbosity="verbose")


def test_the_two_fallback_tiers_differ_only_by_effort(tmp_path: Path) -> None:
    """Both tiers are the same model now, so effort is what makes the
    'light' path light. If these ever match, the distinction is gone."""
    s = _settings(tmp_path)
    assert s.openai_hard_model == s.openai_light_model
    assert s.openai_hard_reasoning_effort != s.openai_light_reasoning_effort


def test_light_tier_is_not_starved(tmp_path: Path) -> None:
    """The light tier serves asset_manifest, which decides which assets get
    rigged and animated -- roughly 30 and 20 Tripo credits each. Reasoning
    below ``medium`` on the call that spends the credits is a false economy."""
    s = _settings(tmp_path)
    assert s.openai_light_reasoning_effort in ("medium", "high", "xhigh", "max")


# ---------------------------------------------------------------------------
# What the SDK actually receives
# ---------------------------------------------------------------------------


class _AgentCapture:
    """Records the kwargs each stage passes to ``Agent`` and builds the real
    thing, so the assertions see the SDK's post-init behaviour."""

    def __init__(self) -> None:
        self.kwargs: dict[str, Any] = {}
        self.agent: Any = None

    def install(self, monkeypatch: pytest.MonkeyPatch) -> None:
        real_agent = agents.Agent

        def factory(**kwargs: Any) -> Any:
            self.kwargs = kwargs
            self.agent = real_agent(**kwargs)
            return self.agent

        # The stages do ``from agents import Agent`` INSIDE their builder
        # functions, so patching the module attribute is enough.
        monkeypatch.setattr(agents, "Agent", factory)

    @property
    def effort(self) -> Any:
        reasoning = self.agent.model_settings.reasoning
        return None if reasoning is None else reasoning.effort

    @property
    def verbosity(self) -> Any:
        return self.agent.model_settings.verbosity


def test_sdk_would_silently_strip_reasoning_without_explicit_settings() -> None:
    """The hazard this whole test module exists for.

    If this ever starts failing, the SDK changed its back-fill table and the
    explicit settings below may no longer be load-bearing -- but do not
    remove them on that basis.
    """
    inferred = agents.Agent(name="probe", model=EXPECTED_MODEL)
    assert inferred.model_settings.reasoning is not None
    assert inferred.model_settings.reasoning.effort == "none", (
        "SDK no longer infers 'none' for gpt-5.6-sol; re-check the codegen "
        "stage's explicit model settings"
    )


def test_npc_plan_agent_gets_the_configured_effort(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    from ptv_orchestrator.pipeline.npc_plan_agent import NpcDirector

    s = _settings(tmp_path)
    cap = _AgentCapture()
    cap.install(monkeypatch)

    NpcDirector(
        instructions="(test)",
        model=s.npc_plan_model,
        reasoning_effort=s.npc_plan_reasoning_effort,
        verbosity=s.npc_plan_verbosity,
    )._build_default_runner()

    assert cap.kwargs["model"] == EXPECTED_MODEL
    assert cap.effort == s.npc_plan_reasoning_effort
    assert cap.verbosity == s.npc_plan_verbosity


def test_mechanic_plan_agent_gets_the_configured_effort(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    from ptv_orchestrator.pipeline.mechanic_plan_agent import MechanicPlanner

    s = _settings(tmp_path)
    cap = _AgentCapture()
    cap.install(monkeypatch)

    MechanicPlanner(
        instructions="(test)",
        model=s.mechanic_plan_model,
        reasoning_effort=s.mechanic_plan_reasoning_effort,
        verbosity=s.mechanic_plan_verbosity,
    )._build_default_runner()

    assert cap.kwargs["model"] == EXPECTED_MODEL
    # Raised from medium: this stage authors the CodeTasks all of codegen
    # works from, so a misclassification costs a whole compile cycle.
    assert cap.effort == "high"
    assert cap.verbosity == s.mechanic_plan_verbosity


def test_scene_spec_agent_gets_the_configured_effort(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    from ptv_orchestrator.pipeline.stage_scene_spec import (
        _default_composer_factory,
    )
    from ptv_schemas import TargetDevice

    s = _settings(tmp_path)
    cap = _AgentCapture()
    cap.install(monkeypatch)

    _default_composer_factory(
        s, "sid", TargetDevice.pcvr
    )._build_default_runner()

    assert cap.kwargs["model"] == EXPECTED_MODEL
    assert cap.effort == s.scene_spec_reasoning_effort
    assert cap.verbosity == s.scene_spec_verbosity
    # The only stage with an explicit output cap, so truncation is a
    # deterministic repair branch rather than a mystery parse failure.
    assert cap.agent.model_settings.max_tokens == s.scene_spec_max_output_tokens


def test_world_agent_gets_the_configured_effort(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    from ptv_orchestrator.pipeline.world_agent import WorldArchitect

    s = _settings(tmp_path)
    cap = _AgentCapture()
    cap.install(monkeypatch)

    WorldArchitect(
        instructions="(test)",
        model=s.world_model,
        reasoning_effort=s.world_reasoning_effort,
        verbosity=s.world_verbosity,
    )._build_default_runner()

    assert cap.kwargs["model"] == EXPECTED_MODEL
    assert cap.effort == s.world_reasoning_effort
    assert cap.verbosity == s.world_verbosity
    # Carries tools, so it MUST stay on the Responses API: 5.6 allows
    # function tools on Chat Completions only at effort 'none'.
    assert cap.kwargs["tools"], "world architect lost its WebSearchTool"


def test_world_agent_class_default_matches_settings() -> None:
    """The class default used to say ``xhigh`` while the stage passed
    ``high``, so the documented tier was one nobody ran."""
    from ptv_orchestrator.pipeline.world_agent import WorldArchitect
    import inspect

    params = inspect.signature(WorldArchitect.__init__).parameters
    assert params["model"].default == EXPECTED_MODEL
    assert params["reasoning_effort"].default == "high"


def test_codegen_agent_gets_explicit_high_effort(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """The regression guard. Without explicit settings the SDK would hand
    this stage effort 'none' (see the probe test above)."""
    from ptv_orchestrator.pipeline import codegen_agent

    monkeypatch.setenv("OPENAI_API_KEY", "sk-test")

    s = _settings(tmp_path)
    cap = _AgentCapture()
    cap.install(monkeypatch)

    class _Stop(Exception):
        pass

    class _NoRun:
        @staticmethod
        def run_sync(*_a: Any, **_k: Any) -> Any:
            raise _Stop()

    monkeypatch.setattr(agents, "Runner", _NoRun)

    inp = codegen_agent.CodegenAgentInput(
        project_id="p",
        task_id="t",
        output_subdir="engine/generated/mechanics/p",
        task_json="{}",
        ledger_snapshot_json="{}",
        dep_exports_json="{}",
        scene_context_json="[]",
        build_spec_excerpt="(none)",
        instructions="(test)",
    )

    with pytest.raises(codegen_agent.CodegenAgentError):
        codegen_agent.run_codegen_agent(
            inp,
            model=s.codegen_model,
            reasoning_effort=s.codegen_reasoning_effort,
            verbosity=s.codegen_verbosity,
        )

    # Codegen hands the Agent an explicit model object rather than a bare
    # id, so it can give the client a timeout longer than the SDK's 600s
    # default. The model id still has to be the one settings chose.
    model_arg = cap.kwargs["model"]
    assert getattr(model_arg, "model", model_arg) == EXPECTED_MODEL
    assert cap.effort == "high", (
        "codegen lost its explicit reasoning effort; the SDK will infer "
        "'none' for gpt-5.6-sol"
    )
    assert cap.verbosity == s.codegen_verbosity
    assert cap.kwargs["tools"], "codegen lost its WebSearchTool"


def test_codegen_client_gets_a_timeout_longer_than_the_sdk_default(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """The SDK's default client caps a request at 600s.

    A large CodeTask at effort ``high`` runs past that, and the two
    built-in retries turn one slow task into ~25 wasted minutes, a failed
    task, every dependent blocked, and a failed stage -- after every other
    task has already been paid for. So codegen supplies its own client.
    """
    import agents

    from ptv_orchestrator.pipeline import codegen_agent

    # The stage bridges settings.openai_api_key into os.environ before any
    # agent runs; the SDK's client reads it from there.
    monkeypatch.setenv("OPENAI_API_KEY", "sk-test")

    cap = _AgentCapture()
    cap.install(monkeypatch)

    class _Stop(Exception):
        pass

    class _NoRun:
        @staticmethod
        def run_sync(*_a: Any, **_k: Any) -> Any:
            raise _Stop()

    monkeypatch.setattr(agents, "Runner", _NoRun)

    inp = codegen_agent.CodegenAgentInput(
        project_id="p",
        task_id="t",
        output_subdir="engine/generated/mechanics/p",
        task_json="{}",
        ledger_snapshot_json="{}",
        dep_exports_json="{}",
        scene_context_json="[]",
        build_spec_excerpt="(none)",
        instructions="(test)",
    )

    with pytest.raises(codegen_agent.CodegenAgentError):
        codegen_agent.run_codegen_agent(
            inp, model="gpt-5.6-sol", request_timeout_seconds=1234.0
        )

    client = cap.kwargs["model"]._client
    assert client.timeout == 1234.0, (
        "codegen fell back to the SDK's shared client, so its 600s cap is "
        "back and a long task dies again"
    )


def test_codegen_reads_its_own_settings_not_the_fallback_tier(
    tmp_path: Path,
) -> None:
    """``openai_hard_model`` means 'what we use when Claude is down'.
    Codegen always runs on OpenAI, so it owns its own knobs -- otherwise
    tuning the fallback would silently retune C++ generation."""
    s = _settings(tmp_path, openai_hard_model="gpt-5.6-luna")
    assert s.codegen_model == EXPECTED_MODEL
