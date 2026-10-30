"""Map the game's weapons onto the engine's weapon built-ins.

Once a project has an InteractionDesign, firing, ammo, reloading, recoil,
projectiles, beams, swings, blocking and drawing a bow are the engine's
(builtin.weapon, builtin.bow, builtin.melee_weapon, builtin.shield,
builtin.item_source, builtin.npc.ranged_attack). A mechanic plan that still
asks for a generated ``shooting_hub`` + ``pistol_fire`` + ``pistol_ammo_reload``
builds a second weapon that fights the first -- and, as the humans-vs-aliens
pistol showed, one that can silently never hit anything.

So, deterministically and with every change named:

* every weapon / bow / melee item / shield in the design gets a
  ``builtin+config`` plan item of its built-in, its tuning taken from the
  design (``weapon_params`` and friends are what the scene compiler binds);
* generated items that ARE the firing of a covered weapon (a shooting hub, a
  fire profile, ammo / reload, recoil, projectile / tracer, hit detection)
  and ``builtin.recoil`` items are removed, named;
* items that depended on a removed task lose the dependency and are told to
  read hits and ammo through ``ctx.weapons`` instead;
* an NPC whose plan has ``ranged_attack`` gets ``builtin.npc.ranged_attack``.
"""

from __future__ import annotations

import re
from typing import Any, Optional

from ptv_schemas import (
    FireSystem,
    InteractionDesign,
    ItemInteraction,
    ProjectileSpec,
)

WEAPON_ID = "builtin.weapon"
BOW_ID = "builtin.bow"
MELEE_ID = "builtin.melee_weapon"
SHIELD_ID = "builtin.shield"
SOURCE_ID = "builtin.item_source"
RANGED_NPC_ID = "builtin.npc.ranged_attack"
ENGINE_WEAPON_IDS = {WEAPON_ID, BOW_ID, MELEE_ID, SHIELD_ID, SOURCE_ID, RANGED_NPC_ID}

# A generated item whose job is firing / swinging a weapon the engine now runs.
_FIRING = re.compile(
    r"shoot(ing)?[\s_-]*hub|weapon[\s_-]*hub|_fire\b|fire[\s_-]*(profile|simulation|logic|"
    r"system|control)|\bfire\b|firing|shoot\b|shooting\b|ammo|reload|magazine|recoil|"
    r"projectile|bullet|hitscan|raycast[\s_-]*(fire|shot)|tracer|muzzle[\s_-]*flash|"
    r"laser[\s_-]*(fire|shot|beam)|beam[\s_-]*(fire|weapon)|blaster[\s_-]*fire|"
    r"sword[\s_-]*(swing|strike|hit|combat)|melee[\s_-]*(hit|strike|weapon|swing)|"
    r"shield[\s_-]*block|bow[\s_-]*(draw|shoot|fire)|arrow[\s_-]*(launch|shoot|fire)|"
    r"weapon[\s_-]*(fire|recoil|ammo|damage)",
    re.IGNORECASE,
)
# ... unless it is clearly something else that only mentions a weapon.
_NOT_FIRING = re.compile(
    r"\bhud\b|score|target[\s_-]*(hit|react|board|range)|enemy|npc|boss|wave|"
    r"pickup|pick[\s_-]*up|holster|rack|tutorial|dialog",
    re.IGNORECASE,
)

_NOTE_MARK = "no longer exist -- weapons are engine built-ins"
_DEP_NOTE = (
    " ENGINE NOTE: {gone} " + _NOTE_MARK + " "
    "(builtin.weapon / bow / melee_weapon / shield). Do not include their "
    "headers. Read shots, hits and ammo through ctx.weapons (hits_since(), "
    "hit_seq(), state(), held_weapon()) and projectile hits through "
    "ctx.projectiles->hits_since(); never fire, raycast from a muzzle, keep "
    "ammo or apply weapon damage yourself."
)


def _rgb(v: list[float]) -> list[float]:
    return [float(x) for x in (v or [1.0, 1.0, 1.0])[:3]]


def projectile_params(p: Optional[ProjectileSpec]) -> dict[str, Any]:
    if p is None:
        return {}
    return {
        "speed_mps": p.speed_mps, "gravity_scale": p.gravity_scale, "drag": p.drag,
        "radius_m": p.radius_m, "lifetime_s": p.lifetime_s, "on_hit": p.on_hit.value,
        "splash_radius_m": p.splash_radius_m, "splash_damage": p.splash_damage,
    }


