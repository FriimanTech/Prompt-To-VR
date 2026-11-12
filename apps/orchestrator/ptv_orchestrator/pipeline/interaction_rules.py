"""Validate, repair and reconcile an InteractionDesign.

The designer agent decides how each item is used; this module guarantees that
what it decided is COMPLETE enough to run:

* every held item has a hold profile whose named sockets are declared;
* every aim-aligned / ranged item declares a muzzle to fire from;
* a strapped shield declares both straps, a bow its rest, string and limb
  tips, an arrow its nock and nose;
* every ranged weapon names a projectile when its fire system needs one, a
  beam carries beam parameters, a bow names its arrow;
* explicit decisions quote the prompt verbatim.

Repairs are deterministic and recorded as notes. Then
:func:`reconcile_manifest` makes the AssetManifest agree with the design:
``socket_hints`` come from the design, a real-world item's scale is corrected
from its researched dimensions, and every projectile that needs a mesh is
appended as a manifest item -- before reference_images and Tripo spend any
money, which is why this stage sits directly after asset_manifest.
"""

from __future__ import annotations

import re
from typing import Any, Iterable, Optional

from ptv_schemas import (
    AssetCategory,
    AssetManifest,
    BeamSpec,
    BowSpec,
    DecisionSource,
    ExplosiveKind,
    ExplosiveSpec,
    PressSpec,
    FingerPose,
    FireMode,
    FireSystem,
    HoldKind,
    HoldProfile,
    ImpactKind,
    InteractionDesign,
    ItemInteraction,
    ItemUsage,
    MeleeSpec,
    OnHit,
    ProjectileKind,
    ProjectileSpec,
    Provenance,
    ShieldSpec,
    ShotVisual,
    SocketKind,
    SocketSpec,
    TwoHandMode,
    WeaponSpec,
    Wielder,
)
from ptv_schemas.asset_manifest import (
    AssetItem,
    CharacterRole,
    ReferenceImageRequest,
)
from ptv_schemas.performance import AssetImportance

#: A real bullet faster than this is invisible at 72-90 Hz (it crosses
#: metres per frame), so it is fired as hybrid: ray hit + tracer.
VISIBLE_PROJECTILE_MAX_MPS = 150.0

_HELD = {ItemUsage.held_weapon, ItemUsage.held_tool, ItemUsage.held_shield,
         ItemUsage.thrown}
_RANGED = {FireSystem.hitscan, FireSystem.projectile, FireSystem.hybrid,
           FireSystem.beam}
_NEEDS_PROJECTILE = {FireSystem.projectile, FireSystem.hybrid,
                     FireSystem.drawn_launcher}


def _norm(text: str) -> str:
    return re.sub(r"\s+", " ", (text or "").lower()).strip()


def _provenances(design: InteractionDesign) -> Iterable[tuple[str, Provenance]]:
    for item in design.items:
        yield f"{item.asset_id}", item.provenance
        yield f"{item.asset_id}.hold", item.hold.provenance
        for name in ("weapon", "melee", "shield", "bow"):
            sub = getattr(item, name)
            if sub is not None:
                yield f"{item.asset_id}.{name}", sub.provenance
                if name == "weapon":
                    yield f"{item.asset_id}.weapon.ammo", sub.ammo.provenance
    for proj in design.projectiles:
        yield f"projectile {proj.id}", proj.provenance


# ---------------------------------------------------------------------------
# Validation
# ---------------------------------------------------------------------------


