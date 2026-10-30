"""InteractionDesign: how every object in the game is used, decided once.

The question this answers is the one no other artifact asks: *what does the
player (or an NPC) do with this thing?* A pistol is gripped by its handle and
can be steadied with the other hand wrapped around the first; a rifle is held
by its pistol grip and supported under the handguard; a Captain America
shield is held by the far strap with the forearm through the near one; a book
is not held at all. The same mesh category ("prop") covers all of them, so
nothing downstream could tell them apart before this stage existed and every
prop got the same two guessed sockets.

From the answer the pipeline derives everything else about the item:

* ``sockets`` -- the named points ``socket_generation`` must find on the mesh,
  each with a ``part_description`` that is literally what the vision model is
  asked to segment;
* ``hold`` -- how the runtime solves the item into one or two hands;
* ``weapon`` / ``melee`` / ``shield`` / ``bow`` -- which engine built-in runs
  it, with its tuning taken from the prompt or from research;
* ``projectiles`` -- the projectile objects a weapon fires, including whether
  one needs a mesh at all (a 9 mm round crosses five metres per frame and is
  never seen; a rocket or an arrow is).

Every decision records where it came from, exactly like GameDesign:
``explicit`` decisions quote the prompt verbatim (checked), ``inferred`` ones
carry a rationale and, when researched, their sources. Explicit always wins
("a Glock that fires 7.62 mm rounds" fires 7.62 mm rounds).
"""

from __future__ import annotations

from enum import Enum
from typing import List, Optional

from pydantic import Field

from .common import IdStr, StrictModel
from .game_design import Provenance


class ItemUsage(str, Enum):
    held_weapon = "held_weapon"
    held_tool = "held_tool"
    held_shield = "held_shield"
    thrown = "thrown"
    worn = "worn"
    #: Spare ammunition, magazines, arrows in a quiver.
    ammo = "ammo"
    #: The object a weapon fires (bullet, rocket, arrow, bolt).
    projectile = "projectile"
    static_prop = "static_prop"
    not_interactive = "not_interactive"


class Wielder(str, Enum):
    player = "player"
    npc = "npc"


class HoldKind(str, Enum):
    #: One hand on one grip socket (a pistol, a wand, a torch, a one-handed
    #: sword, a hammer).
    one_hand = "one_hand"
    #: Two hands on two points along the item; the line between them steers
    #: it (a rifle's grip + handguard, a two-handed sword's hilt, a spear).
    two_hand_line = "two_hand_line"
    #: The support hand wraps the PRIMARY HAND rather than a second point on
    #: the item (a two-handed pistol stance).
    two_hand_wrap = "two_hand_wrap"
    #: Hand on a strap, forearm through a second strap (a round shield).
    forearm_strap = "forearm_strap"
    none = "none"


class TwoHandMode(str, Enum):
    none = "none"
    #: The item works one-handed; a second hand steadies it.
    optional = "optional"
    #: The item does not fire / strike until both hands hold it.
    required = "required"


class FingerPose(str, Enum):
    #: Full curl around a handle.
    wrap = "wrap"
    #: Wrap with the index finger on the trigger (follows the trigger value).
    trigger = "trigger"
    #: Closed around a strap.
    strap = "strap"
    pinch = "pinch"
    #: Cupped support under a handguard / around the other hand.
    open_support = "open_support"


class SocketKind(str, Enum):
    # Hand sockets: stored in the OpenXR GRIP frame (see docs/sockets.md):
    # origin on the gripped part's axis at palm height, -Z little finger ->
    # thumb along the part, +X the item's right, +Y completing the frame.
    hand_grip = "hand_grip"
    support_grip = "support_grip"
    support_wrap = "support_wrap"
    hand_strap = "hand_strap"
    forearm_strap = "forearm_strap"
    # Facing sockets: +Z is the direction the point faces, +Y up.
    muzzle = "muzzle"
    projectile_nose = "projectile_nose"
    projectile_tail = "projectile_tail"
    arrow_nock = "arrow_nock"
    arrow_rest = "arrow_rest"
    string_rest = "string_rest"
    limb_tip_top = "limb_tip_top"
    limb_tip_bottom = "limb_tip_bottom"
    magwell = "magwell"
    strike_base = "strike_base"
    strike_tip = "strike_tip"
    strike_face = "strike_face"
    attach_point = "attach_point"


