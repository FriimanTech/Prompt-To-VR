"""OpenAI Agents SDK wrapper for the world-generation stage.

A SINGLE "world architect" agent authors all the C++ for the VR environment,
iterating across turns. It runs with a persistent ``Session`` so the
compile-and-repair loop can send follow-up messages (compiler errors) and the
agent remembers everything it already wrote.

Verified against the current SDK docs (OpenAI Agents SDK / openai-agents 0.18,
July 2026):

* ``from agents import Agent, Runner, WebSearchTool, ModelSettings,
  SQLiteSession, OpenAIResponsesModel``
* ``from openai import AsyncOpenAI`` and ``from openai.types.shared import Reasoning``
* GPT-5.6 Sol (``gpt-5.6-sol``; the ``gpt-5.6`` alias also routes to Sol) is
  the frontier model. It supports reasoning efforts
  ``none|low|medium|high|xhigh|max`` and OpenAI recommends the **Responses API**
  for reasoning + tools + multi-turn, so we drive the agent with an explicit
  ``OpenAIResponsesModel`` rather than the default chat path.
* The model runs behind a **custom ``AsyncOpenAI`` client with a long request
  timeout**. The SDK's default client uses the openai library's 600s (10 min)
  read timeout; a single ``xhigh`` turn that also calls ``WebSearchTool`` and
  returns a large multi-file C++ payload can exceed that and fail with
  "Request timed out", dropping the stage to the fallback world. We raise the
  per-request timeout (``request_timeout_s``) to give the architect headroom.
* ``Agent(model=OpenAIResponsesModel(model=..., openai_client=client),
  model_settings=ModelSettings(reasoning=Reasoning(effort="xhigh"),
  verbosity="medium"), tools=[WebSearchTool()], output_type=...)``
* ``Runner.run_sync(agent, message, session=session, max_turns=...)`` -> RunResult
* ``RunResult.final_output_as(Cls)``

The SDK import is lazy so unit tests can inject a stub ``runner`` and never
touch the network or require ``openai-agents`` to be installed.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Callable, List, Optional

from pydantic import BaseModel, Field


# ---------------------------------------------------------------------------
# Structured agent output
# ---------------------------------------------------------------------------


class WorldGeneratedFile(BaseModel):
    """One C++ file the agent wants written under the world output dir."""

    path: str = Field(
        ...,
        max_length=256,
        description=(
            "Relative path under engine/generated/world/<project_id>/ -- e.g. "
            "world.cpp, city_layout.cpp, world_shared.h. Bare filename is fine."
        ),
    )
    contents: str = Field(
        ...,
        max_length=200_000,
        description="Full C++ file contents. No markdown fences, no prose.",
    )


class WorldAgentResult(BaseModel):
    """Structured result the world architect returns."""

    files: List[WorldGeneratedFile] = Field(default_factory=list, max_length=24)
    builder_id: str = Field(
        default="",
        description="The id passed to PTV_REGISTER_WORLD_BUILDER in the .cpp.",
    )
    plan: str = Field(
        default="",
        max_length=8_000,
        description="Short description of the sections/areas the agent built.",
    )
    notes: str = Field(default="", max_length=8_000)


# ---------------------------------------------------------------------------
# Input bundle
# ---------------------------------------------------------------------------


@dataclass
class WorldAgentInput:
    """Everything the architect needs for the initial build turn."""

    project_id: str
    output_subdir: str
    """``engine/generated/world/<project_id>`` -- every file path must start here."""

    builder_id: str
    """``generated.world.<project_id>`` -- the id to register."""

    engine_guide: str
    """Contents of ``engine/world_author_guide.md`` (understand-the-engine input)."""

    build_spec_excerpt: str
    """Title / pitch / setting / constraints text from the BuildSpec."""

    object_assets_json: str
    """JSON list of available prop/object asset ids the agent MAY place_object()."""

    extra_instructions: list[str] = field(default_factory=list)


# ---------------------------------------------------------------------------
# Pluggable runner (tests inject a stub)
# ---------------------------------------------------------------------------

RunnerFn = Callable[[str], WorldAgentResult]
"""(message) -> result. Runs one turn within the persistent session."""


class WorldAgentError(RuntimeError):
    """Raised when the agent run fails or returns an unusable result."""


# ---------------------------------------------------------------------------
# World architect: persistent-session agent driver
# ---------------------------------------------------------------------------


class WorldArchitect:
    """Owns one Agents-SDK agent + session for a project's world build.

    Call :meth:`run` with the initial build message, then again with repair
    messages (compiler errors); the session preserves the agent's memory of
    everything it wrote so it can fix issues in context.
    """

    def __init__(
        self,
        *,
        instructions: str,
        model: str = "gpt-5.6-sol",
        reasoning_effort: str = "xhigh",
        max_turns: int = 80,
        session_id: str = "ptv-world",
        request_timeout_s: float = 1800.0,
        runner: Optional[RunnerFn] = None,
    ) -> None:
        self.instructions = instructions
        self.model = model
        self.reasoning_effort = reasoning_effort
        self.max_turns = max_turns
        self.session_id = session_id
        self.request_timeout_s = request_timeout_s
        self._runner = runner  # injected (tests) or lazily built

    def run(self, message: str) -> WorldAgentResult:
        if self._runner is None:
            self._runner = self._build_default_runner()
        result = self._runner(message)
        if not isinstance(result, WorldAgentResult):
            raise WorldAgentError(
                f"runner returned {type(result).__name__}, expected WorldAgentResult"
            )
        return result

    # ------------------------------------------------------------------
    def _build_default_runner(self) -> RunnerFn:
        """Construct the real Agents SDK agent + in-memory session once."""
        try:
            import httpx  # type: ignore
            from agents import (  # type: ignore
                Agent,
                AgentOutputSchema,
                ModelSettings,
                OpenAIResponsesModel,
                Runner,
                SQLiteSession,
                WebSearchTool,
            )
            from openai import AsyncOpenAI  # type: ignore
            from openai.types.shared import Reasoning  # type: ignore
        except ImportError as exc:  # pragma: no cover - real runs only
            raise WorldAgentError(
                "openai-agents is not installed. Add `openai-agents` to "
                "apps/orchestrator/pyproject.toml and `pip install -e "
                "apps/orchestrator`."
            ) from exc

        # ``WorldAgentResult`` carries max_length constraints not compatible
        # with OpenAI strict JSON schema mode; disable strict so the model
        # emits freely and the SDK still validates via Pydantic.
        output_schema = AgentOutputSchema(WorldAgentResult, strict_json_schema=False)

        model_settings = ModelSettings(
            reasoning=Reasoning(effort=self.reasoning_effort),
            verbosity="medium",
        )

        # Drive the agent through the Responses API (OpenAI's recommendation
        # for GPT-5.6 reasoning + tools + multi-turn) with a dedicated client
        # whose HTTP read/write timeout is large enough for one long xhigh
        # turn. The SDK's default client keeps the openai library's 600s read
        # timeout, which a single web-searching, large-C++ turn can exceed --
        # the "Request timed out" that was dropping this stage to the fallback
        # world. ``connect`` stays short so genuine network failures fail fast;
        # only one retry so a real timeout doesn't silently re-run for hours.
        http_timeout = httpx.Timeout(
            self.request_timeout_s,
            connect=15.0,
        )
        client = AsyncOpenAI(timeout=http_timeout, max_retries=1)
        model = OpenAIResponsesModel(model=self.model, openai_client=client)

        agent = Agent(
            name=f"ptv-world-architect-{self.session_id}",
            instructions=self.instructions,
            model=model,
            model_settings=model_settings,
            tools=[WebSearchTool()],
            output_type=output_schema,
        )

        # In-memory session (no db_path) -- ephemeral per stage run so a fresh
        # build never inherits stale memory.
        session = SQLiteSession(self.session_id)

        def _run(message: str) -> WorldAgentResult:
            try:
                run_result = Runner.run_sync(
                    agent, message, session=session, max_turns=self.max_turns
                )
            except Exception as exc:  # pragma: no cover - live SDK
                raise WorldAgentError(f"Agents SDK run failed: {exc}") from exc
            try:
                return run_result.final_output_as(WorldAgentResult)
            except Exception as exc:  # pragma: no cover - live SDK
                raise WorldAgentError(
                    f"Agents SDK returned non-conforming output: {exc}"
                ) from exc

        return _run


# ---------------------------------------------------------------------------
# Message construction (also used by tests to verify wiring)
# ---------------------------------------------------------------------------


def build_initial_message(inp: WorldAgentInput) -> str:
    """The first user turn: the full brief + engine guide."""
    extras = (
        "\n".join(f"- {line}" for line in inp.extra_instructions)
        if inp.extra_instructions
        else "(none)"
    )
    return (
        "You are the VR WORLD ARCHITECT. Author ALL the C++ for a full, "
        "creative, walkable VR environment for this game, iterating over each "
        "part (sky, ground, structure, landmarks, detail, spawn).\n\n"
        f"project_id: {inp.project_id}\n"
        f"output_subdir: {inp.output_subdir}  (every files[].path MUST start here)\n"
        f"builder_id to register: {inp.builder_id}\n\n"
        "Process:\n"
        "  1) READ the engine guide below and ground everything in it. You may "
        "ONLY #include \"ptv/api/IWorldBuilder.h\", the C++ standard library, "
        "and <glm/...>. Never include or modify any engine core.\n"
        "  2) OPTIONALLY use web_search (no cap) to research the setting and "
        "native C++ / procedural techniques. Reject Unity/Unreal/Godot/WebXR.\n"
        "  3) Implement one IWorldBuilder subclass that builds the whole "
        "environment via WorldContext, registered EXACTLY once with "
        "PTV_REGISTER_WORLD_BUILDER in a .cpp. Split across multiple files if "
        "helpful. Guarantee sky + a light + ground-with-collision + a "
        "player_spawn standing on the ground.\n\n"
        "Return the WorldAgentResult schema: files[] (full C++), builder_id, "
        "plan, notes.\n\n"
        "EXTRA INSTRUCTIONS:\n"
        f"{extras}\n\n"
        "================ ENGINE / WORLD AUTHOR GUIDE ================\n"
        f"{inp.engine_guide}\n\n"
        "================ GAME BRIEF (BuildSpec excerpt) ================\n"
        f"{inp.build_spec_excerpt}\n\n"
        "================ AVAILABLE OBJECT ASSET IDS (optional place_object) ==\n"
        f"{inp.object_assets_json}\n"
    )


def build_repair_message(compile_log: str, changed_hint: str = "") -> str:
    """A follow-up turn: feed compiler errors back to fix in-context."""
    return (
        "Your generated world C++ FAILED to compile in the offline ptv_worldgen "
        "host. Fix ALL errors and return the COMPLETE corrected files[] again "
        "(same schema). Keep the same builder_id. Do not remove working parts.\n"
        + (f"\nHINT: {changed_hint}\n" if changed_hint else "")
        + "\n================ COMPILER OUTPUT ================\n"
        + compile_log
    )


def build_validation_repair_message(reason: str) -> str:
    """A follow-up turn: the world compiled but failed validation."""
    return (
        "Your world compiled but produced an INVALID scene: "
        f"{reason}. Fix the build() logic and return the COMPLETE corrected "
        "files[] again (same schema, same builder_id). Ensure there is a "
        "ground with collision, a player_spawn standing on it, at least one "
        "light, and a reasonable number of entities."
    )
