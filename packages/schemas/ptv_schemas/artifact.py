"""ArtifactMetadata: per-file metadata used by the orchestrator UI."""

from __future__ import annotations

from datetime import datetime, timezone
from enum import Enum
from typing import Optional

from pydantic import Field

from .common import HexSha256, IdStr, StrictModel


class ArtifactKind(str, Enum):
    json = "json"
    image = "image"
    glb = "glb"
    cpp = "cpp"
    header = "header"
    log = "log"
    binary = "binary"
    other = "other"


def _utcnow() -> datetime:
    return datetime.now(tz=timezone.utc)


class ArtifactMetadata(StrictModel):
    id: IdStr
    kind: ArtifactKind
    path: str = Field(..., max_length=1024)
    size_bytes: int = Field(..., ge=0)
    sha256: HexSha256
    stage: str = Field(..., max_length=64)
    created_at: datetime = Field(default_factory=_utcnow)
    description: Optional[str] = Field(default=None, max_length=1000)