#: Kinds whose frame is the OpenXR grip frame rather than "+Z faces".
HAND_SOCKET_KINDS = frozenset(
    {
        SocketKind.hand_grip,
        SocketKind.support_grip,
        SocketKind.support_wrap,
        SocketKind.hand_strap,
        SocketKind.forearm_strap,
    }
)


class SocketSpec(StrictModel):
    #: The name gameplay reads: ``ctx.sockets->socket_world(e, "<name>")``.
    #: The hand socket of a one-handed item is conventionally ``grip``.
    name: IdStr
    kind: SocketKind
    #: What the vision model is asked to find, in plain words about THIS
    #: item: "the pistol's handle below the slide, where the palm wraps".
    part_description: str = Field(..., min_length=3, max_length=600)
    required: bool = True


class HoldProfile(StrictModel):
    kind: HoldKind = HoldKind.none
    #: Guns, launchers, wands: the item points where the controller aims,
    #: rotated about the palm. Swords, tools and shields sit in the hand as
    #: the grip socket says.
    aim_aligned: bool = False
    two_hand: TwoHandMode = TwoHandMode.none
    primary_socket: str = Field(default="", max_length=64)
    support_socket: str = Field(default="", max_length=64)
    forearm_socket: str = Field(default="", max_length=64)
    finger_pose: FingerPose = FingerPose.wrap
    provenance: Provenance = Field(default_factory=Provenance)


class FireSystem(str, Enum):
    #: Instant ray from the muzzle; no object travels.
    hitscan = "hitscan"
    #: A simulated object travels over time and hits what it reaches.
    projectile = "projectile"
    #: Instant ray decides the hit; a visible tracer / bolt flies to it.
    hybrid = "hybrid"
    #: A continuous beam, ray-tested every frame while the trigger is held.
    beam = "beam"
    #: Draw-and-release launcher (a bow, a slingshot).
    drawn_launcher = "drawn_launcher"
    #: The item itself is thrown (grenade, knife).
    thrown = "thrown"
    none = "none"


class FireMode(str, Enum):
    semi = "semi"
    burst = "burst"
    auto = "auto"
    charge = "charge"
    hold_beam = "hold_beam"


class ReloadMode(str, Enum):
    #: A face button reloads after ``reload_time_s``.
    button = "button"
    #: Reloads itself when the magazine runs dry.
    auto = "auto"
    #: Eject, take a magazine from an item source, insert it in the magwell.
    magazine_insert = "magazine_insert"
    #: Energy weapons: heat builds per shot and vents over time.
    overheat = "overheat"
    none = "none"


class AmmoSpec(StrictModel):
    #: Calibre or ammunition family, e.g. "9x19mm Parabellum", "energy cell".
    calibre: str = Field(default="", max_length=160)
    infinite: bool = False
    magazine: int = Field(default=0, ge=0, le=10_000)
    reserve: int = Field(default=0, ge=0, le=100_000)
    reload: ReloadMode = ReloadMode.none
    reload_time_s: float = Field(default=1.5, ge=0.0, le=30.0)
    heat_per_shot: float = Field(default=0.0, ge=0.0, le=1.0)
    heat_vent_per_s: float = Field(default=0.0, ge=0.0, le=10.0)
    provenance: Provenance = Field(default_factory=Provenance)


class ShotVisual(str, Enum):
    #: Stretched streak from muzzle to hit (bullets).
    tracer = "tracer"
    #: Glowing bolt (blasters).
    energy_bolt = "energy_bolt"
    #: The projectile's own mesh.
    mesh = "mesh"
    none = "none"


class ImpactKind(str, Enum):
    sparks = "sparks"
    blood = "blood"
    energy = "energy"
    dust = "dust"
    explosion = "explosion"
    none = "none"


class BeamSpec(StrictModel):
    damage_per_s: float = Field(default=40.0, ge=0.0, le=100_000.0)
    width_m: float = Field(default=0.02, gt=0.0, le=1.0)
    color_rgb: List[float] = Field(default_factory=lambda: [1.0, 0.15, 0.1])
    max_length_m: float = Field(default=60.0, gt=0.0, le=2000.0)


