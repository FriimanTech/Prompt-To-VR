"""SceneSpec: deterministic entity / component layout for a scene."""

from __future__ import annotations

from typing import Dict, List, Optional

from pydantic import Field

from .common import IdStr, QuatXYZW, StrictModel, Vec3


class Transform(StrictModel):
    position: Vec3 = Field(default_factory=Vec3)
    rotation: QuatXYZW = Field(default_factory=QuatXYZW)
    scale: float = Field(default=1.0, gt=0.0, lt=1000.0)


class SocketRef(StrictModel):
    """A named attachment point in the mesh's own local space.

    Derived deterministically from geometry by the ``asset_contract`` stage
    and converted from unit space to metres by the scene compiler, so the
    runtime can just compose ``entity_transform * socket``.
    """

    position: Vec3 = Field(default_factory=Vec3)
    rotation: QuatXYZW = Field(default_factory=QuatXYZW)


class MeshRef(StrictModel):
    asset_id: IdStr
    glb_path: str = Field(
        ...,
        max_length=512,
        description="Project-relative path under projects/<id>/assets/",
    )
    sockets: Dict[str, SocketRef] = Field(
        default_factory=dict,
        description=(
            "Named attachment points (e.g. 'muzzle', 'grip') in metres,"
            " relative to the mesh origin. Populated by the scene compiler"
            " from asset_contract.json; the SceneSpec agent leaves this empty."
        ),
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


class AttachmentRef(StrictModel):
    """Ride on a point of another entity instead of standing on your own.

    This is how a separately-generated weapon ends up in a separately-
    generated character's hand. Tripo sometimes returns "goblin holding a
    rifle" as one mesh and sometimes returns two, and only the second case
    needs this -- but it is common enough that a scene with no way to express
    it cannot build half the games it is asked for.
    """

    entity_id: IdStr = Field(
        ..., description="The entity to ride on, by scene entity id."
    )
    anchor: str = Field(
        default="grip",
        max_length=64,
        description=(
            "Socket or rig joint on the parent to follow. Named sockets"
            " ('muzzle', 'grip') win; otherwise the runtime matches a joint,"
            " including loose aliases like 'hand_r' / 'hand_l' / 'head' that"
            " work regardless of what Tripo named the bones."
        ),
    )
    position: Vec3 = Field(
        default_factory=Vec3,
        description="Offset from the anchor, in the anchor's space (metres).",
    )
    rotation: QuatXYZW = Field(
        default_factory=QuatXYZW,
        description="Rotation relative to the anchor.",
    )


class HealthProps(StrictModel):
    """Hit points for anything that can be damaged, including the player."""

    max: float = Field(default=100.0, gt=0.0, le=100_000.0)
    invuln_after_hit_s: float = Field(
        default=0.4,
        ge=0.0,
        le=10.0,
        description=(
            "Invulnerability window after each hit. Contact damage is tested"
            " every frame, so without one a single touch empties a health"
            " bar in a few frames."
        ),
    )


class FactionProps(StrictModel):
    """Which side an entity fights for.

    Needed so enemies engage the player's allied NPCs and not only the
    player. Use short stable ids: 'player', 'enemy', 'ally', 'wildlife'.
    """

    id: str = Field(default="neutral", max_length=32)
    hostile_to: List[str] = Field(default_factory=list, max_length=8)


class EntitySpec(StrictModel):
    id: IdStr
    name: str = Field(..., max_length=128)
    transform: Transform = Field(default_factory=Transform)
    mesh: Optional[MeshRef] = None
    physics: Optional[PhysicsProps] = None
    mechanic_bindings: List[MechanicBinding] = Field(default_factory=list)
    tags: List[str] = Field(default_factory=list, max_length=16)
    health: Optional[HealthProps] = Field(
        default=None,
        description=(
            "Give this entity hit points. Required for anything that can be"
            " killed or can take damage."
        ),
    )
    faction: Optional[FactionProps] = Field(
        default=None,
        description=(
            "Which side this entity is on. Required for anything that"
            " selects targets or can be targeted."
        ),
    )
    attached_to: Optional[AttachmentRef] = Field(
        default=None,
        description=(
            "Parent this entity to a point on another one. Its transform is"
            " then driven by the parent every frame, so the authored"
            " 'transform' above is only its pose before the first update."
        ),
    )


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
