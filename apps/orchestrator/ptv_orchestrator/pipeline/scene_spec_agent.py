"""OpenAI Agents SDK wrapper for the SceneSpec stage.

A "scene composer" agent turns the compressed per-scene brief (BuildSpec
meta + this scene + assets + mechanics) into a full :class:`SceneSpec`.
No tools are attached -- the agent only produces a SceneSpec from the
context in the message.

It runs with a persistent ``Session`` so the stage's validate-and-repair
loop can send follow-up messages (schema / semantic problems) and the
agent remembers the SceneSpec it already produced, fixing issues in
context rather than re-composing from scratch.

Mirrors ``mechanic_plan_agent.py`` (no tools) + the persistent-session
repair pattern of ``world_agent.py``:

* ``from agents import Agent, AgentOutputSchema, ModelSettings, Runner, SQLiteSession``
* ``from openai.types.shared import Reasoning``
* ``Agent(model="gpt-5.5", model_settings=ModelSettings(reasoning=Reasoning(effort=...)),
  output_type=AgentOutputSchema(SceneSpec, strict_json_schema=False))``
* ``Runner.run_sync(agent, message, session=session, max_turns=...) -> RunResult``
* ``RunResult.final_output_as(SceneSpec)``

The SDK import is lazy so unit tests can inject a stub ``runner`` and never
touch the network or require ``openai-agents`` to be installed.
"""

from __future__ import annotations

from typing import Callable, Optional

from ptv_schemas import SceneSpec


# ---------------------------------------------------------------------------
# Pluggable runner (tests inject a stub)
# ---------------------------------------------------------------------------

RunnerFn = Callable[[str], SceneSpec]
"""(message) -> SceneSpec. Runs one turn within the persistent session."""


class SceneSpecAgentError(RuntimeError):
    """Raised when the agent run fails or returns an unusable result."""


# ---------------------------------------------------------------------------
# Scene composer: persistent-session structured-output agent driver
# ---------------------------------------------------------------------------


class SceneSpecComposer:
    """Owns one Agents-SDK agent + session for composing ONE scene.

    Call :meth:`compose` with the initial per-scene message, then again with
    repair messages (schema / semantic problems); the session preserves the
    agent's memory of the SceneSpec it already produced so it can fix issues
    in context.
    """

    def __init__(
        self,
        *,
        instructions: str,
        model: str = "gpt-5.5",
        reasoning_effort: str = "medium",
        max_turns: int = 30,
        session_id: str = "ptv-scene-spec",
        runner: Optional[RunnerFn] = None,
    ) -> None:
        self.instructions = instructions
        self.model = model
        self.reasoning_effort = reasoning_effort
        self.max_turns = max_turns
        self.session_id = session_id
        self._runner = runner  # injected (tests) or lazily built

    def compose(self, message: str) -> SceneSpec:
        if self._runner is None:
            self._runner = self._build_default_runner()
        result = self._runner(message)
        if not isinstance(result, SceneSpec):
            raise SceneSpecAgentError(
                f"runner returned {type(result).__name__}, expected SceneSpec"
            )
        return result

    # ------------------------------------------------------------------
    def _build_default_runner(self) -> RunnerFn:
        """Construct the real Agents SDK agent + in-memory session once."""
        try:
            from agents import (  # type: ignore
                Agent,
                AgentOutputSchema,
                ModelSettings,
                Runner,
                SQLiteSession,
            )
            from openai.types.shared import Reasoning  # type: ignore
        except ImportError as exc:  # pragma: no cover - real runs only
            raise SceneSpecAgentError(
                "openai-agents is not installed. Add `openai-agents` to "
                "apps/orchestrator/pyproject.toml and `pip install -e "
                "apps/orchestrator`."
            ) from exc

        # ``SceneSpec`` carries max_length constraints that are incompatible
        # with OpenAI strict JSON schema mode; disable strict so the model
        # emits freely and the SDK still validates via Pydantic.
        output_schema = AgentOutputSchema(SceneSpec, strict_json_schema=False)

        model_settings = ModelSettings(
            reasoning=Reasoning(effort=self.reasoning_effort),
            verbosity="medium",
        )

        agent = Agent(
            name=f"ptv-scene-composer-{self.session_id}",
            instructions=self.instructions,
            model=self.model,
            model_settings=model_settings,
            output_type=output_schema,
        )

        # In-memory session (no db_path) -- ephemeral per scene so a fresh
        # compose never inherits stale memory, but repair turns within this
        # scene share context.
        session = SQLiteSession(self.session_id)

        def _run(message: str) -> SceneSpec:
            try:
                run_result = Runner.run_sync(
                    agent, message, session=session, max_turns=self.max_turns
                )
            except Exception as exc:  # pragma: no cover - live SDK
                raise SceneSpecAgentError(
                    f"Agents SDK run failed: {exc}"
                ) from exc
            try:
                return run_result.final_output_as(SceneSpec)
            except Exception as exc:  # pragma: no cover - live SDK
                raise SceneSpecAgentError(
                    f"Agents SDK returned non-conforming output: {exc}"
                ) from exc

        return _run


# ---------------------------------------------------------------------------
# Repair-message construction (also used by tests to verify wiring)
# ---------------------------------------------------------------------------


def build_repair_message(issues: list[str]) -> str:
    """A follow-up turn: feed schema / semantic problems back to fix
    in-context. The agent must return the COMPLETE corrected SceneSpec."""
    bullets = "\n".join(f"- {i}" for i in issues) if issues else "- (unspecified)"
    return (
        "Your SceneSpec had the following problems. Fix ALL of them and "
        "return the COMPLETE corrected SceneSpec (same scene_id, full "
        "entities list, obeying the JSON Schema exactly). Do not drop "
        "entities that were already correct.\n\n"
        "PROBLEMS:\n"
        f"{bullets}\n"
    )
