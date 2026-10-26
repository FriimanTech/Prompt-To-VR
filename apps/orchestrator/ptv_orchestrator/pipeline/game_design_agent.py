"""OpenAI Agents SDK wrapper for the GameDesign stage.

One "game designer" agent reads the prompt plus the BuildSpec, NpcPlan and
AssetManifest and decides the rules of the game loop. No tools: the answer is
a reading of the prompt. Mirrors ``npc_plan_agent.py``, including the lazy SDK
import so tests inject a stub runner and never touch the network.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Callable, Optional

from ptv_schemas import GameDesign

from .agent_output_capture import (
    MAX_LOGGED_OUTPUT_CHARS,
    diagnose_output_failure,
    make_capturing_schema,
)


@dataclass
class GameDesignAgentInput:
    prompt_text: str
    """Verbatim: explicit rules ("3 lives", "survive 3 minutes") live in the
    user's wording, and every explicit decision must quote it."""
    build_spec_json: str
    npc_plan_json: str
    asset_manifest_json: str
    extra_instructions: list[str] = field(default_factory=list)


RunnerFn = Callable[[str], GameDesign]
LogFn = Callable[[str], None]


class GameDesignAgentError(RuntimeError):
    """The agent run failed or returned an unusable result."""


class GameDesignOutputError(GameDesignAgentError):
    def __init__(self, message: str, *, issues: list[str]) -> None:
        super().__init__(message)
        self.issues = issues


class GameDesigner:
    """Owns one Agents-SDK agent that produces a GameDesign."""

    def __init__(self, *, instructions: str, model: str = "gpt-5.6-sol",
                 reasoning_effort: str = "high", verbosity: str = "medium",
                 max_turns: int = 20, session_id: str = "ptv-game-design",
                 runner: Optional[RunnerFn] = None, log: Optional[LogFn] = None,
                 max_retries: int = 1) -> None:
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

    def run(self, message: str) -> GameDesign:
        if self._runner is None:
            self._runner = self._build_default_runner()
        attempts = max(0, int(self.max_retries)) + 1
        last: GameDesignAgentError | None = None
        for attempt in range(attempts):
            msg = message
            if attempt > 0 and isinstance(last, GameDesignOutputError):
                bullets = "\n".join(f"- {i}" for i in last.issues) or "- (unspecified)"
                msg = ("Your previous answer did not match the schema. Fix ALL of "
                       f"these and return the COMPLETE GameDesign:\n{bullets}\n\n{message}")
            try:
                result = self._runner(msg)
            except GameDesignAgentError as exc:
                last = exc
                self._log(f"attempt {attempt + 1} failed: {exc}")
                continue
            if not isinstance(result, GameDesign):
                raise GameDesignAgentError(
                    f"runner returned {type(result).__name__}, expected GameDesign")
            return result
        assert last is not None
        raise last

    def _build_default_runner(self) -> RunnerFn:
        try:
            import agents as agents_mod  # type: ignore
            from agents import Agent, ModelSettings, Runner  # type: ignore
            from openai.types.shared import Reasoning  # type: ignore
        except ImportError as exc:  # pragma: no cover - real runs only
            raise GameDesignAgentError("openai-agents is not installed") from exc

        output_schema = make_capturing_schema(agents_mod, GameDesign, strict=False)
        agent = Agent(
            name=f"ptv-game-designer-{self.session_id}",
            instructions=self.instructions,
            model=self.model,
            # Effort is explicit: Agent.__post_init__ otherwise infers it from
            # the model name, and infers ``none`` for gpt-5.6-sol.
            model_settings=ModelSettings(
                reasoning=Reasoning(effort=self.reasoning_effort),
                verbosity=self.verbosity,
            ),
            output_type=output_schema,
        )

        def _run(message: str) -> GameDesign:
            output_schema.reset()
            self._log(f"turn: model={self.model} effort={self.reasoning_effort} "
                      f"message={len(message)} chars")
            try:
                run_result = Runner.run_sync(agent, message, max_turns=self.max_turns)
                design = run_result.final_output_as(GameDesign)
            except Exception as exc:  # pragma: no cover - live SDK
                raw = output_schema.last_raw
                if raw is not None:
                    self._log(f"FAILED; raw output follows:\n{raw[:MAX_LOGGED_OUTPUT_CHARS]}")
                diagnosis = diagnose_output_failure(raw, exc, GameDesign)
                self._log(f"FAILED: {diagnosis.message}")
                for issue in diagnosis.issues:
                    self._log(f"  issue: {issue}")
                if diagnosis.repairable:
                    raise GameDesignOutputError(diagnosis.message,
                                                issues=diagnosis.issues) from exc
                raise GameDesignAgentError(diagnosis.message) from exc
            self._log(f"ok: {design.experience.value}, {len(design.objectives)} objective(s)")
            return design

        return _run


def build_designer_message(inp: GameDesignAgentInput) -> str:
    extras = ("\n".join(f"- {line}" for line in inp.extra_instructions)
              if inp.extra_instructions else "(none)")
    return (
        "You are the GAME DESIGNER. Decide the complete rules of this game's loop "
        "and emit a GameDesign.\n\n"
        "Process:\n"
        "  1) Read the prompt for EXPLICIT rules (lives, timers, win/lose, what "
        "happens on death, menus). Each one is a decision with source=explicit and "
        "the prompt's exact words in prompt_quote.\n"
        "  2) Decide what kind of experience this is, then infer ONLY the systems "
        "it needs (source=inferred with a one-line rationale). List the systems "
        "you deliberately left out in `omitted`.\n"
        "  3) Make the flow complete: every way the game ends has an end screen, "
        "every screen has buttons, and something offers Leave Game.\n\n"
        f"EXTRA INSTRUCTIONS:\n{extras}\n\n"
        "================ Game prompt (verbatim) ================\n"
        f"{inp.prompt_text}\n\n"
        "================ BuildSpec (JSON) ================\n"
        f"{inp.build_spec_json}\n\n"
        "================ NpcPlan (JSON) ================\n"
        f"{inp.npc_plan_json}\n\n"
        "================ AssetManifest (JSON) ================\n"
        f"{inp.asset_manifest_json}\n"
    )