class RecoilSpec(StrictModel):
    kick_back_m: float = Field(default=0.02, ge=0.0, le=0.3)
    kick_up_deg: float = Field(default=4.0, ge=0.0, le=45.0)
    recover_s: float = Field(default=0.08, ge=0.01, le=2.0)


class WeaponSpec(StrictModel):
    """A ranged weapon, run by ``builtin.weapon`` (or ``builtin.bow``)."""

    fire_system: FireSystem
    fire_mode: FireMode = FireMode.semi
    rpm: float = Field(default=300.0, gt=0.0, le=6000.0)
    burst_count: int = Field(default=3, ge=1, le=20)
    pellets: int = Field(default=1, ge=1, le=64)
    spread_deg: float = Field(default=0.5, ge=0.0, le=45.0)
    range_m: float = Field(default=100.0, gt=0.0, le=5000.0)
    damage: float = Field(default=25.0, ge=0.0, le=100_000.0)
    #: Damage at ``range_m`` as a fraction of ``damage`` (linear falloff).
    falloff_at_range: float = Field(default=1.0, ge=0.0, le=1.0)
    headshot_mult: float = Field(default=1.0, ge=1.0, le=20.0)
    ammo: AmmoSpec = Field(default_factory=AmmoSpec)
    #: ``ProjectileSpec.id`` for projectile / hybrid / drawn_launcher.
    projectile_id: str = Field(default="", max_length=64)
    visual: ShotVisual = ShotVisual.tracer
    #: Hybrid / automatic weapons: draw a tracer every Nth round.
    tracer_every_n: int = Field(default=1, ge=1, le=20)
    color_rgb: List[float] = Field(default_factory=lambda: [1.0, 0.85, 0.5])
    beam: Optional[BeamSpec] = None
    muzzle_flash: bool = True
    impact: ImpactKind = ImpactKind.sparks
    recoil: RecoilSpec = Field(default_factory=RecoilSpec)
    haptic_amplitude: float = Field(default=0.6, ge=0.0, le=1.0)
    provenance: Provenance = Field(default_factory=Provenance)


class ProjectileKind(str, Enum):
    bullet = "bullet"
    rocket = "rocket"
    arrow = "arrow"
    energy_bolt = "energy_bolt"
    grenade = "grenade"
    other = "other"


class OnHit(str, Enum):
    despawn = "despawn"
    stick = "stick"
    bounce = "bounce"
    explode = "explode"


class ProjectileSpec(StrictModel):
    id: IdStr
    kind: ProjectileKind
    calibre: str = Field(default="", max_length=160)
    #: Real (or intended) dimensions of the projectile object itself.
    diameter_m: float = Field(default=0.009, gt=0.0, le=2.0)
    length_m: float = Field(default=0.015, gt=0.0, le=5.0)
    speed_mps: float = Field(default=360.0, gt=0.0, le=5000.0)
    gravity_scale: float = Field(default=1.0, ge=0.0, le=10.0)
    drag: float = Field(default=0.0, ge=0.0, le=10.0)
    lifetime_s: float = Field(default=3.0, gt=0.0, le=60.0)
    #: Collision radius for the swept test; 0 = a ray.
    radius_m: float = Field(default=0.0, ge=0.0, le=2.0)
    on_hit: OnHit = OnHit.despawn
    splash_radius_m: float = Field(default=0.0, ge=0.0, le=50.0)
    splash_damage: float = Field(default=0.0, ge=0.0, le=100_000.0)
    #: True only when the object is slow or big enough to be SEEN (rockets,
    #: arrows, grenades, slow bolts) or the prompt asks to see it. A Tripo
    #: mesh is bought for it only then.
    needs_mesh: bool = False
    #: Manifest asset id of the mesh; filled by the manifest reconcile.
    asset_id: str = Field(default="", max_length=64)
    #: For ``asset_id`` generation: what the mesh looks like.
    visual_description: str = Field(default="", max_length=600)
    provenance: Provenance = Field(default_factory=Provenance)