def interaction_violations(design: InteractionDesign, *, prompt: str,
                           manifest: AssetManifest) -> list[str]:
    """Every way the design cannot be run as-is, in words the agent can act
    on. Empty when it is sound."""

    out: list[str] = []
    items = {i.id: i for i in manifest.items}
    seen: set[str] = set()
    p = _norm(prompt)

    for it in design.items:
        if it.asset_id in seen:
            out.append(f"asset '{it.asset_id}' appears twice; one entry per asset")
        seen.add(it.asset_id)
        asset = items.get(it.asset_id)
        if asset is None:
            out.append(f"'{it.asset_id}' is not an AssetManifest item id")
            continue
        if asset.category == AssetCategory.character:
            out.append(f"'{it.asset_id}' is a character; characters attach by rig "
                       "joints and must not be in the InteractionDesign")
        names = [s.name for s in it.sockets]
        if len(set(names)) != len(names):
            out.append(f"'{it.asset_id}' declares a socket name twice")
        for ref, label in ((it.hold.primary_socket, "primary_socket"),
                           (it.hold.support_socket, "support_socket"),
                           (it.hold.forearm_socket, "forearm_socket")):
            if ref and ref not in names:
                out.append(f"'{it.asset_id}' hold.{label} '{ref}' is not in its sockets")
        if it.usage in _HELD and it.hold.kind == HoldKind.none and it.usage != ItemUsage.thrown:
            out.append(f"'{it.asset_id}' is {it.usage.value} but has no hold kind")
        if it.hold.kind != HoldKind.none and not it.hold.primary_socket:
            out.append(f"'{it.asset_id}' hold {it.hold.kind.value} names no primary_socket")
        if it.hold.kind == HoldKind.forearm_strap and not it.hold.forearm_socket:
            out.append(f"'{it.asset_id}' forearm_strap hold needs a forearm_socket "
                       "(the strap the forearm passes through)")
        if it.hold.kind in (HoldKind.two_hand_line, HoldKind.two_hand_wrap) \
                and not it.hold.support_socket:
            out.append(f"'{it.asset_id}' {it.hold.kind.value} hold needs a support_socket")
        if it.usage in (ItemUsage.static_prop, ItemUsage.not_interactive) and it.sockets:
            out.append(f"'{it.asset_id}' is {it.usage.value} but declares sockets; "
                       "scenery gets none")

        w = it.weapon
        if w is not None:
            if w.fire_system in _RANGED and not any(
                    s.kind == SocketKind.muzzle for s in it.sockets):
                out.append(f"'{it.asset_id}' fires ({w.fire_system.value}) but declares "
                           "no muzzle socket")
            if w.fire_system in _NEEDS_PROJECTILE and w.fire_system != FireSystem.drawn_launcher:
                if not w.projectile_id:
                    out.append(f"'{it.asset_id}' is {w.fire_system.value} and needs a "
                               "projectile_id")
                elif design.projectile(w.projectile_id) is None:
                    out.append(f"'{it.asset_id}' names projectile '{w.projectile_id}', "
                               "which is not in `projectiles`")
            if w.fire_system == FireSystem.beam and w.beam is None:
                out.append(f"'{it.asset_id}' is a beam weapon with no `beam` parameters")
            if w.fire_system == FireSystem.drawn_launcher and it.bow is None:
                out.append(f"'{it.asset_id}' is a drawn_launcher with no `bow`")
        if it.bow is not None:
            for kind in (SocketKind.arrow_rest, SocketKind.string_rest,
                         SocketKind.limb_tip_top, SocketKind.limb_tip_bottom):
                if not any(s.kind == kind for s in it.sockets):
                    out.append(f"bow '{it.asset_id}' needs a {kind.value} socket")
            if design.projectile(it.bow.arrow_projectile_id) is None:
                out.append(f"bow '{it.asset_id}' names arrow projectile "
                           f"'{it.bow.arrow_projectile_id}', not in `projectiles`")
        if it.melee is not None and not any(
                s.kind in (SocketKind.strike_tip, SocketKind.strike_face)
                for s in it.sockets):
            out.append(f"melee item '{it.asset_id}' needs strike_base + strike_tip "
                       "(blade) or strike_face (blunt) sockets")

    pids = [pr.id for pr in design.projectiles]
    if len(set(pids)) != len(pids):
        out.append("a projectile id appears twice")

    for where, prov in _provenances(design):
        if prov.source == DecisionSource.explicit:
            if not prov.prompt_quote:
                out.append(f"{where}: explicit decision without prompt_quote")
            elif _norm(prov.prompt_quote) not in p:
                out.append(f"{where}: prompt_quote {prov.prompt_quote!r} is not in the "
                           "prompt verbatim; quote it exactly or mark it inferred")
    return out


# ---------------------------------------------------------------------------
# Repair
# ---------------------------------------------------------------------------


def _ensure_socket(it: ItemInteraction, name: str, kind: SocketKind,
                   desc: str, notes: list[str]) -> str:
    existing = it.socket(name)
    if existing is not None:
        return name
    same_kind = next((s for s in it.sockets if s.kind == kind), None)
    if same_kind is not None:
        return same_kind.name
    it.sockets.append(SocketSpec(name=name, kind=kind, part_description=desc))
    notes.append(f"{it.asset_id}: added {kind.value} socket '{name}'")
    return name


def _label(asset: Optional[AssetItem], it: ItemInteraction) -> str:
    if it.real_world_reference:
        return it.real_world_reference
    return asset.name if asset is not None else it.asset_id.replace("_", " ")


