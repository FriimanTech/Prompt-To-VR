"""interaction_design: every held item gets a complete hold + sockets, weapons
get a runnable fire system, projectiles get a mesh only when they are seen,
explicit prompt instructions win, and scenery gets nothing."""

from __future__ import annotations

import json
import threading
from pathlib import Path

from ptv_schemas import (
    AssetManifest,
    DecisionSource,
    FireSystem,
    HoldKind,
    InteractionDesign,
    ItemUsage,
    ProjectileKind,
    RealWorldDimensions,
    SocketKind,
    TargetDevice,
)

from ptv_orchestrator.events import EventBus, PipelineEvent
from ptv_orchestrator.pipeline import stage_interaction_design as sid
from ptv_orchestrator.pipeline.context import PipelineContext
from ptv_orchestrator.pipeline.interaction_design_agent import (
    InteractionDesignAgentError,
    InteractionDesigner,
)
from ptv_orchestrator.pipeline.interaction_rules import (
    design_excerpt,
    fallback_design,
    interaction_violations,
    reconcile_manifest,
    repair_interaction_design,
)
from ptv_orchestrator.pipeline.orchestrator import _stages_for

PROMPT = ("Survive waves of aliens with a Glock pistol and an M16. I also carry a "
          "round shield, a sword, a bow with arrows and an RPG. There is a book on "
          "the table.")


def _item(aid: str, name: str, category: str = "prop", scale: float = 0.3) -> dict:
    return {"id": aid, "name": name, "category": category, "style_notes": name,
            "reference": {"prompt": name}, "target_scale_meters": scale}


def _manifest() -> AssetManifest:
    return AssetManifest.model_validate({"prompt_id": "p", "items": [
        _item("glock_pistol", "Glock pistol", scale=0.3),
        _item("m16_rifle", "M16 rifle", scale=1.0),
        _item("round_shield", "Round shield", scale=0.7),
        _item("knight_sword", "Knight sword", scale=1.0),
        _item("hunting_bow", "Hunting bow", scale=1.2),
        _item("rpg_launcher", "RPG launcher", scale=1.0),
        _item("old_book", "Old book", scale=0.25),
        _item("alien", "Alien", category="character", scale=1.8),
    ]})


def test_stage_order() -> None:
    for device in (TargetDevice.pcvr, TargetDevice.quest):
        order = [s.value for s, _ in _stages_for(device)]
        assert order.index("asset_manifest") < order.index("interaction_design") \
            < order.index("npc_plan") < order.index("reference_images")


def test_fallback_gives_every_weapon_a_complete_hold() -> None:
    m = _manifest()
    d = fallback_design(m, PROMPT)
    assert interaction_violations(d, prompt=PROMPT, manifest=m) == []
    pistol = d.item("glock_pistol")
    assert pistol.hold.kind == HoldKind.two_hand_wrap and pistol.hold.aim_aligned
    kinds = {s.kind for s in pistol.sockets}
    assert {SocketKind.hand_grip, SocketKind.support_wrap, SocketKind.muzzle} <= kinds
    assert pistol.weapon.fire_system == FireSystem.hybrid
    rifle = d.item("m16_rifle")
    assert rifle.hold.kind == HoldKind.two_hand_line
    assert any(s.kind == SocketKind.support_grip for s in rifle.sockets)
    shield = d.item("round_shield")
    assert shield.hold.kind == HoldKind.forearm_strap
    assert {s.kind for s in shield.sockets} == {SocketKind.hand_strap,
                                                SocketKind.forearm_strap}
    sword = d.item("knight_sword")
    assert {SocketKind.strike_base, SocketKind.strike_tip} <= {s.kind for s in sword.sockets}
    bow = d.item("hunting_bow")
    assert bow.bow is not None and d.projectile(bow.bow.arrow_projectile_id) is not None
    # Scenery and characters get nothing.
    assert d.item("old_book") is None and d.item("alien") is None


def test_invisible_bullets_get_no_mesh_and_visible_ones_do() -> None:
    m = _manifest()
    d = fallback_design(m, PROMPT)
    assert not d.projectile("pistol_round").needs_mesh
    rocket = next(p for p in d.projectiles if p.kind == ProjectileKind.rocket)
    arrow = next(p for p in d.projectiles if p.kind == ProjectileKind.arrow)
    assert rocket.needs_mesh and arrow.needs_mesh


