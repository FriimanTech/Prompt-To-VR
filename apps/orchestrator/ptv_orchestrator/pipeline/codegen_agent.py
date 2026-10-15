"""OpenAI Agents SDK wrapper for the code-generation stage.

One ``run_codegen_agent`` call drives **one** SDK ``Runner.run_sync``
session for **one** claimed ``CodeTask``. The agent has the hosted
``WebSearchTool`` enabled with **no cap** on the number of search calls;
the cap is implicit in the agent's own loop budget. The structured
output is a :class:`CodegenAgentResult` that the worker writes to disk
via ``CodeTask.files[]``.

This module isolates the ``openai-agents`` Python SDK behind a single
function so the rest of the orchestrator can import it without paying
the dependency cost in tests, and so unit tests can inject a stub
runner. Tests pass ``runner=<callable>`` and never touch the network.

Verified against:

* https://openai.github.io/openai-agents-python/ (Agents, Runner, output_type)
* https://openai.github.io/openai-agents-python/tools/ (WebSearchTool)

API surface used:

* ``from agents import Agent, Runner, WebSearchTool``
* ``Agent(name=..., instructions=..., model=..., tools=[WebSearchTool()], output_type=<Pydantic>)``
* ``Runner.run_sync(agent, user_message) -> RunResult``
* ``RunResult.final_output_as(Cls) -> Cls``
"""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from typing import Any, Callable, List, Optional

from pydantic import BaseModel, Field


# ---------------------------------------------------------------------------
# Structured agent output
# ---------------------------------------------------------------------------


class GeneratedFile(BaseModel):
    """One file the agent wants the worker to write to disk."""

    path: str = Field(
        ...,
        max_length=256,
        description=(
            "Relative path under engine/generated/{mechanics,ai,dialogue}/"
            "<project_id>/ — e.g. engine/generated/mechanics/proj_abc/"
            "shooting_hub_register.cpp"
        ),
    )
    contents: str = Field(
        ...,
        max_length=120_000,
        description="Full C++ file contents. No markdown fences, no prose.",
    )


class CodegenAgentResult(BaseModel):
    """Structured result a codegen agent returns for one CodeTask.

    The orchestrator uses ``files`` to write the multi-file mechanic to
    disk (with the per-file bounds pipeline) and ``exports`` to populate
    the ``codegen_board.json`` row so downstream dependent CodeTasks can
    read the public surface this task produced.
    """

    files: List[GeneratedFile] = Field(default_factory=list, max_length=16)
    exports: dict = Field(
        default_factory=dict,
        description=(
            "Public surface this CodeTask produced. Free-form per-mechanic"
            " keys: registered_mechanic_id, weapon_profile_id,"
            " registration_macro, public_symbols, etc. Downstream tasks"
            " read these from the ledger and plug into them directly."
        ),
    )
    notes: str = Field(
        default="",
        max_length=4_000,
        description=(
            "Optional free-form notes the agent wants to leave in the"
            " ledger / sidecar for human inspection (e.g. what it"
            " researched, what it stubbed)."
        ),
    )


# ---------------------------------------------------------------------------
# Input bundle the worker hands to the agent
# ---------------------------------------------------------------------------


