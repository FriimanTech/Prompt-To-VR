"""Weapons run on engine built-ins: the mechanic plan loses its generated
shooting hub / fire profiles, the scene compiler binds builtin.weapon / bow /
melee_weapon / shield from the interaction design, emits projectile and arrow
templates, arms ranged NPCs, and the codegen contract rejects a second gun."""

from __future__ import annotations

from ptv_schemas import (
    AssetManifest,
    FireMode,
    MechanicPlan,
    NpcPlan,
    TargetDevice,
)
from ptv_schemas.performance import derive_profile

from ptv_orchestrator.pipeline.codegen_contracts import lint_task_sources
from ptv_orchestrator.pipeline.interaction_rules import fallback_design, reconcile_manifest
from ptv_orchestrator.pipeline.projectile_scale import projectile_scale_corrections
from ptv_orchestrator.pipeline.weapon_plan import (
    BOW_ID,
    MELEE_ID,
    RANGED_NPC_ID,
    SHIELD_ID,
    SOURCE_ID,
    WEAPON_ID,
    reconcile_weapon_items,
    weapon_params,
)
from ptv_orchestrator.pipeline.weapon_scene import (
    apply_weapons,
    arrow_template_id,
    proj_template_id,
    weapon_violations,
)

PROMPT = ("Survive waves of aliens with a Glock pistol and an M16. I also carry a "
          "round shield, a sword, a bow with arrows and an RPG.")


def _item(aid: str, name: str, category: str = "prop", scale: float = 0.3) -> dict:
    return {"id": aid, "name": name, "category": category, "style_notes": name,
            "reference": {"prompt": name}, "target_scale_meters": scale}


def _design():
    m = AssetManifest.model_validate({"prompt_id": "p", "items": [
        _item("glock_pistol", "Glock pistol"),
        _item("m16_rifle", "M16 rifle", scale=1.0),
        _item("round_shield", "Round shield", scale=0.7),
        _item("knight_sword", "Knight sword", scale=1.0),
        _item("hunting_bow", "Hunting bow", scale=1.2),
        _item("rpg_launcher", "RPG launcher", scale=1.0),
        _item("alien", "Alien", category="character", scale=1.8),
    ]})
    _m2, d, _ = reconcile_manifest(m, fallback_design(m, PROMPT))
    return d


def _task(tid: str, title: str, deps: list[str] | None = None) -> dict:
    return {"id": tid, "classification": "generate", "code_task": {
        "id": tid, "kind": "mechanic", "title": title, "description": title,
        "interface": "IMechanic", "depends_on": deps or []}}


def _plan() -> MechanicPlan:
    pickup = _task("pistol_pickup", "Either-hand glock_pistol grab and drop")
    pickup.update(classification="builtin+config_code_edit", base_builtin_id="builtin.grab")
    return MechanicPlan.model_validate({"prompt_id": "p", "items": [
        pickup,
        _task("shooting_hub", "Shooting hub"),
        _task("pistol_fire", "Pistol fire profile", ["shooting_hub"]),
        _task("pistol_ammo_reload", "Pistol ammo and reload", ["shooting_hub"]),
        _task("target_scoring", "Target hit reaction and score", ["shooting_hub"]),
        {"id": "recoil", "classification": "builtin", "builtin_id": "builtin.recoil"},
    ]})


def _npc_plan() -> NpcPlan:
    return NpcPlan.model_validate({"prompt_id": "p", "npcs": [{
        "asset_id": "alien", "role": "enemy", "faction": "enemy",
        "hostile_to": ["player"], "behaviors": ["chase", "ranged_attack"],
        "combat": {"attack_range_m": 15.0, "damage": 8.0}}]})


def test_plan_drops_the_private_gun_and_adds_the_built_ins() -> None:
    d = _design()
    plan, notes = reconcile_weapon_items(_plan(), d, _npc_plan())
    ids = {it.id: it for it in plan.items}
    assert "shooting_hub" not in ids and "pistol_fire" not in ids
    assert "pistol_ammo_reload" not in ids and "recoil" not in ids
    # A copy of grab holding the pistol becomes the built-in (socket holds).
    assert ids["pistol_pickup"].builtin_id == "builtin.grab"
    assert ids["pistol_pickup"].code_task is None
    # The score target stays, loses its dependency and is told about ctx.weapons.
    scoring = ids["target_scoring"]
    assert scoring.code_task.depends_on == []
    assert "ctx.weapons" in scoring.code_task.description
    builtins = {it.builtin_id for it in plan.items if it.builtin_id}
    assert {WEAPON_ID, BOW_ID, MELEE_ID, SHIELD_ID, SOURCE_ID, RANGED_NPC_ID} <= builtins
    assert notes
    # Idempotent.
    again, notes2 = reconcile_weapon_items(plan, d, _npc_plan())
    assert notes2 == [] and again is plan


def test_no_design_is_a_no_op() -> None:
    plan = _plan()
    out, notes = reconcile_weapon_items(plan, None)
    assert out is plan and notes == []


