"""OpenAI Agents SDK wrapper for the SceneSpec stage.

A "scene composer" agent turns the compressed per-scene brief (BuildSpec
meta + this scene + assets + mechanics) into a full :class:`SceneSpec`.
No tools are attached -- the agent only produces a SceneSpec from the
context in the message.

It runs with a persistent ``Session`` so the stage's validate-and-repair
loop can send follow-up messages (schema / semantic problems) and the
agent remembers the SceneSpec it already produced, fixing issues in
context rather than re-composing from scratch.

Two things here are deliberate and were learned the hard way.

**Strict structured output.** The agent's ``output_type`` is
:class:`WireSceneSpec` (see ``scene_spec_wire``) with
``strict_json_schema=True``, so OpenAI constrains decoding and invalid
JSON / pattern-violating ids cannot be produced at all. Running non-strict
against ``SceneSpec`` directly meant the schema was mere guidance, and one
hallucinated key or one uppercase id anywhere in a hundred entities threw
away a three-minute generation.

**We capture the raw output ourselves** via ``agent_output_capture``, so a
failure can be logged in full and turned into field-level repair
instructions instead of the SDK's redacted "Invalid JSON when parsing model
output". See that module for why that mattered.

The SDK import is lazy so unit tests can inject a stub ``runner`` and never
touch the network or require ``openai-agents`` to be installed.
"""

from __future__ import annotations

from typing import Callable, Optional

from ptv_schemas import SceneSpec

from .agent_output_capture import (
    MAX_LOGGED_OUTPUT_CHARS,
    diagnose_output_failure,
    make_capturing_schema,
)
from .scene_spec_wire import WireSceneSpec, to_scene_spec


# ---------------------------------------------------------------------------
# Pluggable runner (tests inject a stub)
# ---------------------------------------------------------------------------

RunnerFn = Callable[[str], SceneSpec]
"""(message) -> SceneSpec. Runs one turn within the persistent session."""

LogFn = Callable[[str], None]
"""Sink for per-attempt diagnostics (the stage writes logs/scene_spec.log)."""


class SceneSpecAgentError(RuntimeError):
    """Raised when the agent run fails or returns an unusable result."""


class SceneSpecOutputError(SceneSpecAgentError):
    """The model's output did not validate.

    Carries field-level ``issues`` so the repair turn can name what was
    wrong, and ``truncated`` when the output was cut off at the token cap
    (a different problem needing a different instruction: emit less).
    """

    def __init__(
        self, message: str, *, issues: list[str], truncated: bool = False
    ) -> None:
        super().__init__(message)
        self.issues = issues
        self.truncated = truncated


def classify_output_failure(
    raw: Optional[str], exc: BaseException
) -> SceneSpecAgentError:
    """Turn an SDK exception into something the repair loop can act on."""
    diagnosis = diagnose_output_failure(raw, exc, WireSceneSpec)
    if not diagnosis.repairable:
        return SceneSpecAgentError(diagnosis.message)
    return SceneSpecOutputError(
        diagnosis.message,
        issues=diagnosis.issues,
        truncated=diagnosis.truncated,
    )


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
        model: str = "gpt-5.6-sol",
        reasoning_effort: str = "medium",
        verbosity: str = "medium",
        max_output_tokens: Optional[int] = None,
        max_turns: int = 30,
        session_id: str = "ptv-scene-spec",
        runner: Optional[RunnerFn] = None,
        log: Optional[LogFn] = None,
    ) -> None:
        self.instructions = instructions
        self.model = model
        self.reasoning_effort = reasoning_effort
        self.verbosity = verbosity
        self.max_output_tokens = max_output_tokens
        self.max_turns = max_turns
        self.session_id = session_id
        self.log = log
        self._runner = runner  # injected (tests) or lazily built

    def _log(self, text: str) -> None:
        if self.log is not None:
            self.log(text)

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
            import agents as agents_mod  # type: ignore
            from agents import (  # type: ignore
                Agent,
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

        output_schema = make_capturing_schema(
            agents_mod, WireSceneSpec, strict=True
        )

        model_settings = ModelSettings(
            reasoning=Reasoning(effort=self.reasoning_effort),
            verbosity=self.verbosity,
            max_tokens=self.max_output_tokens,
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
            output_schema.reset()
            self._log(
                f"turn: model={self.model} effort={self.reasoning_effort} "
                f"verbosity={self.verbosity} max_tokens={self.max_output_tokens} "
                f"message={len(message)} chars"
            )
            try:
                run_result = Runner.run_sync(
                    agent, message, session=session, max_turns=self.max_turns
                )
                wire = run_result.final_output_as(WireSceneSpec)
            except Exception as exc:  # pragma: no cover - live SDK
                raw = output_schema.last_raw
                if raw is not None:
                    self._log(
                        f"FAILED; raw model output ({len(raw)} chars) follows:\n"
                        f"{raw[:MAX_LOGGED_OUTPUT_CHARS]}"
                    )
                error = classify_output_failure(raw, exc)
                self._log(f"FAILED: {error}")
                for issue in getattr(error, "issues", []):
                    self._log(f"  issue: {issue}")
                raise error from exc

            scene = to_scene_spec(wire)
            self._log(
                f"ok: {len(scene.entities)} entities, {len(scene.lights)} lights"
            )
            return scene

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


def build_truncation_message(max_entities: int) -> str:
    """A follow-up turn for output that was cut off at the token cap.

    Repeating "fix these problems" is useless here -- there is nothing
    wrong with the content, there is just too much of it.
    """
    return (
        "Your previous output was CUT OFF before the JSON was complete: it "
        "exceeded the output token limit. Nothing was wrong with the "
        "content, there was simply too much of it.\n\n"
        "Compose the scene again, smaller:\n"
        f"- emit at most {max_entities} entities\n"
        "- keep every entity the game needs (the player rig, each asset "
        "once, gameplay triggers) and drop pure decoration -- the "
        "environment and its dressing are authored separately in C++\n"
        "- leave 'param_overrides' empty unless this entity genuinely "
        "needs a value different from the MechanicPlan config\n"
        "- do not add explanatory text; return only the JSON object\n"
    )