def repair_interaction_design(design: InteractionDesign, *, prompt: str,
                              manifest: AssetManifest
                              ) -> tuple[InteractionDesign, list[str]]:
    """Complete what the agent left incomplete, deterministically."""

    d = design.model_copy(deep=True)
    notes: list[str] = []
    assets = {i.id: i for i in manifest.items}
    p = _norm(prompt)

    # Unknown assets, characters and duplicates out.
    kept: list[ItemInteraction] = []
    seen: set[str] = set()
    for it in d.items:
        asset = assets.get(it.asset_id)
        if asset is None or asset.category == AssetCategory.character \
                or it.asset_id in seen:
            notes.append(f"dropped interaction for '{it.asset_id}'")
            continue
        seen.add(it.asset_id)
        kept.append(it)
    d.items = kept

    for it in d.items:
        asset = assets.get(it.asset_id)
        label = _label(asset, it)
        if it.usage in (ItemUsage.static_prop, ItemUsage.not_interactive):
            if it.sockets or it.hold.kind != HoldKind.none:
                notes.append(f"{it.asset_id}: scenery keeps no sockets or hold")
            it.sockets = []
            it.hold = HoldProfile(provenance=it.hold.provenance)
            continue

        w = it.weapon
        ranged = w is not None and w.fire_system in _RANGED

        # A held item with no hold kind gets the obvious one.
        if it.usage in _HELD and it.hold.kind == HoldKind.none \
                and it.usage != ItemUsage.thrown:
            it.hold.kind = (HoldKind.forearm_strap if it.usage == ItemUsage.held_shield
                            else HoldKind.one_hand)
            notes.append(f"{it.asset_id}: hold kind defaulted to {it.hold.kind.value}")

        if it.hold.kind == HoldKind.forearm_strap:
            it.hold.primary_socket = _ensure_socket(
                it, it.hold.primary_socket or "hand_strap", SocketKind.hand_strap,
                f"the strap on the back of the {label} that the hand grips, farther "
                "from the body", notes)
            it.hold.forearm_socket = _ensure_socket(
                it, it.hold.forearm_socket or "forearm_strap", SocketKind.forearm_strap,
                f"the strap on the back of the {label} the forearm slides through, "
                "nearer the elbow", notes)
            it.hold.finger_pose = FingerPose.strap
            it.hold.aim_aligned = False
        elif it.hold.kind != HoldKind.none:
            it.hold.primary_socket = _ensure_socket(
                it, it.hold.primary_socket or "grip", SocketKind.hand_grip,
                f"the handle of the {label} where the dominant hand's palm wraps",
                notes)
            if it.hold.kind == HoldKind.two_hand_line:
                it.hold.support_socket = _ensure_socket(
                    it, it.hold.support_socket or "support_grip",
                    SocketKind.support_grip,
                    f"the second place on the {label} the other hand holds "
                    "(a handguard under the barrel, or lower on a long hilt)", notes)
                if it.hold.two_hand == TwoHandMode.none:
                    it.hold.two_hand = TwoHandMode.optional
            elif it.hold.kind == HoldKind.two_hand_wrap:
                it.hold.support_socket = _ensure_socket(
                    it, it.hold.support_socket or "grip_wrap", SocketKind.support_wrap,
                    f"the handle of the {label}; the support hand wraps the "
                    "shooting hand", notes)
                if it.hold.two_hand == TwoHandMode.none:
                    it.hold.two_hand = TwoHandMode.optional
            if it.hold.support_socket and it.hold.two_hand == TwoHandMode.none:
                it.hold.two_hand = TwoHandMode.optional

        if ranged:
            _ensure_socket(it, "muzzle", SocketKind.muzzle,
                           f"the open end of the {label}'s barrel or emitter where "
                           "the shot leaves", notes)
            it.hold.aim_aligned = True
            if it.hold.finger_pose == FingerPose.wrap:
                it.hold.finger_pose = FingerPose.trigger
            if Wielder.player not in it.wielders and Wielder.npc not in it.wielders:
                it.wielders = [Wielder.player]

        if w is not None:
            _repair_weapon(d, it, label, p, notes)

        if it.bow is not None:
            for name, kind, desc in (
                ("arrow_rest", SocketKind.arrow_rest,
                 "the arrow rest / shelf on the bow's riser just above the handle"),
                ("string_rest", SocketKind.string_rest,
                 "the middle of the bowstring at rest, where the arrow nocks"),
                ("limb_tip_top", SocketKind.limb_tip_top,
                 "the tip of the bow's upper limb where the string attaches"),
                ("limb_tip_bottom", SocketKind.limb_tip_bottom,
                 "the tip of the bow's lower limb where the string attaches"),
            ):
                _ensure_socket(it, name, kind, desc, notes)
            if it.weapon is None:
                it.weapon = WeaponSpec(fire_system=FireSystem.drawn_launcher,
                                       visual=ShotVisual.mesh,
                                       provenance=it.bow.provenance)
            if d.projectile(it.bow.arrow_projectile_id) is None:
                d.projectiles.append(ProjectileSpec(
                    id=it.bow.arrow_projectile_id or "arrow", kind=ProjectileKind.arrow,
                    diameter_m=0.008, length_m=0.75, speed_mps=it.bow.max_speed_mps,
                    gravity_scale=1.0, lifetime_s=8.0, on_hit=OnHit.stick,
                    needs_mesh=True,
                    visual_description="a single wooden arrow with fletching and a "
                                       "steel broadhead",
                    provenance=Provenance(rationale="a bow needs an arrow to fire")))
                it.bow.arrow_projectile_id = it.bow.arrow_projectile_id or "arrow"
                notes.append(f"{it.asset_id}: added arrow projectile")
            it.hold.aim_aligned = False

        if it.melee is not None:
            if it.melee.blunt:
                _ensure_socket(it, "strike_face", SocketKind.strike_face,
                               f"the striking face of the {label}'s head", notes)
            else:
                _ensure_socket(it, "strike_base", SocketKind.strike_base,
                               f"where the {label}'s blade meets the guard / handle",
                               notes)
                _ensure_socket(it, "strike_tip", SocketKind.strike_tip,
                               f"the point of the {label}'s blade", notes)

        _repair_interactive(it, label, notes)

    # Arrows and other mesh projectiles the player holds get an entry so
    # socket_generation finds their nock / nose.
    for proj in d.projectiles:
        if proj.kind == ProjectileKind.bullet and proj.speed_mps > VISIBLE_PROJECTILE_MAX_MPS \
                and proj.provenance.source != DecisionSource.explicit and proj.needs_mesh:
            proj.needs_mesh = False
            notes.append(f"projectile {proj.id}: a {proj.speed_mps:.0f} m/s bullet is "
                         "never seen; no mesh")
        if proj.kind in (ProjectileKind.rocket, ProjectileKind.arrow,
                         ProjectileKind.grenade) and not proj.needs_mesh:
            proj.needs_mesh = True
            notes.append(f"projectile {proj.id}: a {proj.kind.value} is visible; "
                         "it gets a mesh")

    return d, notes


