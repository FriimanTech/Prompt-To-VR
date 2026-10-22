"""Checks and repairs for the NPC plan's population: who is placed, who
arrives in waves, and how many are alive at once.

The NPC director (an LLM) decides the population; these rules make sure the
decision is one the engine can run:

* every NPC type the BuildSpec roster planned has an NpcSpec;
* a singular character (a companion) is placed once and never spawned in
  waves;
* waves reference only NPC types that exist;
* enemies that "arrive in waves" have an encounter plan -- and when the
  director left it out, a sensible default progression is built from the
  roster's variants rather than shipping a game whose enemies never come;
* the alive cap fits the animated-agent budget.
"""

from __future__ import annotations

from typing import Any, Optional

from ptv_schemas import (
    BuildSpec,
    EncounterGroup,
    EncounterPlan,
    EncounterStart,
    EncounterWave,
    NpcPlan,
)

#: Animated characters alive at once, across the whole scene, that a Quest
#: comfortably affords (the NPC director is told the same number).
DEFAULT_ANIMATED_BUDGET = 12

_SPAWNABLE_ROLES = ("enemy", "boss", "critter", "neutral")
#: Default wave progression order of variant physiques: the easy body first,
#: the strongest last.
_PHYSIQUE_ORDER = ("small_fast", "standard", "tall_lean", "custom", "elite",
                   "big_bulky", "boss")


def _singular_assets(spec: Optional[BuildSpec]) -> set[str]:
    if spec is None:
        return set()
    return {
        v.asset_id
        for fam in spec.npc_roster
        if fam.count_reading.value == "singular"
        for v in fam.variants
    }


def npc_plan_violations(plan: NpcPlan, spec: Optional[BuildSpec]) -> list[str]:
    """Problems worth sending back to the NPC director, in its terms."""

    problems: list[str] = []
    by_asset = {n.asset_id: n for n in plan.npcs}
    if spec is not None:
        for fam in spec.npc_roster:
            for v in fam.variants:
                if v.asset_id not in by_asset:
                    problems.append(
                        f"roster variant '{v.asset_id}' ({fam.family_id}) has no "
                        "NpcSpec; every variant needs its own stats and behaviours"
                    )
    singular = _singular_assets(spec)
    if plan.encounters is not None:
        for wi, w in enumerate(plan.encounters.waves):
            for g in w.groups:
                npc = by_asset.get(g.asset_id)
                if npc is None:
                    problems.append(
                        f"wave {wi + 1} spawns '{g.asset_id}', which has no NpcSpec"
                    )
                elif npc.role.value == "ally" or g.asset_id in singular:
                    problems.append(
                        f"wave {wi + 1} spawns '{g.asset_id}', a singular/companion "
                        "character; place it with persistent_count instead"
                    )
    wave_enemies = [
        n.asset_id for n in plan.npcs
        if n.spawn.waves and n.role.value in _SPAWNABLE_ROLES
    ]
    if wave_enemies and plan.encounters is None:
        problems.append(
            f"{wave_enemies} arrive in waves but there is no encounters plan"
        )
    return problems


