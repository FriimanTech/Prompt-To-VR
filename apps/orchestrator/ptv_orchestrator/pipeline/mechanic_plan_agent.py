"""OpenAI Agents SDK wrapper for the MechanicPlan stage.

A single "mechanic planner" agent classifies every BuildSpec mechanic against
the engine catalog and emits a full :class:`MechanicPlan` (including nested
CodeTasks for generated / code-edited mechanics and controller bindings).
No tools are attached — the agent only produces a MechanicPlan from the
BuildSpec, AssetManifest, and engine catalog in the prompt.

Mirrors the proven world-generation agent wiring (see ``world_agent.py``),
minus tools:

* ``from agents import Agent, AgentOutputSchema, ModelSettings, Runner``
* ``from openai.types.shared import Reasoning``
* ``Agent(model="gpt-5.6-sol", model_settings=ModelSettings(reasoning=Reasoning(effort=...)),
  output_type=AgentOutputSchema(MechanicPlan, strict_json_schema=False))``
* ``Runner.run_sync(agent, message) -> RunResult`` ; ``RunResult.final_output_as(MechanicPlan)``

The SDK import is lazy so unit tests can inject a stub ``runner`` and never
touch the network or require ``openai-agents`` to be installed.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Callable, Optional

from ptv_schemas import MechanicPlan

from .agent_output_capture import (
    MAX_LOGGED_OUTPUT_CHARS,
    diagnose_output_failure,
    make_capturing_schema,
)


# ---------------------------------------------------------------------------
# Input bundle
# ---------------------------------------------------------------------------


@dataclass
class MechanicPlanAgentInput:
    """Everything the planner agent needs for its single planning turn."""

    build_spec_json: str
    """``BuildSpec.model_dump_json(indent=2)``."""

    asset_manifest_json: str
    """``AssetManifest.model_dump_json(indent=2)``."""

    catalog_text: str
    """Engine catalog (built-in + promoted) rendered by ``as_planner_text``."""

    npc_plan_json: str = ""
    """``NpcPlan.model_dump_json(indent=2)``, or empty for a game with no
    characters. The planner needs it so the CodeTasks it writes cover the
    behaviours the NPC director committed to, rather than a second, different
    reading of the same prompt."""

    editable_builtins_note: str = ""
    """Short note listing which catalog built-ins have real, editable source
    available for the ``builtin+config_code_edit`` path."""

    extra_instructions: list[str] = field(default_factory=list)


# ---------------------------------------------------------------------------
# Pluggable runner (tests inject a stub)
# ---------------------------------------------------------------------------

RunnerFn = Callable[[str], MechanicPlan]
"""(message) -> MechanicPlan."""

LogFn = Callable[[str], None]
"""Sink for per-attempt diagnostics (the stage writes logs/mechanic_plan.log)."""


class MechanicPlanAgentError(RuntimeError):
    """Raised when the agent run fails or returns an unusable result."""


class MechanicPlanOutputError(MechanicPlanAgentError):
    """The model produced output that did not validate.

    Carries field-level ``issues`` so a retry can name what was wrong.
    Before this existed the stage had no retry at all: one hallucinated key
    anywhere in a large plan failed the whole stage.
    """

    def __init__(
        self, message: str, *, issues: list[str], truncated: bool = False
    ) -> None:
        super().__init__(message)
        self.issues = issues
        self.truncated = truncated


# ---------------------------------------------------------------------------
# Mechanic planner: single-turn structured-output agent driver
# ---------------------------------------------------------------------------


class MechanicPlanner:
    """Owns one Agents-SDK agent that produces a MechanicPlan."""

    def __init__(
        self,
        *,
        instructions: str,
        model: str = "gpt-5.6-sol",
        reasoning_effort: str = "high",
        verbosity: str = "medium",
        max_turns: int = 40,
        session_id: str = "ptv-mechanic-plan",
        runner: Optional[RunnerFn] = None,
        log: Optional[LogFn] = None,
        max_retries: int = 1,
    ) -> None:
        self.instructions = instructions
        self.model = model
        self.reasoning_effort = reasoning_effort
        self.verbosity = verbosity
        self.max_turns = max_turns
        self.session_id = session_id
        self.log = log
        self.max_retries = max_retries
        self._runner = runner  # injected (tests) or lazily built

    def _log(self, text: str) -> None:
        if self.log is not None:
            self.log(text)

    def run(self, message: str) -> MechanicPlan:
        if self._runner is None:
            self._runner = self._build_default_runner()

        # One shot plus ``max_retries``. A retry is only worth making when
        # the model actually produced something we can describe back to it;
        # a transport failure is re-sent unchanged.
        attempts = max(0, int(self.max_retries)) + 1
        last: MechanicPlanAgentError | None = None
        for attempt in range(attempts):
            msg = message
            if attempt > 0 and isinstance(last, MechanicPlanOutputError):
                msg = build_retry_message(message, last)
                self._log(f"retry {attempt}/{attempts - 1} after: {last}")
            elif attempt > 0:
                self._log(f"retry {attempt}/{attempts - 1} (re-sending brief)")
            try:
                result = self._runner(msg)
            except MechanicPlanAgentError as exc:
                last = exc
                continue
            if not isinstance(result, MechanicPlan):
                raise MechanicPlanAgentError(
                    f"runner returned {type(result).__name__}, expected MechanicPlan"
                )
            return result

        assert last is not None  # the loop only exits here after a failure
        raise last

    # ------------------------------------------------------------------
    def _build_default_runner(self) -> RunnerFn:
        """Construct the real Agents SDK agent once."""
        try:
            import agents as agents_mod  # type: ignore
            from agents import (  # type: ignore
                Agent,
                ModelSettings,
                Runner,
            )
            from openai.types.shared import Reasoning  # type: ignore
        except ImportError as exc:  # pragma: no cover - real runs only
            raise MechanicPlanAgentError(
                "openai-agents is not installed. Add `openai-agents` to "
                "apps/orchestrator/pyproject.toml and `pip install -e "
                "apps/orchestrator`."
            ) from exc

        # ``MechanicPlan`` carries open-ended ``config`` dicts, which strict
        # JSON-schema mode forbids, so this one stays non-strict and relies
        # on Pydantic validation. The capturing wrapper is what makes such a
        # failure describable instead of a redacted one-liner.
        output_schema = make_capturing_schema(
            agents_mod, MechanicPlan, strict=False
        )

        model_settings = ModelSettings(
            reasoning=Reasoning(effort=self.reasoning_effort),
            verbosity=self.verbosity,
        )

        agent = Agent(
            name=f"ptv-mechanic-planner-{self.session_id}",
            instructions=self.instructions,
            model=self.model,
            model_settings=model_settings,
            output_type=output_schema,
        )

        def _run(message: str) -> MechanicPlan:
            output_schema.reset()
            self._log(
                f"turn: model={self.model} effort={self.reasoning_effort} "
                f"message={len(message)} chars"
            )
            try:
                run_result = Runner.run_sync(
                    agent, message, max_turns=self.max_turns
                )
                plan = run_result.final_output_as(MechanicPlan)
            except Exception as exc:  # pragma: no cover - live SDK
                raw = output_schema.last_raw
                if raw is not None:
                    self._log(
                        f"FAILED; raw model output ({len(raw)} chars) follows:\n"
                        f"{raw[:MAX_LOGGED_OUTPUT_CHARS]}"
                    )
                diagnosis = diagnose_output_failure(raw, exc, MechanicPlan)
                self._log(f"FAILED: {diagnosis.message}")
                for issue in diagnosis.issues:
                    self._log(f"  issue: {issue}")
                if diagnosis.repairable:
                    raise MechanicPlanOutputError(
                        diagnosis.message,
                        issues=diagnosis.issues,
                        truncated=diagnosis.truncated,
                    ) from exc
                raise MechanicPlanAgentError(diagnosis.message) from exc

            self._log(f"ok: {len(plan.items)} plan item(s)")
            return plan

        return _run


# ---------------------------------------------------------------------------
# Message construction (also used by tests to verify wiring)
# ---------------------------------------------------------------------------


def build_retry_message(
    original: str, failure: MechanicPlanOutputError
) -> str:
    """Re-send the brief with what went wrong the first time.

    This agent has no session, so the retry has to restate the whole brief;
    what it adds is the part the SDK used to redact.
    """
    if failure.truncated:
        problem = (
            "Your previous answer was CUT OFF before the JSON was complete: "
            "it exceeded the output token limit. Keep every mechanic but "
            "write shorter CodeTask descriptions."
        )
    else:
        bullets = "\n".join(f"- {i}" for i in failure.issues) or "- (unspecified)"
        problem = (
            "Your previous answer did not match the schema. Fix ALL of these "
            "and return the COMPLETE MechanicPlan:\n"
            f"{bullets}"
        )
    return f"{problem}\n\n{original}"


def build_planner_message(inp: MechanicPlanAgentInput) -> str:
    """The single user turn: BuildSpec + AssetManifest + engine catalog."""
    extras = (
        "\n".join(f"- {line}" for line in inp.extra_instructions)
        if inp.extra_instructions
        else "(none)"
    )
    editable = inp.editable_builtins_note or "(any catalog built-in id is a valid base_builtin_id)"
    return (
        "You are the MECHANIC PLANNER. Classify EVERY mechanic in the "
        "BuildSpec and emit a matching MechanicPlanItem (1:1 by id). Produce "
        "a complete MechanicPlan.\n\n"
        "Process:\n"
        "  1) For each BuildSpec mechanic, check the ENGINE CATALOG below. "
        "Decide whether a built-in truly satisfies what the prompt asks: as-is "
        "(builtin), with parameter tuning only (builtin+config), by copying + "
        "editing the built-in's source into a NEW mechanic "
        "(builtin+config_code_edit), or from scratch (generate). Escalate only "
        "as far as needed: prefer reuse over params over code_edit over "
        "generate.\n"
        "  2) Assign a controller binding to every input-driven mechanic "
        "(from the prompt when it names one, otherwise a VR-best-practice "
        "default). Do not research online — decide from the prompt and "
        "these instructions only.\n"
        "  3) Set depends_on on code_tasks so hubs are built before the "
        "profiles that branch off them.\n\n"
        f"Built-ins with editable source (for builtin+config_code_edit):\n{editable}\n\n"
        "EXTRA INSTRUCTIONS:\n"
        f"{extras}\n\n"
        "================ BuildSpec (JSON) ================\n"
        f"{inp.build_spec_json}\n\n"
        "================ AssetManifest (JSON) ================\n"
        f"{inp.asset_manifest_json}\n\n"
        + (
            "================ NpcPlan (JSON) ================\n"
            "Already decided by the NPC director. Every behavior listed here "
            "needs C++ behind it, and every character listed here is already "
            "bound to builtin.character_locomotion -- do NOT plan a mechanic "
            "that moves characters or plays their walk/run clips.\n"
            f"{inp.npc_plan_json}\n\n"
            if inp.npc_plan_json
            else ""
        )
        + f"{inp.catalog_text}\n"
    )
