"""OpenAI Agents SDK wrapper for the InteractionDesign stage.

One "interaction designer" agent reads the prompt, the BuildSpec and the
AssetManifest and decides how every object is held, which named points the
vision pass must find on it, and -- for weapons -- how it fires and what it
fires. ``WebSearchTool`` is attached so real-world dimensions, calibres and
rates of fire are looked up rather than guessed. Mirrors
``game_design_agent.py``, including the lazy SDK import so tests inject a stub
runner and never touch the network.

Verified against the installed SDK (openai-agents 0.20):
``Agent(model=..., model_settings=ModelSettings(reasoning=Reasoning(effort=...),
verbosity=...), tools=[WebSearchTool()], output_type=...)`` and
``Runner.run_sync(agent, message, max_turns=...)``.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Callable, Optional

from ptv_schemas import InteractionDesign

from .agent_output_capture import (
    MAX_LOGGED_OUTPUT_CHARS,
    diagnose_output_failure,
    make_capturing_schema,
)


@dataclass
class InteractionDesignAgentInput:
    prompt_text: str
    """Verbatim: explicit choices ("a Glock that fires a laser beam") live in
    the user's wording, and every explicit decision must quote it."""
    build_spec_json: str
    asset_manifest_json: str
    extra_instructions: list[str] = field(default_factory=list)


RunnerFn = Callable[[str], InteractionDesign]
LogFn = Callable[[str], None]


class InteractionDesignAgentError(RuntimeError):
    """The agent run failed or returned an unusable result."""


class InteractionDesignOutputError(InteractionDesignAgentError):
    def __init__(self, message: str, *, issues: list[str]) -> None:
        super().__init__(message)
        self.issues = issues


class InteractionDesigner:
    """Owns one Agents-SDK agent that produces an InteractionDesign."""

    def __init__(self, *, instructions: str, model: str = "gpt-5.6-sol",
                 reasoning_effort: str = "high", verbosity: str = "medium",
                 max_turns: int = 40, session_id: str = "ptv-interaction-design",
                 web_search: bool = True, runner: Optional[RunnerFn] = None,
                 log: Optional[LogFn] = None, max_retries: int = 1) -> None:
        self.instructions = instructions
        self.model = model
        self.reasoning_effort = reasoning_effort
        self.verbosity = verbosity
        self.max_turns = max_turns
        self.session_id = session_id
        self.web_search = web_search
        self.log = log
        self.max_retries = max_retries
        self._runner = runner

    def _log(self, text: str) -> None:
        if self.log is not None:
            self.log(text)

    def run(self, message: str) -> InteractionDesign:
        if self._runner is None:
            self._runner = self._build_default_runner()
        attempts = max(0, int(self.max_retries)) + 1
        last: InteractionDesignAgentError | None = None
        for attempt in range(attempts):
            msg = message
            if attempt > 0 and isinstance(last, InteractionDesignOutputError):
                bullets = "\n".join(f"- {i}" for i in last.issues) or "- (unspecified)"
                msg = ("Your previous answer did not match the schema. Fix ALL of "
                       "these and return the COMPLETE InteractionDesign:\n"
                       f"{bullets}\n\n{message}")
            try:
                result = self._runner(msg)
            except InteractionDesignAgentError as exc:
                last = exc
                self._log(f"attempt {attempt + 1} failed: {exc}")
                continue
            if not isinstance(result, InteractionDesign):
                raise InteractionDesignAgentError(
                    f"runner returned {type(result).__name__}, expected InteractionDesign")
            return result
        assert last is not None
        raise last

    def _build_default_runner(self) -> RunnerFn:
        try:
            import agents as agents_mod  # type: ignore
            from agents import Agent, ModelSettings, Runner, WebSearchTool  # type: ignore
            from openai.types.shared import Reasoning  # type: ignore
        except ImportError as exc:  # pragma: no cover - real runs only
            raise InteractionDesignAgentError("openai-agents is not installed") from exc

        output_schema = make_capturing_schema(agents_mod, InteractionDesign, strict=False)
        agent = Agent(
            name=f"ptv-interaction-designer-{self.session_id}",
            instructions=self.instructions,
            model=self.model,
            # Effort is explicit: Agent.__post_init__ otherwise infers it from
            # the model name, and infers ``none`` for gpt-5.6-sol.
            model_settings=ModelSettings(
                reasoning=Reasoning(effort=self.reasoning_effort),
                verbosity=self.verbosity,
            ),
            tools=[WebSearchTool()] if self.web_search else [],
            output_type=output_schema,
        )

        def _run(message: str) -> InteractionDesign:
            output_schema.reset()
            self._log(f"turn: model={self.model} effort={self.reasoning_effort} "
                      f"message={len(message)} chars")
            try:
                run_result = Runner.run_sync(agent, message, max_turns=self.max_turns)
                design = run_result.final_output_as(InteractionDesign)
            except Exception as exc:  # pragma: no cover - live SDK
                raw = output_schema.last_raw
                if raw is not None:
                    self._log(f"FAILED; raw output follows:\n{raw[:MAX_LOGGED_OUTPUT_CHARS]}")
                diagnosis = diagnose_output_failure(raw, exc, InteractionDesign)
                self._log(f"FAILED: {diagnosis.message}")
                for issue in diagnosis.issues:
                    self._log(f"  issue: {issue}")
                if diagnosis.repairable:
                    raise InteractionDesignOutputError(diagnosis.message,
                                                       issues=diagnosis.issues) from exc
                raise InteractionDesignAgentError(diagnosis.message) from exc
            self._log(f"ok: {len(design.items)} item(s), "
                      f"{len(design.weapons())} weapon(s), "
                      f"{len(design.projectiles)} projectile(s)")
            return design

        return _run


def build_designer_message(inp: InteractionDesignAgentInput) -> str:
    extras = ("\n".join(f"- {line}" for line in inp.extra_instructions)
              if inp.extra_instructions else "(none)")
    return (
        "You are the INTERACTION DESIGNER. Decide how every non-character asset "
        "is used, held and (for weapons) fired, and emit an InteractionDesign.\n\n"
        "Process:\n"
        "  1) Read the prompt for EXPLICIT instructions about each item (how it "
        "fires, what it fires, how it is held). Each one is a decision with "
        "source=explicit and the prompt's exact words in prompt_quote.\n"
        "  2) For each asset decide usage, hold and sockets; research real-world "
        "dimensions / calibres where the item depicts a real object.\n"
        "  3) For ranged weapons decide the fire system and projectile; give a "
        "projectile a mesh only when it can actually be seen.\n"
        "  4) List what you deliberately left out in `omitted`.\n\n"
        f"EXTRA INSTRUCTIONS:\n{extras}\n\n"
        "================ Game prompt (verbatim) ================\n"
        f"{inp.prompt_text}\n\n"
        "================ BuildSpec (JSON) ================\n"
        f"{inp.build_spec_json}\n\n"
        "================ AssetManifest (JSON) ================\n"
        f"{inp.asset_manifest_json}\n"
    )
