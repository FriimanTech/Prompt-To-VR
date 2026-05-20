"""SceneSpec: deterministic entity / component layout for a scene."""

from __future__ import annotations

from typing import Dict, List, Optional

from pydantic import Field

from .common import IdStr, QuatXYZW, StrictModel, Vec3


class Transform(StrictModel):
    position: Vec3 = Field(default_factory=Vec3)
    rotation: QuatXYZW = Field(default_factory=QuatXYZW)
    scale: float = Field(default=1.0, gt=0.0, lt=1000.0)


class MeshRef(StrictModel):
    asset_id: IdStr
    glb_path: str = Field(
        ...,
        max_length=512,
        description="Project-relative path under projects/<id>/assets/",
    )


class MechanicBinding(StrictModel):
    mechanic_id: str = Field(
        ...,
        max_length=128,
        description=(
            "Either 'builtin.<name>' for a reusable mechanic or"
            " 'generated.<code_task_id>' for a generated module."
        ),
    )
    params: Dict[str, object] = Field(default_factory=dict)


class PhysicsProps(StrictModel):
    kind: str = Field(default="static", max_length=32)  # static | dynamic | kinematic
    mass_kg: float = Field(default=1.0, ge=0.0, lt=10_000.0)
    collision_shape: str = Field(default="aabb", max_length=32)
    restitution: float = Field(default=0.2, ge=0.0, le=1.0)
    friction: float = Field(default=0.5, ge=0.0, le=2.0)


class EntitySpec(StrictModel):
    id: IdStr
    name: str = Field(..., max_length=128)
    transform: Transform = Field(default_factory=Transform)
    mesh: Optional[MeshRef] = None
    physics: Optional[PhysicsProps] = None
    mechanic_bindings: List[MechanicBinding] = Field(default_factory=list)
    tags: List[str] = Field(default_factory=list, max_length=16)


class LightSpec(StrictModel):
    id: IdStr
    kind: str = Field(default="directional", max_length=32)  # directional | point
    transform: Transform = Field(default_factory=Transform)
    color_rgb: List[float] = Field(default_factory=lambda: [1.0, 1.0, 1.0])
    intensity: float = Field(default=1.0, ge=0.0, le=20.0)


class PlayerSpawn(StrictModel):
    transform: Transform = Field(default_factory=Transform)
    face_direction: Vec3 = Field(
        default_factory=lambda: Vec3(x=0.0, y=0.0, z=-1.0)
    )


class SceneSpec(StrictModel):
    prompt_id: IdStr
    scene_id: IdStr
    name: str = Field(..., max_length=128)

    player_spawn: PlayerSpawn = Field(default_factory=PlayerSpawn)
    entities: List[EntitySpec] = Field(..., min_length=0, max_length=1000)
    lights: List[LightSpec] = Field(default_factory=list, max_length=32)

    sky_color: List[float] = Field(
        default_factory=lambda: [0.05, 0.07, 0.11]
    )
    ambient_intensity: float = Field(default=0.25, ge=0.0, le=5.0)
