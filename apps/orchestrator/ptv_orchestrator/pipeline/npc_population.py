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


#: What each built-in NPC behaviour fully implements. An NPC whose
#: behaviours all fall inside one of these needs no generated AI at all.
BUILTIN_BEHAVIOUR_COVERAGE: dict[str, frozenset[str]] = {
    "builtin.npc.follow": frozenset({"follow_leader", "idle"}),
    "builtin.npc.melee_chase": frozenset(
        {"chase", "melee_attack", "react_to_damage", "idle"}),
}

#: Params each built-in reads (see its source header). Anything else in a
#: plan item's config is a code-edit invention the built-in would ignore.
BUILTIN_BEHAVIOUR_PARAMS: dict[str, frozenset[str]] = {
    "builtin.npc.follow": frozenset({
        "leader_tag", "min_distance_m", "max_distance_m", "run_distance_m",
        "walk_speed_mps", "run_speed_mps", "catchup_distance_m",
        "stuck_catchup_s", "catch_up",
    }),
    "builtin.npc.melee_chase": frozenset({
        "target_tag", "sight_range_m", "chase_speed_mps", "walk_speed_mps",
        "sprint", "attack_range_m", "windup_s", "attack_cooldown_s", "damage",
        "hit_reach_m", "stagger_s", "attack_clip", "hurt_clip",
        "gait_variant_when_chasing",
    }),
}


def _npcs_driven_by(item: Any, npc_plan: NpcPlan,
                    scene_specs: Optional[list[Any]]) -> list[Any]:
    """The NpcSpecs a plan item's AI is bound to.

    From the SceneSpecs when they exist (the entity carrying the binding
    names its asset), otherwise from the asset ids the CodeTask names --
    the mechanic planner always says which character a behaviour is for."""

    by_asset = {n.asset_id: n for n in npc_plan.npcs}
    task_id = item.code_task.id if item.code_task is not None else item.id
    ids = {item.id, task_id, f"generated.{task_id}", f"generated.{item.id}"}
    found: dict[str, Any] = {}
    for spec in scene_specs or []:
        for e in getattr(spec, "entities", None) or []:
            mesh = getattr(e, "mesh", None)
            if mesh is None or mesh.asset_id not in by_asset:
                continue
            if any(getattr(b, "mechanic_id", "") in ids
                   for b in getattr(e, "mechanic_bindings", None) or []):
                found[mesh.asset_id] = by_asset[mesh.asset_id]
    if found or scene_specs:
        return list(found.values())
    if item.code_task is None:
        return []
    import re

    text = f"{item.code_task.title} {item.code_task.description}"
    for aid, npc in by_asset.items():
        if re.search(rf"\b{re.escape(aid)}\b", text):
            found[aid] = npc
    return list(found.values())


def reconcile_npc_ai_items(
    mech_plan: Any,
    npc_plan: Optional[NpcPlan],
    scene_specs: Optional[list[Any]] = None,
) -> tuple[Any, list[str]]:
    """Turn generated NPC AI that a built-in already implements into that
    built-in.

    The mechanic planner sometimes asks for a code edit of
    ``builtin.npc.follow`` just to change a parameter the built-in already
    has (``catch_up: false``). The copy then has to be written, compiled
    and kept in step with the engine forever -- and in the game this was
    found in, the copy was the one thing that never ran. A behaviour set
    the built-in fully covers is configuration, not code.

    An item converts only when EVERY NPC it drives is covered, the NPC walks
    (not flying / stationary), and the item is AI-kind. Its config carries
    over, minus keys the built-in does not read (named in the notes). Other
    CodeTasks that depended on the dropped task lose that dependency."""

    from ptv_schemas import MechanicClassification

    if npc_plan is None or not npc_plan.npcs:
        return mech_plan, []
    notes: list[str] = []
    converted: set[str] = set()
    items = []
    for it in mech_plan.items:
        task = it.code_task
        if task is None or getattr(task.kind, "value", str(task.kind)) != "ai":
            items.append(it)
            continue
        npcs = _npcs_driven_by(it, npc_plan, scene_specs)
        if not npcs or any(
                n.movement_style.value not in ("animated_locomotion",
                                               "slide_no_animation")
                for n in npcs):
            items.append(it)
            continue
        wanted = set().union(*({getattr(b, "value", str(b)) for b in n.behaviors}
                               for n in npcs))
        base = it.base_builtin_id or it.builtin_id or ""
        candidates = [base] if base in BUILTIN_BEHAVIOUR_COVERAGE else list(
            BUILTIN_BEHAVIOUR_COVERAGE)
        builtin = next((b for b in candidates
                        if wanted <= BUILTIN_BEHAVIOUR_COVERAGE[b]), None)
        if builtin is None:
            items.append(it)
            continue
        allowed = BUILTIN_BEHAVIOUR_PARAMS[builtin]
        config = {k: v for k, v in (it.config or {}).items() if k in allowed}
        dropped = sorted(k for k in (it.config or {}) if k not in allowed)
        items.append(it.model_copy(update={
            "classification": MechanicClassification.builtin_config,
            "builtin_id": builtin,
            "base_builtin_id": None,
            "code_task": None,
            "config": config,
        }))
        converted.add(task.id)
        notes.append(
            f"'{it.id}' drives {[n.asset_id for n in npcs]} whose behaviours "
            f"{sorted(wanted)} {builtin} already implements: using it as "
            "builtin+config instead of generating a copy"
            + (f" (dropped params it does not read: {dropped})" if dropped else "")
        )
    if not converted:
        return mech_plan, []
    # Nobody may wait on a task that no longer exists.
    final = []
    for it in items:
        if it.code_task is not None and set(it.code_task.depends_on) & converted:
            task = it.code_task.model_copy(update={
                "depends_on": [d for d in it.code_task.depends_on
                               if d not in converted]})
            it = it.model_copy(update={"code_task": task})
        final.append(it)
    return mech_plan.model_copy(update={"items": final}), notes


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
