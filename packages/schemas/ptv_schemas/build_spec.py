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
    # Full VR-game scope: scene summaries may include layout, story beats,
    # encounters, lighting mood, audio direction, etc. so the downstream
    # SceneSpec stage has enough authoring detail to compose a real level.
    summary: str = Field(..., max_length=4000)
    entry_point: bool = False
    expected_playtime_minutes: float = Field(
        default=5.0, ge=0.5, le=240.0
    )


class MechanicEntry(StrictModel):
    id: IdStr
    name: str = Field(..., max_length=128)
    # Mechanic summaries are the primary brief the MechanicPlan / CodeTask
    # stages consume. Keep them long enough to describe Quest 3 button
    # bindings, edge cases, comfort rules, etc.
    summary: str = Field(..., max_length=4000)
    depends_on_assets: List[IdStr] = Field(default_factory=list)
    required_in_scenes: List[IdStr] = Field(default_factory=list)


class AssetEntry(StrictModel):
    id: IdStr
    name: str = Field(..., max_length=128)
    category: str = Field(..., max_length=32)
    # Asset summaries flow into image prompts + style notes; full-game
    # prompts often include explicit shot direction / mood / material
    # cues so we allow up to 4000 chars here.
    summary: str = Field(..., max_length=4000)


class TestEntry(StrictModel):
    id: IdStr
    description: str = Field(..., max_length=2000)
    automated: bool = False


class BuildSpec(StrictModel):
    """Top-level specification for a generation run.

    Sizing notes (updated for full VR-game generation):

    * ``scenes``  — 1..50. Many prompts describe hub + multiple mission
      / training / arena spaces, so we lift the cap from 6 -> 50.
    * ``mechanics`` — 1..unbounded. A fully detailed VR game can call
      for dozens of distinct mechanics (movement, combat, abilities,
      UI, save / load, NPC interaction, etc.). The planner is
      responsible for keeping the list realistic.
    * ``assets`` — 1..48. Per-game asset budgets are still bounded by
      the Tripo / Gemini cost ceiling; this matches the AssetManifest
      cap.
    * ``acceptance_tests`` — 1..unbounded. The number of checks scales
      with the number of mechanics.
    * ``constraints`` — up to 50 free-form rules (raised from 32).
    """

    prompt_id: IdStr
    title: str = Field(..., max_length=128)
    pitch: str = Field(..., max_length=2000)
    game_type: GameType
    locomotion: LocomotionStyle = LocomotionStyle.teleport
    comfort: ComfortLevel = ComfortLevel.comfort
    player_height_meters: float = Field(default=1.7, gt=0.0, lt=3.0)

    scenes: List[SceneEntry] = Field(..., min_length=1, max_length=50)
    mechanics: List[MechanicEntry] = Field(..., min_length=1)
    assets: List[AssetEntry] = Field(..., min_length=1, max_length=48)

    npc_summary: Optional[str] = Field(default=None, max_length=4000)
    hud_summary: Optional[str] = Field(default=None, max_length=4000)
    constraints: List[str] = Field(default_factory=list, max_length=50)
    acceptance_tests: List[TestEntry] = Field(..., min_length=1)
