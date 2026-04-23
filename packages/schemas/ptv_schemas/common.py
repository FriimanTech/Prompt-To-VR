"""Common primitives reused by several schemas."""

from __future__ import annotations

from typing import Annotated

from pydantic import BaseModel, ConfigDict, Field


class StrictModel(BaseModel):
    """Base model that forbids unknown fields and strips whitespace."""

    model_config = ConfigDict(
        extra="forbid",
        str_strip_whitespace=True,
        populate_by_name=True,
    )


IdStr = Annotated[
    str,
    Field(
        pattern=r"^[a-z0-9][a-z0-9_.\-]{0,63}$",
        description="Snake-case identifier, max 64 chars.",
    ),
]

SemVer = Annotated[
    str,
    Field(pattern=r"^\d+\.\d+\.\d+$", description="Semantic version x.y.z."),
]

HexSha256 = Annotated[
    str,
    Field(pattern=r"^[a-f0-9]{64}$", description="Lowercase hex sha-256."),
]


class Vec3(StrictModel):
    x: float = 0.0
    y: float = 0.0
    z: float = 0.0


class QuatXYZW(StrictModel):
    """Quaternion stored as (x, y, z, w), identity = (0,0,0,1)."""

    x: float = 0.0
    y: float = 0.0
    z: float = 0.0
    w: float = 1.0
