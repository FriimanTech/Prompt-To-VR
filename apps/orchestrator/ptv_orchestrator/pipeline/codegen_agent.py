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
    model: str = "gpt-5",
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
        Model id (``gpt-5``, ``gpt-5-mini``, etc). Must be compatible
        with the OpenAI Agents SDK. Default matches
        ``Settings.openai_hard_model``.
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

    return _default_sdk_runner(inp, model)


# ---------------------------------------------------------------------------
# Default runner: real OpenAI Agents SDK
# ---------------------------------------------------------------------------


def _default_sdk_runner(inp: CodegenAgentInput, model: str) -> CodegenAgentResult:
    """Production runner. Imports the SDK lazily so unit tests that
    inject a stub don't need the dependency installed."""

    try:
        # The Python SDK package is ``openai-agents`` on PyPI, import
        # path ``agents``. Verified against the official docs at
        # https://openai.github.io/openai-agents-python/.
        from agents import (  # type: ignore
            Agent,
            AgentOutputSchema,
            Runner,
            WebSearchTool,
        )
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

    agent = Agent(
        name=f"ptv-codegen-{inp.task_id}",
        instructions=inp.instructions,
        model=model,
        # ``WebSearchTool`` is the hosted web-search tool. We pass NO
        # numeric cap on calls — the cap is implicit in the agent's
        # own loop budget. See multi_agent_update.md, Part B5.
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
        f"{base_source_section}"
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