def default_encounters(plan: NpcPlan, spec: Optional[BuildSpec]) -> Optional[EncounterPlan]:
    """A sensible progression when the director left one out: the easiest
    variant first, then mixes, then the strongest last."""

    physique: dict[str, str] = {}
    if spec is not None:
        for fam in spec.npc_roster:
            for v in fam.variants:
                physique[v.asset_id] = v.physique.value
    singular = _singular_assets(spec)
    types = [
        n for n in plan.npcs
        if n.role.value in _SPAWNABLE_ROLES and n.asset_id not in singular
        and n.persistent_count == 0
    ]
    if not types:
        return None
    types.sort(key=lambda n: _PHYSIQUE_ORDER.index(physique.get(n.asset_id, "standard"))
               if physique.get(n.asset_id, "standard") in _PHYSIQUE_ORDER else 3)
    base = max(2, min(6, types[0].spawn.initial_count or 3))
    waves: list[EncounterWave] = []
    if len(types) == 1:
        # One type: three waves that grow.
        for k in range(3):
            waves.append(EncounterWave(groups=[
                EncounterGroup(asset_id=types[0].asset_id, count=base + 2 * k)]))
    else:
        # One wave introducing each type in turn, mixed with what came
        # before, then a finale of the strongest.
        for k in range(len(types)):
            groups = [EncounterGroup(asset_id=types[0].asset_id, count=base + k)]
            if k > 0:
                groups.append(EncounterGroup(asset_id=types[k].asset_id,
                                             count=max(1, base - 1)))
            waves.append(EncounterWave(groups=groups))
        waves.append(EncounterWave(groups=[
            EncounterGroup(asset_id=types[-1].asset_id, count=max(1, base // 2 + 1))
        ]))
    cap = max(n.spawn.max_concurrent for n in types) or 6
    # A short delay so the player has a moment to take in the level.
    return EncounterPlan(waves=waves, max_concurrent=max(1, min(cap, 8)),
                         start=EncounterStart.delay, start_delay_s=5.0,
                         notes="default progression (the plan named none)")


#: Built-in NPC mechanics the population can need, with the plan item id
#: the reconcile adds them under.
_NPC_BUILTINS = {
    "builtin.encounter_director": "encounter_director",
    "builtin.npc.follow": "npc_follow",
    "builtin.npc.melee_chase": "npc_melee_chase",
}


def reconcile_npc_mechanics(mech_plan: Any, npc_plan: Optional[NpcPlan]) -> tuple[Any, list[str]]:
    """Make sure the mechanic plan carries the built-ins the NPC plan needs.

    The encounter director whenever NPCs arrive during play; the follow and
    melee-chase behaviours whenever an NPC type needs them. Per-type numbers
    are filled in by the scene compiler from the NPC plan, so the plan items
    carry no config. A game whose planner already generated its own AI for
    an NPC keeps it -- the compiler binds the built-in only to NPCs without
    one."""

    from ptv_schemas import MechanicClassification, MechanicPlanItem

    if npc_plan is None or not npc_plan.npcs:
        return mech_plan, []
    wanted: list[str] = []
    if npc_plan.encounters is not None:
        wanted.append("builtin.encounter_director")
    for n in npc_plan.npcs:
        beh = {getattr(b, "value", str(b)) for b in n.behaviors or []}
        if "follow_leader" in beh and not beh & {"chase", "melee_attack"}:
            wanted.append("builtin.npc.follow")
        elif beh & {"chase", "melee_attack"}:
            wanted.append("builtin.npc.melee_chase")
    have = {it.builtin_id for it in mech_plan.items if it.builtin_id}
    ids = {it.id for it in mech_plan.items}
    added: list[str] = []
    items = list(mech_plan.items)
    for builtin_id in dict.fromkeys(wanted):
        if builtin_id in have:
            continue
        item_id = _NPC_BUILTINS[builtin_id]
        while item_id in ids:
            item_id += "_builtin"
        items.append(MechanicPlanItem(
            id=item_id,
            classification=MechanicClassification.builtin,
            builtin_id=builtin_id,
        ))
        ids.add(item_id)
        added.append(builtin_id)
    if not added:
        return mech_plan, []
    return mech_plan.model_copy(update={"items": items}), [
        f"added {b} for the NPC plan" for b in added]


def repair_npc_plan(
    plan: NpcPlan,
    spec: Optional[BuildSpec],
    animated_budget: int = DEFAULT_ANIMATED_BUDGET,
) -> tuple[NpcPlan, list[str]]:
    """Deterministic fixes the engine can make without the director."""

    notes: list[str] = []
    singular = _singular_assets(spec)

    if plan.encounters is not None:
        by_asset = {n.asset_id: n for n in plan.npcs}
        waves = []
        for w in plan.encounters.waves:
            groups = [g for g in w.groups
                      if g.asset_id in by_asset
                      and by_asset[g.asset_id].role.value != "ally"
                      and g.asset_id not in singular]
            if groups:
                waves.append(w.model_copy(update={"groups": groups}))
        if len(waves) != len(plan.encounters.waves) or any(
                len(a.groups) != len(b.groups)
                for a, b in zip(waves, plan.encounters.waves)):
            notes.append("dropped wave groups naming unknown or companion NPCs")
        plan = plan.model_copy(update={
            "encounters": plan.encounters.model_copy(update={"waves": waves})
            if waves else None
        })

    # Singular characters (a companion) and allies nothing spawns are placed
    # once at load; otherwise they would never appear at all.
    spawned = set(plan.spawned_asset_ids())
    npcs = []
    for n in plan.npcs:
        placeable = n.asset_id in singular or n.role.value == "ally"
        if placeable and n.persistent_count == 0 and n.asset_id not in spawned:
            n = n.model_copy(update={"persistent_count": 1})
            notes.append(f"'{n.asset_id}' is placed once at load")
        npcs.append(n)
    plan = plan.model_copy(update={"npcs": npcs})

    wave_enemies = [n for n in plan.npcs if n.spawn.waves]
    if plan.encounters is None and wave_enemies:
        enc = default_encounters(plan, spec)
        if enc is not None:
            plan = plan.model_copy(update={"encounters": enc})
            notes.append(f"built a default {len(enc.waves)}-wave progression")

    if plan.encounters is not None:
        persistent = sum(n.persistent_count for n in plan.npcs)
        room = max(1, animated_budget - persistent)
        if plan.encounters.max_concurrent > room:
            plan = plan.model_copy(update={"encounters": plan.encounters.model_copy(
                update={"max_concurrent": room})})
            notes.append(f"alive cap lowered to {room} to fit the animated budget")
    return plan, notes