@dataclass
class CodegenAgentInput:
    """Everything the agent needs to run the four-step process for one
    claimed CodeTask. Built by ``stage_code_generation`` per worker
    invocation and never shared across workers."""

    project_id: str
    task_id: str

    output_subdir: str
    """The resolved ``engine/generated/<kind>/<project_id>`` directory the
    worker will write to (``<kind>`` is ``mechanics`` / ``ai`` /
    ``dialogue``). Used in the user message so the agent emits paths under
    this exact prefix without guessing the kind."""

    task_json: str
    """``CodeTask.model_dump_json(indent=2)`` for the claimed task."""

    ledger_snapshot_json: str
    """Full ``codegen_board.json`` snapshot at the moment of claim."""

    dep_exports_json: str
    """``{dep_id: row.exports}`` for every ``depends_on`` task. Always
    populated because the board guarantees deps are ``done`` before
    claim."""

    scene_context_json: str
    """List of ``scene_spec*.json`` excerpts: entity ids, tags,
    mechanic_bindings, MeshRef paths that touch this mechanic. The
    worker filters out unrelated entities to keep token usage sane."""

    build_spec_excerpt: str
    """``BuildSpec`` mechanic + acceptance excerpt as plain text."""

    instructions: str
    """The ``CODE_TASK_SYSTEM`` prompt (after the four-step / multi-file
    rewrite). Becomes the agent's ``instructions``."""

    npc_plan_json: str = ""
    """The NpcPlan, when the game has characters. Every task gets it, not
    just AI ones: a wave spawner needs the spawn caps, a HUD needs the health
    values, and a weapon needs to know which faction it damages."""

    animation_inventory_json: str = ""
    """Per character: which provider animated it and the EXACT animation keys
    it has (``idle``, ``walk``, ``run``, ``run.punching``, ``box_01``...), plus
    the actions it lacks. The only reliable list of what ``play_clip`` and
    ``CLocomotionIntent::gait_variant`` can ask for on that asset."""

    world_context_json: str = ""
    """The declared vocabulary of every generated environment: interactive
    ids and their state keys, spawn points, portals, nav regions.

    These names are invented by the world architects, so this is the only
    place gameplay code can learn them. A guessed id is not a compile error --
    it resolves to nothing at runtime and the mechanic quietly does nothing --
    which is why the world stage runs before codegen."""

    base_source_block: str = ""
    """For ``builtin+config_code_edit`` tasks: the source of the base
    built-in mechanic to COPY-AND-EDIT into this new generated mechanic.
    Empty for ``generate`` tasks."""

    repair_context: str = ""
    """Set only on a compile-repair re-run: the C++ compiler errors from
    the previous attempt plus the current file contents. When present, the
    agent must FIX the existing files rather than author from scratch.
    Empty on the first attempt."""

    extra_instructions: list[str] = field(default_factory=list)


# ---------------------------------------------------------------------------
# Pluggable runner (for tests)
# ---------------------------------------------------------------------------


RunnerFn = Callable[[CodegenAgentInput, str], CodegenAgentResult]
"""(input, model_id) -> result. Tests inject a stub; production uses
the real SDK runner."""


class CodegenAgentError(RuntimeError):
    """Raised when the agent run fails or returns an unusable result."""


# ---------------------------------------------------------------------------
# Main entry point
# ---------------------------------------------------------------------------


def run_codegen_agent(
    inp: CodegenAgentInput,
    *,
    model: str = "gpt-5.6-sol",
    reasoning_effort: str = "high",
    verbosity: str = "medium",
    request_timeout_seconds: float = 1800.0,
    runner: Optional[RunnerFn] = None,
) -> CodegenAgentResult:
    """Run one OpenAI Agents SDK session for one claimed CodeTask.

    The hosted ``WebSearchTool`` is always enabled. No numeric cap is
    imposed on the number of web-search calls; the cap is implicit in
    the agent's own loop budget.

    Parameters
    ----------
    inp:
        The bundle the worker built for this CodeTask.
    model:
        Model id. Must be compatible with the OpenAI Agents SDK.
        Default matches ``Settings.codegen_model``.
    reasoning_effort:
        ``reasoning.effort`` for the run. This MUST be passed explicitly
        (see :func:`_default_sdk_runner`) -- leaving the Agent without
        model settings lets the SDK infer them from the model name, and
        for ``gpt-5.6-sol`` it infers ``none``, i.e. no reasoning at all
        on the hardest C++ generation in the pipeline.
    verbosity:
        ``text.verbosity`` for the run.
    request_timeout_seconds:
        Per-request HTTP timeout handed to the SDK's OpenAI client. The
        SDK's own default is 600s, which a large CodeTask at effort
        ``high`` can exceed -- and the failure lands after ~25 minutes of
        retries, taking the task, its dependents and the stage with it.
    runner:
        Optional injection point for tests. If supplied, the SDK is not
        imported and the callable is invoked directly with
        ``(inp, model)``.

    Returns
    -------
    A validated :class:`CodegenAgentResult`. ``files`` may be empty if
    the agent decided no work was required (the worker treats that as
    a failure for the task).
    """

    if runner is not None:
        result = runner(inp, model)
        if not isinstance(result, CodegenAgentResult):
            raise CodegenAgentError(
                f"runner returned {type(result).__name__}, expected CodegenAgentResult"
            )
        return result

    return _default_sdk_runner(
        inp,
        model,
        reasoning_effort=reasoning_effort,
        verbosity=verbosity,
        request_timeout_seconds=request_timeout_seconds,
    )


