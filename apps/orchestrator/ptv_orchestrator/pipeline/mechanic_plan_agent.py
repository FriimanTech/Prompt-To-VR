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
* ``Agent(model="gpt-5.5", model_settings=ModelSettings(reasoning=Reasoning(effort=...)),
  output_type=AgentOutputSchema(MechanicPlan, strict_json_schema=False))``
* ``Runner.run_sync(agent, message) -> RunResult`` ; ``RunResult.final_output_as(MechanicPlan)``

The SDK import is lazy so unit tests can inject a stub ``runner`` and never
touch the network or require ``openai-agents`` to be installed.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Callable, Optional

from ptv_schemas import MechanicPlan


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

    editable_builtins_note: str = ""
    """Short note listing which catalog built-ins have real, editable source
    available for the ``builtin+config_code_edit`` path."""

    extra_instructions: list[str] = field(default_factory=list)


# ---------------------------------------------------------------------------
# Pluggable runner (tests inject a stub)
# ---------------------------------------------------------------------------

RunnerFn = Callable[[str], MechanicPlan]
"""(message) -> MechanicPlan."""


class MechanicPlanAgentError(RuntimeError):
    """Raised when the agent run fails or returns an unusable result."""


# ---------------------------------------------------------------------------
# Mechanic planner: single-turn structured-output agent driver
# ---------------------------------------------------------------------------


class MechanicPlanner:
    """Owns one Agents-SDK agent that produces a MechanicPlan."""

    def __init__(
        self,
        *,
        instructions: str,
        model: str = "gpt-5.5",
        reasoning_effort: str = "high",
        max_turns: int = 40,
        session_id: str = "ptv-mechanic-plan",
        runner: Optional[RunnerFn] = None,
    ) -> None:
        self.instructions = instructions
        self.model = model
        self.reasoning_effort = reasoning_effort
        self.max_turns = max_turns
        self.session_id = session_id
        self._runner = runner  # injected (tests) or lazily built

    def run(self, message: str) -> MechanicPlan:
        if self._runner is None:
            self._runner = self._build_default_runner()
        result = self._runner(message)
        if not isinstance(result, MechanicPlan):
            raise MechanicPlanAgentError(
                f"runner returned {type(result).__name__}, expected MechanicPlan"
            )
        return result

    # ------------------------------------------------------------------
    def _build_default_runner(self) -> RunnerFn:
        """Construct the real Agents SDK agent once."""
        try:
            from agents import (  # type: ignore
                Agent,
                AgentOutputSchema,
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

        # ``MechanicPlan`` (with nested CodeTask) carries max_length
        # constraints that are incompatible with OpenAI strict JSON schema
        # mode; disable strict so the model emits freely and the SDK still
        # validates via Pydantic.
        output_schema = AgentOutputSchema(MechanicPlan, strict_json_schema=False)

        model_settings = ModelSettings(
            reasoning=Reasoning(effort=self.reasoning_effort),
            verbosity="medium",
        )

        agent = Agent(
            name=f"ptv-mechanic-planner-{self.session_id}",
            instructions=self.instructions,
            model=self.model,
            model_settings=model_settings,
            output_type=output_schema,
        )

        def _run(message: str) -> MechanicPlan:
            try:
                run_result = Runner.run_sync(
                    agent, message, max_turns=self.max_turns
                )
            except Exception as exc:  # pragma: no cover - live SDK
                raise MechanicPlanAgentError(
                    f"Agents SDK run failed: {exc}"
                ) from exc
            try:
                return run_result.final_output_as(MechanicPlan)
            except Exception as exc:  # pragma: no cover - live SDK
                raise MechanicPlanAgentError(
                    f"Agents SDK returned non-conforming output: {exc}"
                ) from exc

        return _run


# ---------------------------------------------------------------------------
# Message construction (also used by tests to verify wiring)
# ---------------------------------------------------------------------------


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
        f"{inp.catalog_text}\n"
    )