def test_reconcile_appends_projectile_assets_at_their_real_size() -> None:
    m = _manifest()
    d = fallback_design(m, PROMPT)
    m2, d2, notes = reconcile_manifest(m, d)
    ids = {a.id: a for a in m2.items}
    rocket = next(p for p in d2.projectiles if p.kind == ProjectileKind.rocket)
    assert rocket.asset_id in ids
    assert abs(ids[rocket.asset_id].target_scale_meters - rocket.length_m) < 1e-3
    arrow = next(p for p in d2.projectiles if p.kind == ProjectileKind.arrow)
    assert d2.item("hunting_bow").bow.arrow_asset_id == arrow.asset_id
    # The arrow mesh asks for its nock and nose.
    arrow_item = d2.item(arrow.asset_id)
    assert {s.kind for s in arrow_item.sockets} == {SocketKind.projectile_nose,
                                                    SocketKind.arrow_nock}
    # socket_hints mirror the design.
    assert ids["glock_pistol"].socket_hints == [s.name for s in d2.item("glock_pistol").sockets]
    assert notes


def test_engine_drawn_effects_are_not_bought_as_meshes() -> None:
    m = AssetManifest.model_validate({"prompt_id": "p", "items": [
        _item("glock_pistol", "Glock pistol"),
        _item("rocket_smoke_trail", "Rocket smoke trail", category="fx"),
        _item("muzzle_flash_fx", "Muzzle flash"),
        _item("campfire_fx", "Campfire", category="fx"),
    ]})
    m2, _, notes = reconcile_manifest(m, fallback_design(m, "A Glock pistol."))
    ids = {a.id for a in m2.items}
    assert "rocket_smoke_trail" not in ids and "muzzle_flash_fx" not in ids
    # Not a shot's effect: kept.
    assert "campfire_fx" in ids and "glock_pistol" in ids
    assert any("engine-drawn" in n for n in notes)


def test_real_world_dimensions_correct_the_scale() -> None:
    m = _manifest()
    d = fallback_design(m, PROMPT)
    d.item("glock_pistol").real_world_dimensions = RealWorldDimensions(
        length_m=0.186, source="Glock")
    m2, _, notes = reconcile_manifest(m, d)
    assert next(a for a in m2.items if a.id == "glock_pistol").target_scale_meters == 0.186
    assert any("0.186" in n for n in notes)


def test_explicit_beam_is_kept_and_needs_no_projectile() -> None:
    m = _manifest()
    prompt = "A Glock that shoots a laser beam."
    d = InteractionDesign.model_validate({"prompt_id": "p", "items": [{
        "asset_id": "glock_pistol", "usage": "held_weapon", "wielders": ["player"],
        "hold": {"kind": "one_hand", "aim_aligned": True, "primary_socket": "grip"},
        "sockets": [{"name": "grip", "kind": "hand_grip",
                     "part_description": "the handle"}],
        "weapon": {"fire_system": "beam",
                   "provenance": {"source": "explicit",
                                  "prompt_quote": "shoots a laser beam"}},
    }]})
    fixed, _ = repair_interaction_design(d, prompt=prompt, manifest=m)
    w = fixed.item("glock_pistol").weapon
    assert w.fire_system == FireSystem.beam and w.beam is not None
    assert not w.projectile_id
    assert any(s.kind == SocketKind.muzzle for s in fixed.item("glock_pistol").sockets)
    assert interaction_violations(fixed, prompt=prompt, manifest=m) == []


def test_fast_bullet_projectile_becomes_hybrid_unless_explicit() -> None:
    m = _manifest()
    base = {"prompt_id": "p", "items": [{
        "asset_id": "glock_pistol", "usage": "held_weapon",
        "hold": {"kind": "one_hand", "aim_aligned": True, "primary_socket": "grip"},
        "sockets": [{"name": "grip", "kind": "hand_grip", "part_description": "handle"},
                    {"name": "muzzle", "kind": "muzzle", "part_description": "barrel"}],
        "weapon": {"fire_system": "projectile", "projectile_id": "r9"}}],
        "projectiles": [{"id": "r9", "kind": "bullet", "speed_mps": 360}]}
    d = InteractionDesign.model_validate(base)
    fixed, _ = repair_interaction_design(d, prompt="a glock", manifest=m)
    assert fixed.item("glock_pistol").weapon.fire_system == FireSystem.hybrid
    explicit = json.loads(json.dumps(base))
    explicit["items"][0]["weapon"]["provenance"] = {
        "source": "explicit", "prompt_quote": "real bullet drop"}
    d2 = InteractionDesign.model_validate(explicit)
    fixed2, _ = repair_interaction_design(d2, prompt="a glock with real bullet drop",
                                          manifest=m)
    assert fixed2.item("glock_pistol").weapon.fire_system == FireSystem.projectile