class MeleeSpec(StrictModel):
    damage: float = Field(default=35.0, ge=0.0, le=100_000.0)
    #: Tip speed below which a swing does nothing (m/s).
    min_speed_mps: float = Field(default=1.5, ge=0.0, le=50.0)
    #: Damage multiplier per m/s above the threshold.
    speed_scale: float = Field(default=0.15, ge=0.0, le=10.0)
    cooldown_s: float = Field(default=0.4, ge=0.0, le=10.0)
    #: Blunt weapons strike with a face rather than along an edge.
    blunt: bool = False
    impact: ImpactKind = ImpactKind.sparks
    provenance: Provenance = Field(default_factory=Provenance)


class ShieldSpec(StrictModel):
    blocks_melee: bool = True
    blocks_projectiles: bool = True
    #: Fraction of a blocked hit's damage that still gets through.
    leak: float = Field(default=0.0, ge=0.0, le=1.0)
    provenance: Provenance = Field(default_factory=Provenance)


class ItemSourceSpec(StrictModel):
    """Where the player reaches to draw a copy of this item (arrows from a
    quiver over the shoulder, magazines from the hip)."""

    #: Body slot: ``back``, ``shoulder_r``, ``shoulder_l``, ``waist``,
    #: ``hip_r``, ``hip_l``, ``chest``. Empty = a world spot (``world_tag``).
    body_slot: str = Field(default="", max_length=32)
    world_tag: str = Field(default="", max_length=64)
    radius_m: float = Field(default=0.25, gt=0.0, le=2.0)
    #: 0 = unlimited.
    count: int = Field(default=0, ge=0, le=10_000)


class BowSpec(StrictModel):
    arrow_projectile_id: str = Field(..., max_length=64)
    #: Manifest asset id of the arrow the player holds and nocks.
    arrow_asset_id: str = Field(default="", max_length=64)
    max_draw_m: float = Field(default=0.6, gt=0.05, le=1.5)
    max_speed_mps: float = Field(default=70.0, gt=1.0, le=500.0)
    damage: float = Field(default=60.0, ge=0.0, le=100_000.0)
    arrow_source: ItemSourceSpec = Field(
        default_factory=lambda: ItemSourceSpec(body_slot="back")
    )
    provenance: Provenance = Field(default_factory=Provenance)


class RealWorldDimensions(StrictModel):
    """Measured dimensions of the real object this item depicts (or the
    prompt's own numbers). ``length_m`` is the longest extent."""

    length_m: float = Field(..., gt=0.0, le=200.0)
    height_m: float = Field(default=0.0, ge=0.0, le=200.0)
    width_m: float = Field(default=0.0, ge=0.0, le=200.0)
    source: str = Field(default="", max_length=600)


class ItemInteraction(StrictModel):
    asset_id: IdStr
    usage: ItemUsage
    wielders: List[Wielder] = Field(default_factory=list, max_length=2)
    #: "Glock 17", "M16A4", "Captain America's shield". Empty for generic.
    real_world_reference: str = Field(default="", max_length=400)
    real_world_dimensions: Optional[RealWorldDimensions] = None
    hold: HoldProfile = Field(default_factory=HoldProfile)
    sockets: List[SocketSpec] = Field(default_factory=list, max_length=12)
    weapon: Optional[WeaponSpec] = None
    melee: Optional[MeleeSpec] = None
    shield: Optional[ShieldSpec] = None
    bow: Optional[BowSpec] = None
    #: Spare copies the player draws from their body (magazines, arrows).
    item_source: Optional[ItemSourceSpec] = None
    provenance: Provenance = Field(default_factory=Provenance)

    def socket(self, name: str) -> Optional[SocketSpec]:
        return next((s for s in self.sockets if s.name == name), None)


class InteractionDesign(StrictModel):
    prompt_id: IdStr
    items: List[ItemInteraction] = Field(default_factory=list, max_length=64)
    projectiles: List[ProjectileSpec] = Field(default_factory=list, max_length=16)
    #: Systems deliberately left out ("no projectiles: a sword game").
    omitted: List[str] = Field(default_factory=list, max_length=32)
    notes: str = Field(default="", max_length=6000)

    def item(self, asset_id: str) -> Optional[ItemInteraction]:
        return next((i for i in self.items if i.asset_id == asset_id), None)

    def projectile(self, pid: str) -> Optional[ProjectileSpec]:
        return next((p for p in self.projectiles if p.id == pid), None)

    def weapons(self) -> List[ItemInteraction]:
        return [i for i in self.items if i.weapon is not None or i.bow is not None]