_BODY_SLOTS = {"back", "shoulder_r", "shoulder_l", "hip_r", "hip_l", "waist", "chest"}


def body_slot(name: str) -> str:
    """builtin.item_source's slot vocabulary for whatever the design wrote
    ("right_shoulder", "Right hip", "belt"); unknown -> back."""
    n = re.sub(r"[\s-]+", "_", (name or "").strip().lower())
    if n in _BODY_SLOTS:
        return n
    side = "_r" if "right" in n or n.endswith("_r") else "_l" if "left" in n or n.endswith("_l") else ""
    if "shoulder" in n:
        return "shoulder" + (side or "_r")
    if "hip" in n or "thigh" in n or "holster" in n:
        return "hip" + (side or "_r")
    if "belt" in n or "waist" in n or "pouch" in n:
        return "waist"
    if "chest" in n or "vest" in n:
        return "chest"
    return "back"


def socket_name(item: ItemInteraction, kind: str, default: str) -> str:
    """The design's name for the socket of ``kind`` (``muzzle`` -> "muzzle")."""
    s = next((s for s in item.sockets if s.kind.value == kind), None)
    return s.name if s is not None else default


def weapon_params(item: ItemInteraction, design: InteractionDesign) -> dict[str, Any]:
    """builtin.weapon params for one item (the scene compiler adds the
    projectile's ``visual_template`` entity id)."""
    w = item.weapon
    assert w is not None
    proj = design.projectile(w.projectile_id) if w.projectile_id else None
    delivery = w.fire_system.value
    out: dict[str, Any] = {
        "delivery": delivery, "fire_mode": w.fire_mode.value, "rpm": w.rpm,
        "burst_count": w.burst_count, "pellets": w.pellets, "spread_deg": w.spread_deg,
        "range_m": w.range_m, "damage": w.damage, "falloff_at_range": w.falloff_at_range,
        "ammo": {
            "infinite": w.ammo.infinite, "magazine": w.ammo.magazine,
            "reserve": w.ammo.reserve, "reload": w.ammo.reload.value,
            "reload_time_s": w.ammo.reload_time_s, "heat_per_shot": w.ammo.heat_per_shot,
            "heat_vent_per_s": w.ammo.heat_vent_per_s,
        },
        "visual": w.visual.value, "tracer_every_n": _tracer_every_n(w),
        "color_rgb": _rgb(w.color_rgb), "muzzle_flash": w.muzzle_flash,
        "impact": w.impact.value,
        "recoil": {"kick_back_m": w.recoil.kick_back_m, "kick_up_deg": w.recoil.kick_up_deg,
                   "recover_s": w.recoil.recover_s},
        "haptic_amplitude": w.haptic_amplitude,
        "two_hand_required": item.hold.two_hand.value == "required",
        "flash_scale": _flash_scale(item, proj),
        "muzzle_socket": socket_name(item, "muzzle", "muzzle"),
    }
    if proj is not None:
        out["projectile"] = projectile_params(proj)
        if delivery == FireSystem.hybrid.value:
            # The tracer is a visual: slower than the round so it can be seen.
            out["tracer_speed_mps"] = float(min(proj.speed_mps, 260.0))
        out["tracer_width_m"] = round(max(0.004, min(0.03, proj.diameter_m * 1.2)), 4)
    if w.beam is not None:
        out["beam"] = {"damage_per_s": w.beam.damage_per_s, "width_m": w.beam.width_m,
                       "max_length_m": w.beam.max_length_m,
                       "color_rgb": _rgb(w.beam.color_rgb)}
    return out


def _tracer_every_n(w: Any) -> int:
    """Automatic fire draws a tracer every 3rd round unless the design says
    otherwise: at 900 rpm one per round is a solid bar, and FX quads."""
    if (w.fire_mode.value == "auto" and w.tracer_every_n == 1
            and w.visual.value == "tracer" and w.provenance.source.value != "explicit"):
        return 3
    return w.tracer_every_n