_FLASH_WORDS = ("flashbang", "flash bang", "stun grenade", "flash grenade", "m84")
_SMOKE_WORDS = ("smoke grenade", "smoke bomb")
_GRENADE_WORDS = ("grenade", "flashbang", "explosive", "bomb", "dynamite", "molotov",
                  "c4", "mine")


def _repair_interactive(it: ItemInteraction, label: str, notes: list[str]) -> None:
    """Pressables press, displays show, thrown explosives explode."""
    if it.usage == ItemUsage.pressable:
        if it.press is None:
            it.press = PressSpec(provenance=Provenance(rationale="pressable default"))
            notes.append(f"{it.asset_id}: press defaulted (poke or trigger)")
        _ensure_socket(it, "press_surface", SocketKind.press_surface,
                       f"the cap / face of the {label} that a fingertip pushes in", notes)
        it.hold = HoldProfile(provenance=it.hold.provenance)
    if it.usage == ItemUsage.display:
        _ensure_socket(it, "screen", SocketKind.screen,
                       f"the centre of the {label}'s screen / face where text is shown",
                       notes)
        it.hold = HoldProfile(provenance=it.hold.provenance)
    text = _norm(f"{it.asset_id.replace('_', ' ')} {it.real_world_reference}")
    thrown = it.usage == ItemUsage.thrown or (
        it.weapon is not None and it.weapon.fire_system == FireSystem.thrown)
    if thrown and it.explosive is None and _has(_GRENADE_WORDS, text):
        kind = (ExplosiveKind.flash if any(w in text for w in _FLASH_WORDS)
                else ExplosiveKind.smoke if any(w in text for w in _SMOKE_WORDS)
                else ExplosiveKind.frag)
        it.explosive = ExplosiveSpec(
            kind=kind, damage=0.0 if kind != ExplosiveKind.frag else 120.0,
            provenance=Provenance(rationale=f"a thrown {kind.value} explosive"))
        notes.append(f"{it.asset_id}: thrown explosive defaulted ({kind.value})")
    if it.explosive is not None:
        if it.usage != ItemUsage.thrown:
            it.usage = ItemUsage.thrown
        if it.hold.kind == HoldKind.none:
            it.hold.kind = HoldKind.one_hand
        it.hold.primary_socket = _ensure_socket(
            it, it.hold.primary_socket or "grip", SocketKind.hand_grip,
            f"the body of the {label} where the palm wraps it", notes)
        it.hold.aim_aligned = False
        if it.weapon is not None and it.weapon.fire_system != FireSystem.thrown:
            it.weapon = None
        if Wielder.player not in it.wielders:
            it.wielders = [Wielder.player]