def test_scenery_with_sockets_is_rejected_then_stripped() -> None:
    m = _manifest()
    d = InteractionDesign.model_validate({"prompt_id": "p", "items": [{
        "asset_id": "old_book", "usage": "static_prop",
        "sockets": [{"name": "grip", "kind": "hand_grip", "part_description": "x x x"}]}]})
    assert any("scenery" in p for p in interaction_violations(d, prompt=PROMPT, manifest=m))
    fixed, _ = repair_interaction_design(d, prompt=PROMPT, manifest=m)
    assert fixed.item("old_book").sockets == []


def test_unquoted_explicit_is_reported() -> None:
    m = _manifest()
    d = fallback_design(m, PROMPT)
    d.item("glock_pistol").weapon.provenance.source = DecisionSource.explicit
    d.item("glock_pistol").weapon.provenance.prompt_quote = "fires plasma"
    assert any("verbatim" in p for p in interaction_violations(d, prompt=PROMPT, manifest=m))


def test_excerpt_mentions_weapons() -> None:
    text = design_excerpt(fallback_design(_manifest(), PROMPT))
    assert "glock_pistol" in text and "hybrid" in text and "forearm_strap" in text


# --- the stage itself, with a stub designer --------------------------------

class _Bus(EventBus):
    def __init__(self) -> None:
        super().__init__()
        self.events: list[PipelineEvent] = []
        self._l = threading.Lock()

    def publish(self, event: PipelineEvent) -> None:
        with self._l:
            self.events.append(event)


def _ctx(tmp_path: Path) -> PipelineContext:
    from types import SimpleNamespace

    from ptv_orchestrator.settings import Settings
    from ptv_orchestrator.storage import ProjectStore

    settings = Settings(projects_dir=tmp_path / "projects", engine_dir=tmp_path / "engine",
                        samples_dir=tmp_path / "samples", repo_root=tmp_path)
    store = ProjectStore(settings.projects_dir)
    state = store.create("Aliens", PROMPT)
    ctx = PipelineContext(project_id=state.project_id, settings=settings, store=store,
                          bus=_Bus())
    ctx.build_spec = SimpleNamespace(prompt_id="p", target_device=TargetDevice.quest,
                                     model_dump_json=lambda **_: "{}")
    ctx.asset_manifest = _manifest()
    return ctx


def test_stage_feeds_problems_back_then_saves_and_reconciles(tmp_path: Path) -> None:
    ctx = _ctx(tmp_path)
    good = fallback_design(_manifest(), PROMPT)
    broken = good.model_copy(deep=True)
    broken.item("glock_pistol").weapon.projectile_id = "missing_round"
    seen: list[str] = []

    def runner(msg: str) -> InteractionDesign:
        seen.append(msg)
        return broken if len(seen) == 1 else good

    sid.run_with_designer(ctx, designer=InteractionDesigner(instructions="x",
                                                             runner=runner))
    assert len(seen) == 2 and "problems" in seen[1]
    saved = InteractionDesign.model_validate_json(
        (ctx.project_dir / "interaction_design.json").read_text(encoding="utf-8"))
    assert saved.item("glock_pistol").usage == ItemUsage.held_weapon
    manifest = json.loads((ctx.project_dir / "asset_manifest.json").read_text("utf-8"))
    assert any(i["id"].endswith("_rocket") or "rocket" in i["id"] for i in manifest["items"])


def test_stage_falls_back_when_the_designer_is_unavailable(tmp_path: Path) -> None:
    ctx = _ctx(tmp_path)

    def runner(_msg: str) -> InteractionDesign:
        raise InteractionDesignAgentError("no key")

    sid.run_with_designer(ctx, designer=InteractionDesigner(instructions="x",
                                                             runner=runner,
                                                             max_retries=0))
    saved = InteractionDesign.model_validate_json(
        (ctx.project_dir / "interaction_design.json").read_text(encoding="utf-8"))
    assert saved.item("m16_rifle") is not None
