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

    files: List[CodeTaskFile] = Field(default_factory=list, max_length=16)
    test_files: List[CodeTaskFile] = Field(default_factory=list, max_length=8)

    acceptance_notes: str = Field(default="", max_length=2000)