def _repair_weapon(d: InteractionDesign, it: ItemInteraction, label: str,
                   prompt_norm: str, notes: list[str]) -> None:
    w = it.weapon
    assert w is not None
    if w.fire_system == FireSystem.beam:
        if w.beam is None:
            w.beam = BeamSpec()
            notes.append(f"{it.asset_id}: beam parameters defaulted")
        w.fire_mode = FireMode.hold_beam
        w.projectile_id = ""
        return
    if w.fire_system not in _NEEDS_PROJECTILE or w.fire_system == FireSystem.drawn_launcher:
        return
    proj = d.projectile(w.projectile_id) if w.projectile_id else None
    if proj is None:
        pid = w.projectile_id or f"{it.asset_id}_round"
        energy = w.visual == ShotVisual.energy_bolt
        proj = ProjectileSpec(
            id=pid,
            kind=ProjectileKind.energy_bolt if energy else ProjectileKind.bullet,
            calibre=w.ammo.calibre,
            diameter_m=0.02 if energy else 0.009,
            length_m=0.12 if energy else 0.015,
            speed_mps=60.0 if energy else 360.0,
            gravity_scale=0.0 if energy else 1.0,
            provenance=Provenance(rationale=f"{label} fires it; defaults for "
                                            f"{'an energy bolt' if energy else 'a pistol round'}"),
        )
        d.projectiles.append(proj)
        w.projectile_id = pid
        notes.append(f"{it.asset_id}: added projectile '{pid}'")
    # Physical simulation of an invisible bullet buys nothing but cost,
    # unless the prompt asked for ballistics.
    if (w.fire_system == FireSystem.projectile and proj.kind == ProjectileKind.bullet
            and proj.speed_mps > VISIBLE_PROJECTILE_MAX_MPS
            and w.provenance.source != DecisionSource.explicit
            and not any(k in prompt_norm for k in ("bullet drop", "ballistic",
                                                   "travel time"))):
        w.fire_system = FireSystem.hybrid
        notes.append(f"{it.asset_id}: {proj.speed_mps:.0f} m/s rounds fired as hybrid "
                     "(instant hit + tracer)")
    if w.fire_system == FireSystem.hybrid and w.visual == ShotVisual.none:
        w.visual = ShotVisual.tracer


# ---------------------------------------------------------------------------
# Fallback (no agent)
# ---------------------------------------------------------------------------

_PISTOL = ("pistol", "glock", "handgun", "revolver", "blaster", "raygun",
           "ray gun", "phaser", "laser gun", "deagle", "beretta", "colt")
_RIFLE = ("rifle", "m16", "m4", "scar", "ak", "carbine", "smg", "shotgun",
          "sniper", "machine gun", "assault")
_LAUNCHER = ("rpg", "launcher", "bazooka")
_SWORD = ("sword", "katana", "blade", "saber", "sabre", "dagger", "knife",
          "machete", "longsword", "claymore")
_BLUNT = ("hammer", "mace", "club", "bat", "axe", "warhammer")
_SHIELD = ("shield", "buckler")
_BOW = ("bow", "longbow", "crossbow")
_ARROW = ("arrow",)


def _has(words: tuple[str, ...], text: str) -> bool:
    return any(re.search(rf"(^|[^a-z]){re.escape(w)}([^a-z]|$)", text) for w in words)


