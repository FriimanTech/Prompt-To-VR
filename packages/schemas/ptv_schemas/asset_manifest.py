"""AssetManifest: per-asset reference image + Tripo request instructions."""

from __future__ import annotations

from enum import Enum
from typing import List, Optional

from pydantic import Field

from .common import IdStr, StrictModel
from .performance import AssetImportance


class AssetCategory(str, Enum):
    environment = "environment"
    prop = "prop"
    character = "character"
    fx = "fx"
    ui = "ui"


class GenerationStrategy(str, Enum):
    image_to_3d = "image_to_3d"
    text_to_3d = "text_to_3d"


class TripoTextureQuality(str, Enum):
    standard = "standard"
    detailed = "detailed"


class TripoTextureAlignment(str, Enum):
    original_image = "original_image"
    geometry = "geometry"


class TripoRequest(StrictModel):
    """Shape of the Tripo image-to-3D / text-to-3D request.

    Textured, PBR, image-aligned defaults are REQUIRED per project policy.
    """

    texture: bool = True
    pbr: bool = True
    texture_quality: TripoTextureQuality = TripoTextureQuality.standard
    texture_alignment: TripoTextureAlignment = (
        TripoTextureAlignment.original_image
    )
    face_limit: Optional[int] = Field(default=None, ge=64, le=50000)
    model_version: Optional[str] = Field(
        default=None,
        description=(
            "Optional explicit model version (e.g. 'v2.5-20250123', 'p1')."
            " When None the client uses its default."
        ),
    )


class ReferenceImageRequest(StrictModel):
    """Reference image parameters sent to Gemini / Imagen.

    ``number_of_images`` is intentionally pinned to 1: nothing in the
    pipeline currently ranks / chooses between multiple candidate
    reference images for the same asset (the Tripo stage picks the
    first alphabetically). Allowing >1 just burns API credits with no
    quality benefit, so we cap the request at one image per asset
    until a real selection step exists.
    """

    prompt: str = Field(..., max_length=4000)
    aspect_ratio: str = Field(default="1:1", pattern=r"^\d{1,2}:\d{1,2}$")
    number_of_images: int = Field(default=1, ge=1, le=1)
    negative_prompt: Optional[str] = Field(default=None, max_length=1000)


class AssetItem(StrictModel):
    id: IdStr
    name: str = Field(..., max_length=128)
    category: AssetCategory
    style_notes: str = Field(..., max_length=2000)
    reference: ReferenceImageRequest
    strategy: GenerationStrategy = GenerationStrategy.image_to_3d
    tripo: TripoRequest = Field(default_factory=TripoRequest)
    target_scale_meters: float = Field(default=1.0, gt=0.0, lt=30.0)
    collision_shape: str = Field(
        default="aabb", max_length=32,
        description=(
            "Deterministic collision primitive: 'aabb', 'sphere', 'capsule',"
            " or 'mesh' (from GLB)."
        ),
    )
    rigged: bool = False
    socket_hints: List[str] = Field(
        default_factory=list,
        max_length=8,
        description=(
            "Named attachment points gameplay will need on this asset, e.g."
            " ['muzzle', 'grip'] for a weapon. The deterministic"
            " asset_contract stage derives their transforms from the geometry"
            " and the scene compiler writes them into scene.json, so generated"
            " C++ can fire from the barrel tip via"
            " ctx.sockets->socket_world(self, 'muzzle') without ever"
            " inspecting the mesh. Leave empty for scenery and any asset no"
            " mechanic attaches to."
        ),
    )
    # --- Optional performance hints -------------------------------------
    # Both are optional: the asset_optimization stage derives sensible values
    # from category/scale/interactivity when the planner leaves them unset.
    importance: Optional[AssetImportance] = Field(
        default=None,
        description=(
            "How much of the triangle budget this asset deserves. 'hero' for"
            " things inspected up close (characters, held props),"
            " 'background' for distant set dressing. Derived when omitted."
        ),
    )
    max_triangles: Optional[int] = Field(
        default=None, ge=100, le=200_000,
        description=(
            "Hard per-asset triangle cap after decimation. Overrides the"
            " budget allocator for this asset when set."
        ),
    )


class AssetManifest(StrictModel):
    prompt_id: IdStr
    items: List[AssetItem] = Field(..., min_length=1, max_length=48)
