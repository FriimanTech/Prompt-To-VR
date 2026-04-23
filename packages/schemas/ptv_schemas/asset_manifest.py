"""AssetManifest: per-asset reference image + Tripo request instructions."""

from __future__ import annotations

from enum import Enum
from typing import List, Optional

from pydantic import Field

from .common import IdStr, StrictModel


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
    texture_quality: TripoTextureQuality = TripoTextureQuality.detailed
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
    """Reference image parameters sent to Gemini / Imagen."""

    prompt: str = Field(..., max_length=4000)
    aspect_ratio: str = Field(default="1:1", pattern=r"^\d{1,2}:\d{1,2}$")
    number_of_images: int = Field(default=1, ge=1, le=4)
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


class AssetManifest(StrictModel):
    prompt_id: IdStr
    items: List[AssetItem] = Field(..., min_length=1, max_length=48)
