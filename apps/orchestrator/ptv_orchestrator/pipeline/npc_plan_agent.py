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

from .agent_output_capture import (
    MAX_LOGGED_OUTPUT_CHARS,
    diagnose_output_failure,
    make_capturing_schema,
)


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

LogFn = Callable[[str], None]
"""Sink for per-attempt diagnostics (the stage writes logs/npc_plan.log)."""


class NpcPlanAgentError(RuntimeError):
    """Raised when the agent run fails or returns an unusable result."""


class NpcPlanOutputError(NpcPlanAgentError):
    """The model produced output that did not validate."""

    def __init__(
        self, message: str, *, issues: list[str], truncated: bool = False
    ) -> None:
        super().__init__(message)
        self.issues = issues
        self.truncated = truncated


class NpcDirector:
    """Owns one Agents-SDK agent that produces an NpcPlan."""

    def __init__(
        self,
        *,
        instructions: str,
        model: str = "gpt-5.6-sol",
        reasoning_effort: str = "high",
        verbosity: str = "medium",
        max_turns: int = 30,
        session_id: str = "ptv-npc-plan",
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
        self._runner = runner

    def _log(self, text: str) -> None:
        if self.log is not None:
            self.log(text)

    def run(self, message: str) -> NpcPlan:
        if self._runner is None:
            self._runner = self._build_default_runner()

        attempts = max(0, int(self.max_retries)) + 1
        last: NpcPlanAgentError | None = None
        for attempt in range(attempts):
            msg = message
            if attempt > 0 and isinstance(last, NpcPlanOutputError):
                bullets = "\n".join(f"- {i}" for i in last.issues) or "- (unspecified)"
                msg = (
                    "Your previous answer did not match the schema. Fix ALL "
                    "of these and return the COMPLETE NpcPlan:\n"
                    f"{bullets}\n\n{message}"
                )
                self._log(f"retry {attempt}/{attempts - 1} after: {last}")
            elif attempt > 0:
                self._log(f"retry {attempt}/{attempts - 1} (re-sending brief)")
            try:
                result = self._runner(msg)
            except NpcPlanAgentError as exc:
                last = exc
                continue
            if not isinstance(result, NpcPlan):
                raise NpcPlanAgentError(
                    f"runner returned {type(result).__name__}, expected NpcPlan"
                )
            return result

        assert last is not None  # the loop only exits here after a failure
        raise last

    # ------------------------------------------------------------------
    def _build_default_runner(self) -> RunnerFn:
        try:
            import agents as agents_mod  # type: ignore
            from agents import (  # type: ignore
                Agent,
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

        # ``NpcPlan`` is not strict-schema compatible, so Pydantic is what
        # validates the result; the capturing wrapper keeps the raw output
        # around so a failure can name the offending field.
        output_schema = make_capturing_schema(agents_mod, NpcPlan, strict=False)

        agent = Agent(
            name=f"ptv-npc-director-{self.session_id}",
            instructions=self.instructions,
            model=self.model,
            model_settings=ModelSettings(
                reasoning=Reasoning(effort=self.reasoning_effort),
                verbosity=self.verbosity,
            ),
            output_type=output_schema,
        )

        def _run(message: str) -> NpcPlan:
            output_schema.reset()
            self._log(
                f"turn: model={self.model} effort={self.reasoning_effort} "
                f"message={len(message)} chars"
            )
            try:
                run_result = Runner.run_sync(
                    agent, message, max_turns=self.max_turns
                )
                plan = run_result.final_output_as(NpcPlan)
            except Exception as exc:  # pragma: no cover - live SDK
                raw = output_schema.last_raw
                if raw is not None:
                    self._log(
                        f"FAILED; raw model output ({len(raw)} chars) follows:\n"
                        f"{raw[:MAX_LOGGED_OUTPUT_CHARS]}"
                    )
                diagnosis = diagnose_output_failure(raw, exc, NpcPlan)
                self._log(f"FAILED: {diagnosis.message}")
                for issue in diagnosis.issues:
                    self._log(f"  issue: {issue}")
                if diagnosis.repairable:
                    raise NpcPlanOutputError(
                        diagnosis.message,
                        issues=diagnosis.issues,
                        truncated=diagnosis.truncated,
                    ) from exc
                raise NpcPlanAgentError(diagnosis.message) from exc

            self._log(f"ok: {len(plan.npcs)} npc(s)")
            return plan

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