# ---------------------------------------------------------------------------
# Default runner: real OpenAI Agents SDK
# ---------------------------------------------------------------------------


def _default_sdk_runner(
    inp: CodegenAgentInput,
    model: str,
    *,
    reasoning_effort: str = "high",
    verbosity: str = "medium",
    request_timeout_seconds: float = 1800.0,
) -> CodegenAgentResult:
    """Production runner. Imports the SDK lazily so unit tests that
    inject a stub don't need the dependency installed."""

    try:
        # The Python SDK package is ``openai-agents`` on PyPI, import
        # path ``agents``. Verified against the official docs at
        # https://openai.github.io/openai-agents-python/.
        from agents import (  # type: ignore
            Agent,
            AgentOutputSchema,
            ModelSettings,
            OpenAIResponsesModel,
            Runner,
            WebSearchTool,
        )
        from openai import AsyncOpenAI  # type: ignore
        from openai.types.shared import Reasoning  # type: ignore
    except ImportError as exc:  # pragma: no cover - exercised only in real runs
        raise CodegenAgentError(
            "openai-agents is not installed. Add `openai-agents` to "
            "apps/orchestrator/pyproject.toml and `pip install -e "
            "apps/orchestrator`."
        ) from exc

    user_message = build_user_message(inp)

    # ``CodegenAgentResult.exports`` is a free-form ``dict`` and the
    # Pydantic fields carry ``max_length`` / ``max_items`` constraints.
    # These are not compatible with OpenAI strict JSON schema mode.
    # Wrapping with ``strict_json_schema=False`` lets the model emit
    # valid JSON freely; the SDK still validates via the Pydantic model.
    output_schema = AgentOutputSchema(
        CodegenAgentResult, strict_json_schema=False
    )

    # Explicit client, for the timeout only. The SDK's shared default
    # client caps a request at 600s; a large CodeTask at effort ``high``
    # runs past that, and the SDK's two retries turn one slow task into
    # ~25 wasted minutes and a dead stage. Everything else (api key,
    # base_url) still comes from the environment exactly as before.
    agent_model = OpenAIResponsesModel(
        model=model,
        openai_client=AsyncOpenAI(timeout=request_timeout_seconds),
    )

    agent = Agent(
        name=f"ptv-codegen-{inp.task_id}",
        instructions=inp.instructions,
        model=agent_model,
        # Explicit, and it has to be. When an Agent is constructed without
        # model settings, the SDK back-fills them from the model NAME:
        #
        #   if self.model is not None and self.model_settings == \
        #           get_default_model_settings():
        #       self.model_settings = _initial_model_settings_for_model(self.model)
        #
        # Its table maps ``gpt-5`` to effort ``low`` but ``gpt-5.6-sol`` to
        # ``none``. This stage used to pass nothing, so the model rename
        # alone would have silently stripped reasoning from the C++
        # generation that has to compile against the engine API first try.
        model_settings=ModelSettings(
            reasoning=Reasoning(effort=reasoning_effort),
            verbosity=verbosity,
        ),
        # ``WebSearchTool`` is the hosted web-search tool. We pass NO
        # numeric cap on calls — the cap is implicit in the agent's
        # own loop budget. See multi_agent_update.md, Part B5.
        #
        # Note this keeps us on the Responses API, which the SDK uses by
        # default. That matters on 5.6: reasoning combined with function
        # tools is Responses-only.
        tools=[WebSearchTool()],
        output_type=output_schema,
    )

    try:
        run_result = Runner.run_sync(agent, user_message)
    except Exception as exc:  # pragma: no cover - depends on live SDK
        raise CodegenAgentError(
            f"Agents SDK run failed for task {inp.task_id!r}: {exc}"
        ) from exc

    # ``final_output_as`` re-validates against our Pydantic model.
    try:
        out = run_result.final_output_as(CodegenAgentResult)
    except Exception as exc:  # pragma: no cover - depends on live SDK
        raise CodegenAgentError(
            f"Agents SDK returned non-conforming output for {inp.task_id!r}: {exc}"
        ) from exc
    return out


