"""OpenAI Agents SDK wrapper for the NpcPlan stage.

One "NPC director" agent reads the prompt plus the BuildSpec and AssetManifest
and decides what every character in the game is supposed to do. No tools: the
answer is a reading of the prompt, not research.

Mirrors ``mechanic_plan_agent.py`` exactly, including the lazy SDK import so
unit tests can inject a stub runner and never touch the network.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Callable, Optional

from ptv_schemas import NpcPlan


@dataclass
class NpcPlanAgentInput:
    """Everything the director needs for its single planning turn."""

    prompt_text: str
    """The user's raw game prompt. Included verbatim because the NPC
    decisions hinge on wording the BuildSpec summarises away -- "the ducks
    should just slide" is exactly the kind of instruction that does not
    survive being condensed into a mechanic summary."""

    build_spec_json: str
    asset_manifest_json: str
    extra_instructions: list[str] = field(default_factory=list)


RunnerFn = Callable[[str], NpcPlan]


class NpcPlanAgentError(RuntimeError):
    """Raised when the agent run fails or returns an unusable result."""


class NpcDirector:
    """Owns one Agents-SDK agent that produces an NpcPlan."""

    def __init__(
        self,
        *,
        instructions: str,
        model: str = "gpt-5.5",
        reasoning_effort: str = "high",
        max_turns: int = 30,
        session_id: str = "ptv-npc-plan",
        runner: Optional[RunnerFn] = None,
    ) -> None:
        self.instructions = instructions
        self.model = model
        self.reasoning_effort = reasoning_effort
        self.max_turns = max_turns
        self.session_id = session_id
        self._runner = runner

    def run(self, message: str) -> NpcPlan:
        if self._runner is None:
            self._runner = self._build_default_runner()
        result = self._runner(message)
        if not isinstance(result, NpcPlan):
            raise NpcPlanAgentError(
                f"runner returned {type(result).__name__}, expected NpcPlan"
            )
        return result

    # ------------------------------------------------------------------
    def _build_default_runner(self) -> RunnerFn:
        try:
            from agents import (  # type: ignore
                Agent,
                AgentOutputSchema,
                ModelSettings,
                Runner,
            )
            from openai.types.shared import Reasoning  # type: ignore
        except ImportError as exc:  # pragma: no cover - real runs only
            raise NpcPlanAgentError(
                "openai-agents is not installed. Add `openai-agents` to "
                "apps/orchestrator/pyproject.toml and `pip install -e "
                "apps/orchestrator`."
            ) from exc

        # max_length constraints on the nested lists are incompatible with
        # OpenAI strict JSON schema mode; Pydantic still validates the result.
        output_schema = AgentOutputSchema(NpcPlan, strict_json_schema=False)

        agent = Agent(
            name=f"ptv-npc-director-{self.session_id}",
            instructions=self.instructions,
            model=self.model,
            model_settings=ModelSettings(
                reasoning=Reasoning(effort=self.reasoning_effort),
                verbosity="medium",
            ),
            output_type=output_schema,
        )

        def _run(message: str) -> NpcPlan:
            try:
                run_result = Runner.run_sync(
                    agent, message, max_turns=self.max_turns
                )
            except Exception as exc:  # pragma: no cover - live SDK
                raise NpcPlanAgentError(f"Agents SDK run failed: {exc}") from exc
            try:
                return run_result.final_output_as(NpcPlan)
            except Exception as exc:  # pragma: no cover - live SDK
                raise NpcPlanAgentError(
                    f"Agents SDK returned non-conforming output: {exc}"
                ) from exc

        return _run


def build_director_message(inp: NpcPlanAgentInput) -> str:
    """The single user turn: raw prompt + BuildSpec + AssetManifest."""
    extras = (
        "\n".join(f"- {line}" for line in inp.extra_instructions)
        if inp.extra_instructions
        else "(none)"
    )
    return (
        "You are the NPC DIRECTOR. Decide what every non-player character in "
        "this game is supposed to do, and emit a complete NpcPlan.\n\n"
        "Process:\n"
        "  1) Find the characters. Go through the AssetManifest and pick out "
        "the items that represent an autonomous actor. Skip props, weapons, "
        "scenery, UI and FX. If there are none, return an empty plan.\n"
        "  2) For each one, decide its movement style FIRST -- that is the "
        "decision that spends credits. Re-read the prompt for an explicit "
        "instruction about animation before assuming it needs a rig.\n"
        "  3) Fill in role, faction and who it fights, then the behaviors the "
        "prompt implies, then the animation intents those behaviors actually "
        "use.\n"
        "  4) Set plausible numbers for speed, health, damage, perception and "
        "spawn counts, and keep the total concurrent NPCs within budget.\n\n"
        "EXTRA INSTRUCTIONS:\n"
        f"{extras}\n\n"
        "================ Game prompt (verbatim) ================\n"
        f"{inp.prompt_text}\n\n"
        "================ BuildSpec (JSON) ================\n"
        f"{inp.build_spec_json}\n\n"
        "================ AssetManifest (JSON) ================\n"
        f"{inp.asset_manifest_json}\n"
    )