def _flash_scale(item: ItemInteraction, proj: Optional[ProjectileSpec]) -> float:
    if proj is not None and proj.kind.value == "rocket":
        return 3.0
    dims = item.real_world_dimensions
    if dims is not None and dims.length_m > 0.6:
        return 1.8
    return 1.0


def bow_params(item: ItemInteraction, design: InteractionDesign) -> dict[str, Any]:
    b = item.bow
    assert b is not None
    proj = design.projectile(b.arrow_projectile_id)
    out: dict[str, Any] = {
        "max_draw_m": b.max_draw_m, "max_speed_mps": b.max_speed_mps, "damage": b.damage,
        "arrow_asset_id": b.arrow_asset_id, "allow_empty_draw": True,
    }
    if proj is not None:
        out["projectile"] = projectile_params(proj)
    return out


def melee_params(item: ItemInteraction) -> dict[str, Any]:
    m = item.melee
    assert m is not None
    return {"damage": m.damage, "min_speed_mps": m.min_speed_mps,
            "speed_scale": m.speed_scale, "cooldown_s": m.cooldown_s, "blunt": m.blunt,
            "impact": m.impact.value,
            "strike_base_socket": socket_name(item, "strike_base", "strike_base"),
            "strike_tip_socket": socket_name(item, "strike_tip", "strike_tip"),
            "strike_face_socket": socket_name(item, "strike_face", "strike_face")}


def shield_params(item: ItemInteraction) -> dict[str, Any]:
    s = item.shield
    assert s is not None
    return {"blocks_melee": s.blocks_melee, "leak": s.leak}


def engine_bindings(item: ItemInteraction, design: InteractionDesign) -> list[tuple[str, dict]]:
    """(builtin id, params) for every weapon built-in this item needs."""
    out: list[tuple[str, dict]] = []
    if item.bow is not None:
        out.append((BOW_ID, bow_params(item, design)))
    elif item.weapon is not None and item.weapon.fire_system in (
            FireSystem.hitscan, FireSystem.projectile, FireSystem.hybrid, FireSystem.beam):
        out.append((WEAPON_ID, weapon_params(item, design)))
    if item.melee is not None:
        out.append((MELEE_ID, melee_params(item)))
    if item.shield is not None:
        out.append((SHIELD_ID, shield_params(item)))
    return out


def _authored(task: Any) -> str:
    """A task's title + description as the planner wrote them: without the
    ENGINE NOTEs reconciles append and without ``builtin.*`` ids, neither of
    which names a task."""
    desc = task.description.split(" ENGINE NOTE:", 1)[0]
    return re.sub(r"builtin\.[a-z0-9_.]+", " ", f"{task.title} {desc}")


def _name_of(item: Any) -> str:
    task = item.code_task
    title = getattr(task, "title", "") if task is not None else ""
    desc = getattr(task, "description", "") if task is not None else ""
    return f"{item.id} {title} {desc[:300]}"