def fallback_design(manifest: AssetManifest, prompt: str) -> InteractionDesign:
    """A conservative design from asset names alone, for when the designer
    cannot run. Only the obvious weapon families get holds."""

    d = InteractionDesign(prompt_id=manifest.prompt_id)
    inferred = Provenance(rationale="fallback: inferred from the asset name")
    for a in manifest.items:
        if a.category != AssetCategory.prop:
            continue
        text = _norm(f"{a.id.replace('_', ' ')} {a.name}")
        it: Optional[ItemInteraction] = None
        if _has(_ARROW, text) and not _has(_BOW, text):
            continue
        if _has(_BOW, text):
            it = ItemInteraction(asset_id=a.id, usage=ItemUsage.held_weapon,
                                 wielders=[Wielder.player],
                                 hold=HoldProfile(kind=HoldKind.one_hand,
                                                  primary_socket="grip"),
                                 bow=BowSpec(arrow_projectile_id="arrow"),
                                 provenance=inferred)
        elif _has(_LAUNCHER, text):
            it = ItemInteraction(
                asset_id=a.id, usage=ItemUsage.held_weapon, wielders=[Wielder.player],
                hold=HoldProfile(kind=HoldKind.two_hand_line, aim_aligned=True,
                                 two_hand=TwoHandMode.optional,
                                 finger_pose=FingerPose.trigger),
                weapon=WeaponSpec(fire_system=FireSystem.projectile, rpm=30,
                                  damage=120, projectile_id=f"{a.id}_rocket",
                                  visual=ShotVisual.mesh,
                                  provenance=inferred),
                provenance=inferred)
            d.projectiles.append(ProjectileSpec(
                id=f"{a.id}_rocket", kind=ProjectileKind.rocket, diameter_m=0.085,
                length_m=0.9, speed_mps=120, gravity_scale=0.2, on_hit=OnHit.explode,
                splash_radius_m=3.0, splash_damage=80, needs_mesh=True,
                visual_description="a rocket-propelled grenade with fins",
                provenance=inferred))
        elif _has(_RIFLE, text):
            it = ItemInteraction(
                asset_id=a.id, usage=ItemUsage.held_weapon, wielders=[Wielder.player],
                hold=HoldProfile(kind=HoldKind.two_hand_line, aim_aligned=True,
                                 two_hand=TwoHandMode.optional,
                                 finger_pose=FingerPose.trigger),
                weapon=WeaponSpec(fire_system=FireSystem.hybrid, fire_mode=FireMode.auto,
                                  rpm=700, damage=30, projectile_id="rifle_round",
                                  tracer_every_n=3, provenance=inferred),
                provenance=inferred)
            if d.projectile("rifle_round") is None:
                d.projectiles.append(ProjectileSpec(
                    id="rifle_round", kind=ProjectileKind.bullet,
                    calibre="5.56x45mm NATO", diameter_m=0.0057, length_m=0.023,
                    speed_mps=900, provenance=inferred))
        elif _has(_PISTOL, text):
            energy = any(k in text for k in ("blaster", "ray", "phaser", "laser"))
            pid = "energy_bolt" if energy else "pistol_round"
            it = ItemInteraction(
                asset_id=a.id, usage=ItemUsage.held_weapon, wielders=[Wielder.player],
                hold=HoldProfile(kind=HoldKind.two_hand_wrap, aim_aligned=True,
                                 two_hand=TwoHandMode.optional,
                                 finger_pose=FingerPose.trigger),
                weapon=WeaponSpec(fire_system=FireSystem.hybrid, rpm=400, damage=34,
                                  projectile_id=pid,
                                  visual=ShotVisual.energy_bolt if energy
                                  else ShotVisual.tracer,
                                  impact=ImpactKind.energy if energy
                                  else ImpactKind.sparks,
                                  provenance=inferred),
                provenance=inferred)
            if d.projectile(pid) is None:
                d.projectiles.append(ProjectileSpec(
                    id=pid,
                    kind=ProjectileKind.energy_bolt if energy else ProjectileKind.bullet,
                    calibre="" if energy else "9x19mm Parabellum",
                    diameter_m=0.02 if energy else 0.009,
                    length_m=0.12 if energy else 0.015,
                    speed_mps=60 if energy else 360,
                    gravity_scale=0.0 if energy else 1.0, provenance=inferred))
        elif _has(_SHIELD, text):
            it = ItemInteraction(asset_id=a.id, usage=ItemUsage.held_shield,
                                 wielders=[Wielder.player],
                                 hold=HoldProfile(kind=HoldKind.forearm_strap),
                                 shield=ShieldSpec(provenance=inferred),
                                 provenance=inferred)
        elif _has(_SWORD, text):
            it = ItemInteraction(asset_id=a.id, usage=ItemUsage.held_weapon,
                                 wielders=[Wielder.player],
                                 hold=HoldProfile(kind=HoldKind.one_hand),
                                 melee=MeleeSpec(provenance=inferred),
                                 provenance=inferred)
        elif _has(_BLUNT, text):
            it = ItemInteraction(asset_id=a.id, usage=ItemUsage.held_weapon,
                                 wielders=[Wielder.player],
                                 hold=HoldProfile(kind=HoldKind.one_hand),
                                 melee=MeleeSpec(blunt=True, provenance=inferred),
                                 provenance=inferred)
        if it is not None:
            d.items.append(it)
    d.notes = "fallback design: the interaction designer was unavailable"
    d, _ = repair_interaction_design(d, prompt=prompt, manifest=manifest)
    return d

def downgrade_unquoted(design: InteractionDesign, prompt: str) -> InteractionDesign:
    d = design.model_copy(deep=True)
    p = _norm(prompt)
    for _where, prov in _provenances(d):
        if prov.source == DecisionSource.explicit and (
                not prov.prompt_quote or _norm(prov.prompt_quote) not in p):
            prov.source = DecisionSource.inferred
            prov.rationale = (prov.rationale or "") + " (quote not found in the prompt)"
            prov.prompt_quote = ""
    return d


