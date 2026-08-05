"""CodeTask: a single bounded C++ generation job."""

from __future__ import annotations

from enum import Enum
from typing import List

from pydantic import Field

from .common import IdStr, StrictModel


class CodeTaskKind(str, Enum):
    mechanic = "mechanic"
    ai = "ai"
    dialogue = "dialogue"


class CodeTaskFile(StrictModel):
    path: str = Field(
        ...,
        max_length=256,
        description=(
            "Relative path starting with 'engine/generated/mechanics/',"
            " 'engine/generated/ai/', or 'engine/generated/dialogue/'."
        ),
    )
    contents: str = Field(..., max_length=120_000)


class CodeTask(StrictModel):
    id: IdStr
    kind: CodeTaskKind
    title: str = Field(..., max_length=200)
    # 10 000 chars is enough room for the planner to lay out:
    #   * the mechanic's behaviour in detail,
    #   * the exact Quest 3 button bindings (when the prompt names them),
    #   * the components it reads / writes on the entity,
    #   * edge cases, comfort rules, failure modes,
    #   * acceptance hooks the C++ generator should respect.
    description: str = Field(..., max_length=10_000)

    interface: str = Field(
        ...,
        description=(
            "Name of the abstract interface the generated module implements:"
            " 'IMechanic' | 'IBehaviour' | 'IDialogueHook'."
        ),
    )

    headers: List[str] = Field(
        default_factory=list,
        description=(
            "Whitelisted public engine headers that the generated code may"
            " include. Allowed: ptv/api/IMechanic.h, ptv/api/IBehaviour.h,"
            " ptv/api/IDialogueHook.h, ptv/ecs/World.h, ptv/math/Math.h,"
            " plus <entt/entt.hpp>, <nlohmann/json.hpp>, <glm/glm.hpp>."
        ),
        max_length=32,
    )

    # Other CodeTask ids in the SAME MechanicPlan that this task depends on.
    # The codegen ledger (``projects/<id>/codegen_board.json``) gates each
    # task: a task with any unresolved dependency stays ``blocked`` and is
    # promoted to ``pending`` only when every dep has reached ``done``.
    #
    # The planner is encouraged to set this whenever one ``generate`` task
    # builds on another (e.g. a weapon profile depends on a shooting-hub
    # task in the same plan). Empty list = no deps, task starts pending.
    depends_on: List[IdStr] = Field(
        default_factory=list,
        max_length=32,
        description=(
            "IDs of other CodeTasks in the same MechanicPlan that this task"
            " depends on. The codegen ledger blocks this task until every"
            " dep is 'done'."
        ),
    )

    # Named attachment points the generated code will look up through
    # ``ctx.sockets->socket_world(entity, name)`` -- e.g. ["muzzle"] for a
    # mechanic that fires from a barrel tip.
    #
    # This is one half of a contract whose other half (the actual per-asset
    # transforms) is produced much later, by the deterministic asset_contract
    # stage. Declaring the names here is what lets the scene compiler catch a
    # mismatch and fail loudly, instead of the mechanic silently falling back
    # to the front of a bounding sphere at runtime.
    required_sockets: List[str] = Field(
        default_factory=list,
        max_length=8,
        description=(
            "Socket names this mechanic reads via ctx.sockets. The scene"
            " compiler fails the build if a bound entity's asset does not"
            " provide every one of them. Leave empty unless the mechanic"
            " genuinely needs a spatial anchor on the mesh."
        ),
    )

    files: List[CodeTaskFile] = Field(default_factory=list, max_length=16)
    test_files: List[CodeTaskFile] = Field(default_factory=list, max_length=8)

    acceptance_notes: str = Field(default="", max_length=2000)