def test_weapon_params_come_from_the_design() -> None:
    d = _design()
    p = weapon_params(d.item("glock_pistol"), d)
    w = d.item("glock_pistol").weapon
    assert p["rpm"] == w.rpm and p["damage"] == w.damage
    assert p["muzzle_socket"] == next(s.name for s in d.item("glock_pistol").sockets
                                      if s.kind.value == "muzzle")
    assert p["ammo"]["magazine"] == w.ammo.magazine


def _mesh(asset_id: str, sockets: list[str]) -> dict:
    return {"asset_id": asset_id, "glb_path": f"{asset_id}.glb", "target_scale_meters": 1.0,
            "sockets": {s: {"position": [0, 0, 0], "rotation": [0, 0, 0, 1]} for s in sockets}}


def _sockets_of(d, aid: str) -> list[str]:
    return [s.name for s in d.item(aid).sockets]


def _doc(d) -> dict:
    ents = []
    for aid in ("glock_pistol", "m16_rifle", "round_shield", "knight_sword", "hunting_bow",
                "rpg_launcher"):
        ents.append({"id": aid, "mesh": _mesh(aid, _sockets_of(d, aid)), "tags": [],
                     "mechanic_bindings": [{"id": "generated.pistol_fire", "params": {}},
                                           {"id": "builtin.recoil", "params": {}}],
                     "transform": {"position": [0, 1, 0], "rotation": [0, 0, 0, 1],
                                   "scale": 1.0}})
    ents.append({"id": "alien_template", "mesh": _mesh("alien", []), "tags": ["template", "enemy"],
                 "mechanic_bindings": [{"id": "builtin.npc.melee_chase", "params": {}}]})
    ents.append({"id": "ptv_game_director", "mesh": None, "tags": [], "mechanic_bindings": []})
    return {"entities": ents}


def _mesh_for(d):
    def f(aid: str):
        item = d.item(aid)
        return _mesh(aid, [s.name for s in item.sockets] if item else [])
    return f


def test_scene_binds_weapons_templates_sources_and_ranged_npcs() -> None:
    d = _design()
    doc = _doc(d)
    summary = apply_weapons(doc, d, known_generated={"target_scoring"},
                            mesh_for=_mesh_for(d), npc_plan=_npc_plan())
    by_id = {e["id"]: e for e in doc["entities"]}
    pistol = by_id["glock_pistol"]
    ids = [b["id"] for b in pistol["mechanic_bindings"]]
    assert ids == [WEAPON_ID, "builtin.grab"]
    assert "grabbable" in pistol["tags"] and pistol["physics"]
    assert [b["id"] for b in by_id["knight_sword"]["mechanic_bindings"]] == [MELEE_ID,
                                                                          "builtin.grab"]
    assert [b["id"] for b in by_id["round_shield"]["mechanic_bindings"]] == [SHIELD_ID,
                                                                          "builtin.grab"]
    bow = by_id["hunting_bow"]
    bow_b = next(b for b in bow["mechanic_bindings"] if b["id"] == BOW_ID)
    arrow_aid = d.item("hunting_bow").bow.arrow_asset_id
    assert bow_b["params"]["arrow_template"] == arrow_template_id(arrow_aid)
    arrow = by_id[arrow_template_id(arrow_aid)]
    assert {"template", "arrow", "grabbable"} <= set(arrow["tags"])
    source = next(e for e in doc["entities"]
                  if any(b["id"] == SOURCE_ID for b in e["mechanic_bindings"]))
    assert "template" not in source["tags"]
    assert source["mechanic_bindings"][0]["params"]["template"] == arrow["id"]
    # The rocket draws with its own mesh template.
    rocket = next(p for p in d.projectiles if p.kind.value == "rocket")
    rpg_b = next(b for b in by_id["rpg_launcher"]["mechanic_bindings"] if b["id"] == WEAPON_ID)
    assert rpg_b["params"]["projectile"]["visual_template"] == proj_template_id(rocket.id)
    assert "template" in by_id[proj_template_id(rocket.id)]["tags"]
    # The alien shoots (melee_chase replaced). The design gives NPCs no gun
    # and its mesh has no muzzle, so it fires from its own body.
    alien = by_id["alien_template"]
    assert [b["id"] for b in alien["mechanic_bindings"]] == [RANGED_NPC_ID, WEAPON_ID]
    assert summary["npc_ranged"]
    assert weapon_violations(doc, d)[0] == []
    # Idempotent.
    n = len(doc["entities"])
    apply_weapons(doc, d, known_generated={"target_scoring"}, mesh_for=_mesh_for(d),
                  npc_plan=_npc_plan())
    assert len(doc["entities"]) == n


def test_scene_spec_item_sources_are_replaced_by_the_passes_own() -> None:
    d = _design()
    doc = _doc(d)
    doc["entities"].append({"id": "quiver_source", "mesh": None, "tags": [],
                            "mechanic_bindings": [{"id": SOURCE_ID, "builtin_id": SOURCE_ID,
                                                   "params": {"body_slot": "back"}}]})
    apply_weapons(doc, d, known_generated=set(), mesh_for=_mesh_for(d))
    by_id = {e["id"]: e for e in doc["entities"]}
    assert by_id["quiver_source"]["mechanic_bindings"] == []
    assert weapon_violations(doc, d)[0] == []