def design_excerpt(design: Optional[InteractionDesign]) -> str:
    """One line per item for later planners' prompts: who holds what, how,
    and how it fires. Empty when there is no design."""

    if design is None or not design.items:
        return ""
    lines = []
    for it in design.items:
        bits = [f"{it.asset_id}: {it.usage.value}"]
        if it.wielders:
            bits.append("wielders " + "/".join(w.value for w in it.wielders))
        if it.hold.kind != HoldKind.none:
            bits.append(f"hold {it.hold.kind.value}"
                        + (" aim-aligned" if it.hold.aim_aligned else "")
                        + (f" two_hand {it.hold.two_hand.value}"
                           if it.hold.two_hand != TwoHandMode.none else ""))
        if it.sockets:
            bits.append("sockets " + ",".join(s.name for s in it.sockets))
        if it.weapon is not None:
            w = it.weapon
            bits.append(f"weapon {w.fire_system.value} {w.fire_mode.value}"
                        + (f" -> {w.projectile_id}" if w.projectile_id else ""))
        if it.melee is not None:
            bits.append("melee" + (" blunt" if it.melee.blunt else ""))
        if it.shield is not None:
            bits.append("shield")
        if it.bow is not None:
            bits.append(f"bow -> {it.bow.arrow_projectile_id}")
        lines.append("; ".join(bits))
    for pr in design.projectiles:
        lines.append(f"projectile {pr.id}: {pr.kind.value} {pr.calibre} "
                     f"{pr.speed_mps:.0f} m/s on_hit {pr.on_hit.value}"
                     + (f" mesh {pr.asset_id}" if pr.asset_id else " (no mesh)"))
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Manifest reconcile
# ---------------------------------------------------------------------------

_MANIFEST_MAX_ITEMS = 48


def _projectile_asset_id(proj: ProjectileSpec, taken: set[str]) -> str:
    base = proj.id if proj.id.endswith(("_projectile", "arrow", "_rocket")) \
        else f"{proj.id}_projectile"
    aid = base[:64]
    n = 2
    while aid in taken:
        aid = f"{base[:60]}_{n}"
        n += 1
    return aid


#: Default floor for the longest axis of a held or thrown item, metres.
VR_MIN_HANDHELD_M = 0.12