# ---------------------------------------------------------------------------
# User-message construction (also used by tests to verify wiring)
# ---------------------------------------------------------------------------


def build_user_message(inp: CodegenAgentInput) -> str:
    """Build the user-turn message handed to the agent.

    The string encodes the four-step CodeTask agent process so the
    contract is visible to both the SDK system prompt
    (``CODE_TASK_SYSTEM``) AND the per-task user message.
    """

    extras = (
        "\n".join(f"- {line}" for line in inp.extra_instructions)
        if inp.extra_instructions
        else "(none)"
    )

    # For code-edit tasks, the base built-in source is injected so the agent
    # copies-and-edits rather than rewriting from scratch.
    base_source_section = (
        (
            "\n================ BASE BUILT-IN SOURCE (copy + edit; do NOT "
            "replace the built-in) ================\n"
            f"{inp.base_source_block}\n"
        )
        if inp.base_source_block
        else ""
    )

    # On a compile-repair re-run, surface the compiler errors + current file
    # contents FIRST so the agent fixes the existing files instead of
    # re-authoring from scratch.
    repair_section = (
        (
            "\n================ COMPILE REPAIR (HIGHEST PRIORITY) ================\n"
            "Your PREVIOUS attempt at this task was written to disk but FAILED "
            "to compile. Return the COMPLETE corrected files[] (same schema, "
            "same output_subdir, same registered id). Fix the C++ so it "
            "compiles against the engine's public headers. Do not remove the "
            "PTV_REGISTER_* registration. Keep the same behaviour; only change "
            "what is needed to compile.\n"
            f"{inp.repair_context}\n"
        )
        if inp.repair_context
        else ""
    )

    return (
        "You are running ONE CodeTask in the Prompt-To-Native-VR "
        "multi-agent code-generation stage.\n"
        f"project_id: {inp.project_id}\n"
        f"task_id: {inp.task_id}\n"
        f"output_subdir: {inp.output_subdir}\n"
        f"{repair_section}\n"
        "Follow this four-step process strictly:\n"
        "  1) Read the task metadata block below (id, kind, interface, "
        "depends_on, headers, planner files[] hints, asset/scene "
        "references).\n"
        "  2) Read the ledger snapshot and the COMPLETED DEPENDENCY EXPORTS "
        "block. Plug into the public surface those dependency tasks "
        "produced (registered ids, registration macros, profile ids); "
        "never re-implement them.\n"
        "  3) OPTIONALLY use the web_search tool. There is NO numeric cap "
        "on web-search calls. Research VR / OpenXR patterns, native C++ "
        "technique, math, VFX intent — anything that improves the "
        "mechanic. Reject Unity/Unreal/Godot/browser-VR/WebXR snippets.\n"
        "  4) Implement the mechanic as ONE OR MORE C++ files. Every "
        f"path in files[] MUST start with ``{inp.output_subdir}/``. "
        "Decide the natural file split for this mechanic (registration, "
        "simulation, profile, helpers, vfx hookup, ...). Single-file "
        "output is a fallback, not the default.\n\n"
        "Return a structured JSON object matching the "
        "CodegenAgentResult schema:\n"
        "  files: [{path, contents}, ...]   # 1..16 entries\n"
        "  exports: { ... }                  # public surface for "
        "downstream tasks\n"
        "  notes: \"...\"                   # optional human-readable notes\n\n"
        "EXTRA INSTRUCTIONS:\n"
        f"{extras}\n\n"
        "================ CODE TASK ================\n"
        f"{inp.task_json}\n\n"
        "================ LEDGER SNAPSHOT (codegen_board.json) ================\n"
        f"{inp.ledger_snapshot_json}\n\n"
        "================ COMPLETED DEPENDENCY EXPORTS ================\n"
        f"{inp.dep_exports_json}\n\n"
        "================ SCENE SPEC EXCERPTS ================\n"
        f"{inp.scene_context_json}\n\n"
        "================ BUILD SPEC EXCERPT ================\n"
        f"{inp.build_spec_excerpt}\n"
        + (
            "\n================ NPC PLAN ================\n"
            "What every character in this game is supposed to do, decided "
            "once so all tasks agree. Characters here are ALREADY bound to "
            "builtin.character_locomotion: write CLocomotionIntent, never "
            "CTransform, and never play their walk/run clips yourself.\n"
            f"{inp.npc_plan_json}\n"
            if inp.npc_plan_json
            else ""
        )
        + (
            "\n================ ANIMATION INVENTORY ================\n"
            "The animation keys each character ACTUALLY has. Ask play_clip only "
            "for keys listed here; a 'missing' action has no clip, so check "
            "play_clip's return and apply that character's animation_fallback. "
            "Keys like 'run.punching' are gait variants: set "
            "want.gait_variant = \"punching\" on CLocomotionIntent while the "
            "character acts on the move -- never play_clip them.\n"
            f"{inp.animation_inventory_json}\n"
            if inp.animation_inventory_json
            else ""
        )
        + (
            "\n================ GENERATED ENVIRONMENTS ================\n"
            "What the world architects actually built and DECLARED. These ids "
            "are real; anything not listed here does not exist.\n"
            "  * Read and change an object's state with ctx.interactive: "
            'get_state(id, key) / set_state(id, key, value). The state keys '
            "listed per object are the only ones it has.\n"
            "  * Never write CTransform on a declared interactive -- its "
            "affordance built-in owns that motion, and a second writer fights "
            "it every frame.\n"
            "  * Persist facts that belong to no object (mission_complete, "
            "score) with ctx.persistence. Object state persists on its own "
            "through ctx.interactive; do not save it a second time.\n"
            "  * Put the player INSIDE a character's head with "
            "builtin.head_interior on the host, naming the nested interior's "
            "scene id. Never build that yourself: the interior is a nested "
            "environment from the world stage, not an asset, and the mechanic "
            "already handles mounting it, shrinking the player and routing "
            "head and stick input.\n"
            "  * Put the player ON something that moves -- a vehicle, lift, "
            "boat, platform, or a character they ride inside -- with "
            "ctx.locomotion->set_reference_frame(entity, anchor), and "
            "clear_reference_frame() to get off. The parent's motion then "
            "carries them without their local position drifting, so standing "
            "still on a moving platform arrives somewhere else. NEVER move the "
            "player every frame to follow a platform yourself: that fights "
            "locomotion and drifts. Resize them with "
            "ctx.locomotion->set_player_scale(s) (1.0 = life size); clip "
            "planes and their collision capsule follow.\n"
            "  * Build a menu with builtin.ui_panel and ctx.ui, never from "
            "scratch: set_rows(panel_id, rows, count) each frame with what is "
            "true now, then poll_click(panel_id, &click) in a loop and act on "
            "click.row_id. Rows are matched by ID, not index, so rebuilding "
            "the list every frame is the correct thing to do. A wardrobe is "
            "this plus ctx.equip; a mission select is this plus "
            "ctx.transition.\n"
            "  * Move the player between scenes with "
            'ctx.transition->go_to(scene_id, spawn_point_id), using ids from '
            "the lists below. It only REQUESTS -- the engine performs the swap "
            "at a frame boundary, because tearing the world down inside a "
            "mechanic tick would destroy the entities being iterated. Portals "
            "the player physically walks through already work with no code; "
            "call this only for a transition gameplay decides (a menu choice, "
            "a completed objective).\n"
            f"{inp.world_context_json}\n"
            if inp.world_context_json
            else ""
        )
        + f"{base_source_section}"
    )


# ---------------------------------------------------------------------------
# Convenience: build dep_exports from a board snapshot
# ---------------------------------------------------------------------------


def collect_dep_exports(
    board_snapshot: dict[str, Any], depends_on: list[str]
) -> dict[str, Any]:
    """Pull ``{dep_id: row.exports + files_written}`` from a board
    snapshot. Used by ``stage_code_generation`` before handing the
    agent its input bundle."""

    tasks = board_snapshot.get("tasks", {})
    out: dict[str, Any] = {}
    for dep in depends_on:
        row = tasks.get(dep)
        if row is None:
            continue
        out[dep] = {
            "exports": dict(row.get("exports", {})),
            "files_written": list(row.get("files_written", [])),
            "status": row.get("status"),
        }
    return out


def dep_exports_json(
    board_snapshot: dict[str, Any], depends_on: list[str]
) -> str:
    return json.dumps(
        collect_dep_exports(board_snapshot, depends_on),
        indent=2,
        sort_keys=True,
    )