def test_violations_catch_a_gun_without_a_muzzle() -> None:
    d = _design()
    doc = _doc(d)
    apply_weapons(doc, d, known_generated=set(), mesh_for=_mesh_for(d))
    pistol = next(e for e in doc["entities"] if e["id"] == "glock_pistol")
    pistol["mesh"]["sockets"] = {}
    errors, _ = weapon_violations(doc, d)
    assert any("glock_pistol" in e and "muzzle" in e for e in errors)


def _full_manifest(d) -> AssetManifest:
    m = AssetManifest.model_validate({"prompt_id": "p", "items": [
        _item("glock_pistol", "Glock pistol"),
        _item("rpg_launcher", "RPG launcher", scale=1.0),
        _item("hunting_bow", "Hunting bow", scale=1.2),
    ]})
    extra = []
    for p in d.projectiles:
        if p.asset_id:
            extra.append(AssetManifest.model_validate({"prompt_id": "p", "items": [
                _item(p.asset_id, p.id, scale=p.length_m)]}).items[0])
    return m.model_copy(update={"items": list(m.items) + extra})


def test_projectile_is_rescaled_to_the_bore_and_arrow_to_the_draw() -> None:
    d = _design()
    rocket = next(p for p in d.projectiles if p.kind.value == "rocket")
    arrow = next(p for p in d.projectiles if p.kind.value == "arrow")
    m = _full_manifest(d)
    muzzle = next(s.name for s in d.item("rpg_launcher").sockets if s.kind.value == "muzzle")
    rocket_len = next(a for a in m.items if a.id == rocket.asset_id).target_scale_meters
    contract = {
        # An 85 mm tube on a 1.0 m launcher; a stubby rocket mesh 0.4 of its length wide.
        "rpg_launcher": {"sockets": {muzzle: {"bore_diameter_unit": 0.085}}},
        rocket.asset_id: {"cross_section_unit": 0.4},
    }
    m = m.model_copy(update={"items": [
        a.model_copy(update={"target_scale_meters": 0.3}) if a.id == arrow.asset_id else a
        for a in m.items]})
    scales, notes = projectile_scale_corrections(d, m, contract)
    drawn = 0.4 * scales[rocket.asset_id]
    assert abs(drawn - 0.085) < 1e-3 and scales[rocket.asset_id] < rocket_len
    assert scales[arrow.asset_id] >= d.item("hunting_bow").bow.max_draw_m + 0.1
    assert any("rescaled" in n for n in notes) and any("lengthened" in n for n in notes)
    # Inside the band: nothing changes.
    contract[rocket.asset_id]["cross_section_unit"] = 0.085 / rocket_len
    scales2, _ = projectile_scale_corrections(d, m, contract)
    assert rocket.asset_id not in scales2


def test_profile_carries_weapon_caps() -> None:
    q = derive_profile(target_device=TargetDevice.quest, asset_count=5, mesh_entity_count=10)
    pc = derive_profile(target_device=TargetDevice.pcvr, asset_count=5, mesh_entity_count=10)
    assert (q.max_live_projectiles, q.max_fx_quads, q.max_beams, q.max_decals,
            q.max_flash_lights, q.projectile_max_triangles) == (64, 1024, 4, 48, 2, 300)
    assert pc.max_live_projectiles == 256 and pc.max_fx_quads > q.max_fx_quads


def test_automatic_fire_thins_its_tracers() -> None:
    d = _design()
    rifle = d.item("m16_rifle")
    rifle.weapon.fire_mode = FireMode.auto
    rifle.weapon.tracer_every_n = 1
    assert weapon_params(rifle, d)["tracer_every_n"] == 3


def test_contract_rejects_a_second_gun() -> None:
    src = {"fire.cpp": (
        'if (input.trigger_pressed(hand)) {\n'
        '  Mat4 m = ctx.sockets->socket_world(self, "muzzle");\n'
        '  ctx.physics->raycast(o, d, 100.0f, hit);\n'
        '}\n int ammo_in_mag = 17;\n --ammo_in_mag;\n struct BulletTracer { Vec3 p; };\n')}
    problems = lint_task_sources("mechanic", src, weapons=True)
    text = " ".join(problems)
    assert "raycast" in text and "builtin.weapon" in text and "ammo" in text
    assert "projectile" in text
    # Without weapons only the unfiltered muzzle ray is still wrong.
    assert len(lint_task_sources("mechanic", src, weapons=False)) == 1


def test_contract_lets_a_hud_show_the_engines_ammo() -> None:
    hud = {"hud.cpp": (
        "WeaponState s{};\n"
        "if (ctx.weapons->state(ctx.weapons->held_weapon(1), &s)) {\n"
        "  int ammo_shown = s.ammo_in_magazine;\n"
        "  draw_number(ammo_shown);\n}\n")}
    assert lint_task_sources("mechanic", hud, weapons=True) == []
