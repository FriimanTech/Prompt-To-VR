"""BuildSpec: the top-level plan derived from the user's prompt."""

from __future__ import annotations

from enum import Enum
from typing import List, Optional

from pydantic import Field

from .common import IdStr, StrictModel


class GameType(str, Enum):
    arcade = "arcade"
    adventure = "adventure"
    puzzle = "puzzle"
    simulation = "simulation"
    shooter = "shooter"
    sandbox = "sandbox"
    narrative = "narrative"
    experience = "experience"


class LocomotionStyle(str, Enum):
    teleport = "teleport"
    smooth = "smooth"
    mixed = "mixed"
    stationary = "stationary"


class ComfortLevel(str, Enum):
    comfort = "comfort"
    standard = "standard"
    intense = "intense"


class SceneEntry(StrictModel):
    id: IdStr
    name: str = Field(..., max_length=128)
    summary: str = Field(..., max_length=2000)
    entry_point: bool = False
    expected_playtime_minutes: float = Field(
        default=5.0, ge=0.5, le=240.0
    )


class MechanicEntry(StrictModel):
    id: IdStr
    name: str = Field(..., max_length=128)
    summary: str = Field(..., max_length=2000)
    depends_on_assets: List[IdStr] = Field(default_factory=list)
    required_in_scenes: List[IdStr] = Field(default_factory=list)


class AssetEntry(StrictModel):
    id: IdStr
    name: str = Field(..., max_length=128)
    category: str = Field(..., max_length=32)
    summary: str = Field(..., max_length=2000)


class TestEntry(StrictModel):
    id: IdStr
    description: str = Field(..., max_length=2000)
    automated: bool = False


class BuildSpec(StrictModel):
    """Top-level specification for a generation run."""

    prompt_id: IdStr
    title: str = Field(..., max_length=128)
    pitch: str = Field(..., max_length=2000)
    game_type: GameType
    locomotion: LocomotionStyle = LocomotionStyle.teleport
    comfort: ComfortLevel = ComfortLevel.comfort
    player_height_meters: float = Field(default=1.7, gt=0.0, lt=3.0)

    scenes: List[SceneEntry] = Field(..., min_length=1, max_length=6)
    mechanics: List[MechanicEntry] = Field(..., min_length=1, max_length=20)
    assets: List[AssetEntry] = Field(..., min_length=1, max_length=48)

    npc_summary: Optional[str] = Field(default=None, max_length=2000)
    hud_summary: Optional[str] = Field(default=None, max_length=2000)
    constraints: List[str] = Field(default_factory=list, max_length=32)
    acceptance_tests: List[TestEntry] = Field(..., min_length=1, max_length=24)