def reconcile_manifest(manifest: AssetManifest, design: InteractionDesign,
                       min_handheld_m: float = VR_MIN_HANDHELD_M,
                       ) -> tuple[AssetManifest, InteractionDesign, list[str]]:
    """Make the manifest agree with the design. Returns the new manifest, the
    design (with projectile ``asset_id`` / bow ``arrow_asset_id`` filled) and
    notes describing every change."""

    m = manifest.model_copy(deep=True)
    d = design.model_copy(deep=True)
    notes: list[str] = []
    by_id = {a.id: a for a in m.items}

    for it in d.items:
        a = by_id.get(it.asset_id)
        if a is None:
            continue
        hints = [s.name for s in it.sockets]
        if hints != list(a.socket_hints):
            a.socket_hints = hints[:8]
        dims = it.real_world_dimensions
        if dims is not None and 0.0 < dims.length_m < 30.0:
            ratio = dims.length_m / a.target_scale_meters
            if ratio > 1.15 or ratio < 1 / 1.15:
                notes.append(f"{a.id}: scale {a.target_scale_meters:.3f} m -> "
                             f"{dims.length_m:.3f} m ({it.real_world_reference or 'researched'})")
                a.target_scale_meters = round(dims.length_m, 4)
        # Smaller than a VR hand can comfortably find and close on: a real
        # 9 cm grenade reads as a pebble in the headset and slips through the
        # fingers. Held and thrown items get at least ``min_handheld_m``
        # (their real size stays in real_world_dimensions).
        if it.usage in (ItemUsage.held_weapon, ItemUsage.held_tool, ItemUsage.thrown) \
                and 0.0 < a.target_scale_meters < min_handheld_m:
            notes.append(f"{a.id}: {a.target_scale_meters:.3f} m is too small to handle in "
                         f"VR; scaled to {min_handheld_m:.2f} m")
            a.target_scale_meters = round(min_handheld_m, 4)
        if it.usage in (ItemUsage.held_weapon, ItemUsage.held_tool,
                        ItemUsage.held_shield) and a.importance is None:
            a.importance = AssetImportance.hero

    taken = set(by_id)
    for proj in d.projectiles:
        if not proj.needs_mesh:
            continue
        if proj.asset_id and proj.asset_id in by_id:
            a = by_id[proj.asset_id]
            if abs(a.target_scale_meters - proj.length_m) / proj.length_m > 0.15:
                notes.append(f"{a.id}: projectile scale -> {proj.length_m:.3f} m")
                a.target_scale_meters = round(proj.length_m, 4)
            continue
        if len(m.items) >= _MANIFEST_MAX_ITEMS:
            notes.append(f"projectile {proj.id}: manifest is full; drawn as a tracer")
            proj.needs_mesh = False
            continue
        aid = _projectile_asset_id(proj, taken)
        taken.add(aid)
        desc = proj.visual_description or f"a single {proj.calibre or proj.kind.value}"
        m.items.append(AssetItem(
            id=aid,
            name=f"{proj.kind.value.replace('_', ' ').title()} ({proj.id})",
            category=AssetCategory.prop,
            style_notes=(f"Projectile fired by a weapon: {desc}. Real size "
                         f"{proj.diameter_m * 1000:.1f} mm diameter, "
                         f"{proj.length_m * 1000:.0f} mm long."),
            character_role=CharacterRole.none,
            reference=ReferenceImageRequest(
                prompt=(f"{desc}, a single object, side view, the whole object "
                        "visible, pointing to the right, centred on a plain light "
                        "grey background, product photo lighting"),
            ),
            target_scale_meters=round(proj.length_m, 4),
            collision_shape="sphere",
            importance=AssetImportance.background,
            max_triangles=600,
            socket_hints=["nose"],
        ))
        proj.asset_id = aid
        by_id[aid] = m.items[-1]
        notes.append(f"added projectile asset '{aid}' ({proj.length_m:.3f} m)")
        if d.item(aid) is None:
            arrow = proj.kind == ProjectileKind.arrow
            sockets = [SocketSpec(name="nose", kind=SocketKind.projectile_nose,
                                  part_description=f"the front tip of the {desc}")]
            if arrow:
                sockets.append(SocketSpec(
                    name="nock", kind=SocketKind.arrow_nock,
                    part_description="the notched back end of the arrow that sits "
                                     "on the bowstring"))
            d.items.append(ItemInteraction(
                asset_id=aid,
                usage=ItemUsage.ammo if arrow else ItemUsage.projectile,
                sockets=sockets,
                provenance=Provenance(rationale=f"projectile mesh for {proj.id}"),
            ))
            m.items[-1].socket_hints = [s.name for s in sockets]

    for it in d.items:
        if it.bow is not None and not it.bow.arrow_asset_id:
            proj = d.projectile(it.bow.arrow_projectile_id)
            if proj is not None and proj.asset_id:
                it.bow.arrow_asset_id = proj.asset_id

    # Effects the engine draws itself (muzzle flashes, tracers, beams, smoke
    # trails, explosions) are procedural quads; a mesh of one is a paid image
    # and Tripo call for a blob in the scene. Dropped unless something holds,
    # wears or fires it -- and never to an empty manifest.
    gone = engine_drawn_fx_assets(m, d)
    if gone and len(gone) < len(m.items):
        m.items = [a for a in m.items if a.id not in gone]
        d.items = [it for it in d.items if it.asset_id not in gone]
        notes.append(f"dropped engine-drawn effect asset(s) {sorted(gone)}: flashes, "
                     "tracers, beams, trails and explosions are drawn by ctx.vfx")
    return m, d, notes


_FX_WORDS = re.compile(
    r"muzzle[\s_-]*flash|tracer|laser|beam|bolt|smoke[\s_-]*trail|trail|explosion|"
    r"impact|spark|blast[\s_-]*fx|flash[\s_-]*fx|shot[\s_-]*fx|projectile[\s_-]*fx",
    re.IGNORECASE)


def engine_drawn_fx_assets(manifest: AssetManifest,
                           design: Optional[InteractionDesign]) -> set[str]:
    """Manifest assets that are meshes of effects the engine draws itself and
    that nothing holds, wears or fires. Empty without a design."""
    if design is None:
        return set()
    used = {it.asset_id for it in design.items
            if it.usage not in (ItemUsage.not_interactive, ItemUsage.static_prop)}
    used |= {p.asset_id for p in design.projectiles if p.asset_id}
    return {a.id for a in manifest.items if a.id not in used and _engine_drawn_fx(a)}


def _engine_drawn_fx(a: Any) -> bool:
    """An fx asset (or an ``*_fx`` prop) depicting a shot's effect."""
    text = f"{a.id} {a.name}"
    is_fx = a.category == AssetCategory.fx or re.search(r"(?:^|_)(?:fx|vfx)(?:_|$)", a.id)
    return bool(is_fx and _FX_WORDS.search(text))
