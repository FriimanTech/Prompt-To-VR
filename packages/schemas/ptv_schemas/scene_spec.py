"""SceneSpec: deterministic entity / component layout for a scene."""

from __future__ import annotations

from typing import Dict, List, Optional

from pydantic import Field, model_validator

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
    kind: str = Field(
        default="",
        max_length=32,
        description=(
            "Socket kind (hand_grip, muzzle, ...). Hand kinds are in the"
            " OpenXR grip frame; every other kind faces along +Z."
        ),
    )
    confidence: str = Field(default="", max_length=24)
    bore_diameter_m: Optional[float] = Field(default=None, ge=0.0, le=5.0)


class HoldRef(StrictModel):
    """How a held item is solved into the hand(s); compiled from the
    InteractionDesign + asset contract. The SceneSpec agent leaves it empty."""

    kind: str = Field(default="none", max_length=32)
    aim_aligned: bool = False
    two_hand: str = Field(default="none", max_length=16)
    primary_socket: str = Field(default="", max_length=64)
    support_socket: str = Field(default="", max_length=64)
    forearm_socket: str = Field(default="", max_length=64)
    finger_pose: str = Field(default="wrap", max_length=16)
    forward: Optional[Vec3] = None
    up: Optional[Vec3] = None


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
    hold: Optional[HoldRef] = Field(
        default=None,
        description=(
            "Hold profile for held items, compiled by the scene compiler from"
            " interaction_design.json. The SceneSpec agent leaves this empty."
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

    @model_validator(mode="after")
    def _movable_bodies_need_mass(self) -> "PhysicsProps":
        """A body that can move must weigh something.

        ``mass_kg`` stays ``ge=0`` because 0 is meaningful on a static body --
        it simply is not used. On a ``dynamic`` or ``kinematic`` body the
        physics engine divides by it to build an inverse inertia tensor, and
        Jolt asserts on the division. That assert is a breakpoint, which on a
        Quest is SIGTRAP: three kinematic targets authored with mass 0 killed
        a shipped APK during scene load with no error shown anywhere.

        Lifted rather than rejected because 0 is an easy and reasonable thing
        to write for a kinematic body -- it does mean "no force moves this" --
        and the correct expression of that intent is the kind, not the mass.
        """
        if self.kind in ("dynamic", "kinematic") and self.mass_kg <= 0.0:
            self.mass_kg = 1.0
        return self


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
    socket: str = Field(
        default="",
        max_length=64,
        description=(
            "Optional socket on THIS entity (e.g. 'grip') to seat on the"
            " anchor, so a rifle sits in an NPC's hand by its own handle."
            " Overrides position/rotation."
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


class EquipSlotSpec(StrictModel):
    """One place something can be equipped ON THIS ENTITY.

    Mirrors the runtime ``ptv::EquipSlot`` field for field. The distinction
    that matters: a mesh SOCKET is a point on an item ("the muzzle of this
    rifle"), while an equip SLOT is a place on a WEARER that an item can go
    ("the holster on this player's right hip"). Holstering is therefore
    declared on the player or the NPC, never on the holster prop.

    ``builtin.equip`` reads these to decide where a released item can snap,
    and its ``params.slots`` filters by ``name``. A wearer with no declared
    slots offers no candidates, so an equip binding on such an entity can
    never fire -- which is why the scene compiler fails the build for it.
    """

    name: str = Field(
        ...,
        max_length=64,
        description=(
            "Slot name gameplay refers to, e.g. 'hand_r', 'hip_holster_r',"
            " 'sword_sheath_l'. Must be unique on this entity."
        ),
    )
    body_slot: str = Field(
        default="",
        max_length=64,
        description=(
            "Canonical body slot this sits on: 'hand_r', 'hand_l', 'waist',"
            " 'wrist_l', 'wrist_r', 'chest', 'back', 'head'. Leave empty"
            " ONLY when the slot is hosted by another equipped item."
        ),
    )
    host_item_slot: str = Field(
        default="",
        max_length=64,
        description=(
            "Name of the slot whose equipped ITEM hosts this slot, for gear"
            " that mounts on other gear (a mask hose on a back tank). Empty"
            " for an ordinary body slot."
        ),
    )
    host_socket: str = Field(
        default="",
        max_length=64,
        description="Socket on the hosting item. Empty for a body slot.",
    )
    local_position: Vec3 = Field(
        default_factory=Vec3,
        description="Offset from the body slot or host socket, in metres.",
    )
    local_rotation: QuatXYZW = Field(default_factory=QuatXYZW)
    snap_radius_m: float = Field(
        default=0.0,
        ge=0.0,
        le=2.0,
        description=(
            "How close a released item must be to snap in. 0 means proximity"
            " is not required (menu- or script-driven equipping). Use"
            " 0.15-0.25 for a hip holster the player reaches for."
        ),
    )
    exclusive: bool = Field(
        default=True,
        description="One item at a time; equipping replaces the incumbent.",
    )
    accepts_tags: List[str] = Field(
        default_factory=list,
        max_length=8,
        description=(
            "Item tags this slot accepts, matched against the item entity's"
            " 'tags'. Empty accepts anything, which is how a sword ends up"
            " holstered just because it brushed the player's hip."
        ),
    )


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
    equip_slots: List[EquipSlotSpec] = Field(
        default_factory=list,
        max_length=24,
        description=(
            "Places items can be equipped ON THIS ENTITY. Declare these on"
            " the WEARER (the player rig or an NPC), not on the holster,"
            " sheath or rack prop. Required for 'builtin.equip' to have"
            " anywhere to put anything."
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
