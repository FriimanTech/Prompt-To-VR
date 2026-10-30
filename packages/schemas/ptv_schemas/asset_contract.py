"""AssetContract: the measured facts about every asset that gameplay reads.

``asset_contract.json`` is a map from asset id to one entry. It was a free-form
dict for a long time, written by one stage and read by three others; this
model is the shape all of them agree on.

Each entry carries, when applicable:

* ``sockets`` -- named local transforms in UNIT space (positions divided by the
  mesh's longest-axis extent; the scene compiler multiplies by
  ``target_scale_meters``). Hand sockets use the OpenXR grip frame, every other
  kind faces along +Z (see ``docs/sockets.md``).
* ``hold`` -- how the runtime solves the item into one or two hands, copied
  from the InteractionDesign and resolved against the sockets that exist.
* ``part_regions`` -- triangle sets the vision pass identified (a bow's
  string, a magazine), stored beside the GLB and referenced by path.
* ``frame`` -- a character's measured model frame (sole, height, forward).
"""

from __future__ import annotations

from enum import Enum
from typing import Any, Dict, List, Optional

from pydantic import BaseModel, ConfigDict, Field, RootModel


class SocketConfidence(str, Enum):
    #: Vision found it and the render-and-verify pass confirmed it.
    vision_verified = "vision_verified"
    #: Vision found it; verification was skipped or inconclusive.
    vision = "vision"
    #: The part is not modelled (a shield with no straps): placed by rule.
    synthesized = "synthesized"
    #: Geometry-only fallback (PCA): no vision model was available.
    heuristic = "heuristic"
    #: Vision and verification disagreed; geometry fallback was used.
    low = "low"


class ContractSocket(BaseModel):
    model_config = ConfigDict(extra="allow")

    position: List[float] = Field(..., min_length=3, max_length=3)
    rotation: List[float] = Field(..., min_length=4, max_length=4)
    kind: str = ""
    confidence: SocketConfidence = SocketConfidence.heuristic
    #: Muzzles: diameter of the bore opening, unit space.
    bore_diameter_unit: Optional[float] = None
    #: Hand sockets: thickness of the gripped part at the palm, unit space.
    handle_thickness_unit: Optional[float] = None


class ContractHold(BaseModel):
    model_config = ConfigDict(extra="forbid")

    kind: str = "none"
    aim_aligned: bool = False
    two_hand: str = "none"
    primary_socket: str = ""
    support_socket: str = ""
    forearm_socket: str = ""
    finger_pose: str = "wrap"
    #: Facing used by aim-aligned holds, in mesh space (unit vectors).
    forward: Optional[List[float]] = None
    up: Optional[List[float]] = None


class AssetContractEntry(BaseModel):
    model_config = ConfigDict(extra="allow")

    status: str = "skipped"
    reason: str = ""
    space: str = "unit"
    sockets: Dict[str, ContractSocket] = Field(default_factory=dict)
    socket_confidence: Dict[str, str] = Field(default_factory=dict)
    socket_hints: List[str] = Field(default_factory=list)
    unsatisfied_hints: List[str] = Field(default_factory=list)
    bounds: Dict[str, Any] = Field(default_factory=dict)
    principal_axis: Optional[List[float]] = None
    source_glb: str = ""
    hold: Optional[ContractHold] = None
    #: part name -> repo-relative JSON file listing triangle indices.
    part_regions: Dict[str, str] = Field(default_factory=dict)
    #: Projectiles: measured cross-section diameter, unit space.
    cross_section_unit: Optional[float] = None
    overlay_paths: List[str] = Field(default_factory=list)
    warnings: List[str] = Field(default_factory=list)
    frame: Optional[Dict[str, Any]] = None


class AssetContract(RootModel[Dict[str, AssetContractEntry]]):
    def get(self, asset_id: str) -> Optional[AssetContractEntry]:
        return self.root.get(asset_id)