def reconcile_weapon_items(mech_plan: Any, design: Optional[InteractionDesign],
                           npc_plan: Any = None) -> tuple[Any, list[str]]:
    """Return ``(plan, notes)``; a no-op without an InteractionDesign."""
    from ptv_schemas import ControllerBinding, MechanicClassification, MechanicPlanItem

    if design is None:
        return mech_plan, []
    notes: list[str] = []
    covered = [i for i in design.items if engine_bindings(i, design)]
    removed: dict[str, str] = {}
    items = []
    for it in mech_plan.items:
        if it.builtin_id == "builtin.recoil" and covered:
            notes.append(f"'{it.id}' removed: builtin.weapon includes recoil")
            continue
        if it.builtin_id in ENGINE_WEAPON_IDS:
            continue  # re-added below from the design
        task = it.code_task
        if (task is not None and covered and it.base_builtin_id == "builtin.grab"
                and any(re.search(rf"\b{re.escape(c.asset_id)}\b", _authored(task))
                        for c in covered)):
            # A copy of grab that picks up a designed weapon predates the
            # socket hold solver (two hands, aim alignment, scripted holds)
            # and would hold the weapon wrongly; the built-in holds it.
            removed[task.id] = it.id
            items.append(it.model_copy(update={
                "classification": MechanicClassification.builtin_config,
                "builtin_id": "builtin.grab", "base_builtin_id": None,
                "code_task": None, "config": {}}))
            notes.append(f"'{it.id}' is a copy of builtin.grab holding a designed weapon: "
                         "using builtin.grab (socket holds) instead")
            continue
        if task is not None and covered:
            name = _name_of(it)
            if _FIRING.search(name) and not _NOT_FIRING.search(f"{it.id} "
                                                               f"{getattr(task, 'title', '')}"):
                removed[task.id] = it.id
                notes.append(f"'{it.id}' removed: firing / swinging / ammo for "
                             f"{[c.asset_id for c in covered]} is the engine's "
                             "(builtin.weapon / bow / melee_weapon / shield)")
                continue
        items.append(it)

    for item in covered:
        for builtin_id, params in engine_bindings(item, design):
            short = builtin_id.split(".")[-1]
            pid = f"{short}_{item.asset_id}"[:64]
            binding = None
            if builtin_id in (WEAPON_ID, BOW_ID):
                binding = ControllerBinding.model_validate(
                    {"control": "trigger" if builtin_id == WEAPON_ID else "grip",
                     "hand": "either"})
            items.append(MechanicPlanItem(
                id=pid, classification=MechanicClassification.builtin_config,
                builtin_id=builtin_id, config=params, binding=binding))
            notes.append(f"'{pid}': {builtin_id} for {item.asset_id}")
    for item in design.items:
        src, drawn = item.item_source, item.asset_id
        if item.bow is not None and item.bow.arrow_asset_id:
            src, drawn = item.bow.arrow_source, item.bow.arrow_asset_id
        if src is not None:
            pid = f"item_source_{drawn}"[:64]
            items.append(MechanicPlanItem(
                id=pid, classification=MechanicClassification.builtin_config,
                builtin_id=SOURCE_ID,
                config={"body_slot": body_slot(src.body_slot),
                        "radius_m": src.radius_m, "count": src.count}))
            notes.append(f"'{pid}': {SOURCE_ID} for {drawn}")
    if npc_plan is not None:
        for npc in getattr(npc_plan, "npcs", []) or []:
            behaviours = [getattr(b, "value", str(b)) for b in (npc.behaviors or [])]
            if "ranged_attack" in behaviours:
                pid = f"ranged_attack_{npc.asset_id}"[:64]
                items.append(MechanicPlanItem(
                    id=pid, classification=MechanicClassification.builtin_config,
                    builtin_id=RANGED_NPC_ID, config={}))
                notes.append(f"'{pid}': {RANGED_NPC_ID} for NPC {npc.asset_id}")

    if covered:
        # A plan reconciled on an earlier run no longer has the removed
        # tasks, but descriptions still name them: treat a firing-looking
        # task id the plan does not have as removed too.
        present = {it.id for it in items} | {it.code_task.id for it in items if it.code_task}
        for it in items:
            if it.code_task is None:
                continue
            for tok in set(re.findall(r"\b[a-z][a-z0-9]*(?:_[a-z0-9]+)+\b", _authored(it.code_task))):
                if tok not in present and _FIRING.search(tok.replace("_", " ") + " " + tok):
                    removed.setdefault(tok, tok)
    if removed:
        final = []
        for it in items:
            task = it.code_task
            if task is not None:
                # Depending on a removed task, or just naming it ("show the
                # pistol_ammo_reload state") -- either way it needs telling.
                text = _authored(task)
                gone = sorted(set(task.depends_on) & set(removed)
                              | {r for r in removed if re.search(rf"\b{re.escape(r)}\b", text)})
                deps = [d for d in task.depends_on if d not in removed]
                told = _NOTE_MARK in task.description
                if gone and (not told or deps != task.depends_on):
                    task = task.model_copy(update={
                        "depends_on": deps,
                        "description": task.description if told else
                        (task.description + _DEP_NOTE.format(gone=gone))[:10_000],
                    })
                    it = it.model_copy(update={"code_task": task})
            final.append(it)
        items = final
    if not items:
        return mech_plan, notes
    # Idempotent: a plan already reconciled comes back unchanged and silent.
    if [i.model_dump(mode="json") for i in items] == \
            [i.model_dump(mode="json") for i in mech_plan.items]:
        return mech_plan, []
    return mech_plan.model_copy(update={"items": items}), notes
