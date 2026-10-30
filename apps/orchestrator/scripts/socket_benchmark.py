"""Socket benchmark: generate a fixed set of held items and measure sockets.

Usage (from the repo root, with the project venv)::

    python apps/orchestrator/scripts/socket_benchmark.py setup      # project + manifest + design
    python apps/orchestrator/scripts/socket_benchmark.py generate   # reference images + Tripo + normalization (PAID)
    python apps/orchestrator/scripts/socket_benchmark.py sockets    # socket_generation on every item
    python apps/orchestrator/scripts/socket_benchmark.py existing   # the same on shipped project weapons
    python apps/orchestrator/scripts/socket_benchmark.py report     # contact sheet + metrics

``report`` writes ``projects/socket-benchmark/bench/contact_sheet.png`` (two
overlay views per item) and ``metrics.json``. When
``bench/ground_truth.json`` exists (per asset and socket: ``ok`` from a human
review of the overlays, optionally ``position_m`` in mesh metres), the metrics
include pass rates and position errors against it.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "apps" / "orchestrator"))

from ptv_orchestrator.events import EventBus  # noqa: E402
from ptv_orchestrator.pipeline.context import PipelineContext  # noqa: E402
from ptv_orchestrator.settings import Settings  # noqa: E402
from ptv_orchestrator.storage import ProjectStore  # noqa: E402
from ptv_schemas import (  # noqa: E402
    AssetManifest,
    InteractionDesign,
    ProjectState,
    TargetDevice,
)

PROJECT_ID = "socket-benchmark"
PROMPT = ("Socket benchmark: a Glock pistol, an assault rifle, a laser blaster, a "
          "one-handed sword, a two-handed greatsword, a round shield with two straps, "
          "a bow, an arrow, a war hammer, an RPG launcher and a 9 mm bullet.")


def _item(aid, name, scale, look, notes=""):
    return {"id": aid, "name": name, "category": "prop", "style_notes": notes or look,
            "target_scale_meters": scale,
            "reference": {"prompt": f"{look}, a single object, the whole object visible, "
                                    "side view, centred on a plain light grey background, "
                                    "realistic product photo lighting"}}


ITEMS = [
    _item("bench_glock", "Glock 17 pistol", 0.186, "a black polymer Glock 17 9mm pistol"),
    _item("bench_rifle", "M4 assault rifle", 0.84, "a black M4 carbine assault rifle with a "
          "pistol grip, magazine and handguard"),
    _item("bench_blaster", "Laser blaster", 0.30, "a sci-fi laser blaster pistol with a "
          "glowing emitter"),
    _item("bench_sword", "Arming sword", 0.95, "a one-handed medieval arming sword with a "
          "cross guard and leather-wrapped grip"),
    _item("bench_greatsword", "Claymore greatsword", 1.40, "a two-handed Scottish claymore "
          "greatsword with a long two-handed grip"),
    _item("bench_shield", "Round strapped shield", 0.76, "a round metal shield seen at a "
          "three-quarter angle from behind, showing the two leather straps on its back "
          "for the forearm and the hand"),
    _item("bench_bow", "Recurve bow", 1.50, "a wooden recurve bow with a taut string and a "
          "leather-wrapped handle"),
    _item("bench_arrow", "Arrow", 0.75, "a single wooden arrow with a steel broadhead and "
          "feather fletching"),
    _item("bench_hammer", "War hammer", 0.90, "a medieval war hammer with a steel head and a "
          "long wooden handle"),
    _item("bench_rpg", "RPG-7 launcher", 0.95, "an RPG-7 rocket launcher with its pistol grip "
          "and front grip, without the rocket loaded"),
    _item("bench_bullet", "9 mm bullet", 0.015, "a single copper 9mm bullet projectile "
          "(no casing), side view"),
]


def _s(name, kind, desc, required=True):
    return {"name": name, "kind": kind, "part_description": desc, "required": required}


DESIGN = {"prompt_id": "socket-benchmark", "items": [
    {"asset_id": "bench_glock", "usage": "held_weapon", "wielders": ["player"],
     "hold": {"kind": "two_hand_wrap", "aim_aligned": True, "two_hand": "optional",
              "primary_socket": "grip", "support_socket": "grip_wrap",
              "finger_pose": "trigger"},
     "sockets": [_s("grip", "hand_grip", "the pistol's handle below the slide, behind the "
                    "trigger guard, where the palm wraps"),
                 _s("grip_wrap", "support_wrap", "the pistol's handle"),
                 _s("muzzle", "muzzle", "the open end of the barrel at the front of the slide")],
     "weapon": {"fire_system": "hybrid", "projectile_id": "r9"}},
    {"asset_id": "bench_rifle", "usage": "held_weapon", "wielders": ["player"],
     "hold": {"kind": "two_hand_line", "aim_aligned": True, "two_hand": "optional",
              "primary_socket": "grip", "support_socket": "handguard",
              "finger_pose": "trigger"},
     "sockets": [_s("grip", "hand_grip", "the rifle's pistol grip behind the trigger, below "
                    "the receiver"),
                 _s("handguard", "support_grip", "the handguard around the barrel in front "
                    "of the magazine, where the second hand holds it from below"),
                 _s("muzzle", "muzzle", "the open end of the barrel (flash hider) at the front")],
     "weapon": {"fire_system": "hybrid", "fire_mode": "auto", "projectile_id": "r556"}},
    {"asset_id": "bench_blaster", "usage": "held_weapon", "wielders": ["player"],
     "hold": {"kind": "one_hand", "aim_aligned": True, "primary_socket": "grip",
              "finger_pose": "trigger"},
     "sockets": [_s("grip", "hand_grip", "the blaster's handle, where the palm wraps"),
                 _s("muzzle", "muzzle", "the emitter / barrel opening at the front where the "
                    "laser bolt leaves")],
     "weapon": {"fire_system": "hybrid", "visual": "energy_bolt", "projectile_id": "bolt"}},
    {"asset_id": "bench_sword", "usage": "held_weapon", "wielders": ["player"],
     "hold": {"kind": "one_hand", "primary_socket": "grip"},
     "sockets": [_s("grip", "hand_grip", "the sword's grip between the cross guard and the "
                    "pommel"),
                 _s("strike_base", "strike_base", "where the blade meets the cross guard"),
                 _s("strike_tip", "strike_tip", "the point of the blade")],
     "melee": {}},
    {"asset_id": "bench_greatsword", "usage": "held_weapon", "wielders": ["player"],
     "hold": {"kind": "two_hand_line", "two_hand": "optional", "primary_socket": "grip",
              "support_socket": "grip_low"},
     "sockets": [_s("grip", "hand_grip", "the upper half of the long grip, just below the "
                    "cross guard"),
                 _s("grip_low", "support_grip", "the lower half of the long grip, just above "
                    "the pommel"),
                 _s("strike_base", "strike_base", "where the blade meets the cross guard"),
                 _s("strike_tip", "strike_tip", "the point of the blade")],
     "melee": {}},
    {"asset_id": "bench_shield", "usage": "held_shield", "wielders": ["player"],
     "hold": {"kind": "forearm_strap", "primary_socket": "hand_strap",
              "forearm_socket": "forearm_strap", "finger_pose": "strap"},
     "sockets": [_s("hand_strap", "hand_strap", "the strap / handle on the back of the shield "
                    "that the hand grips, nearer the rim"),
                 _s("forearm_strap", "forearm_strap", "the strap on the back of the shield "
                    "the forearm slides through, nearer the centre")],
     "shield": {}},
    {"asset_id": "bench_bow", "usage": "held_weapon", "wielders": ["player"],
     "hold": {"kind": "one_hand", "primary_socket": "grip"},
     "sockets": [_s("grip", "hand_grip", "the bow's handle in the middle of the riser"),
                 _s("arrow_rest", "arrow_rest", "the arrow rest / shelf just above the handle"),
                 _s("string_rest", "string_rest", "the middle of the bowstring"),
                 _s("limb_tip_top", "limb_tip_top", "the tip of the upper limb"),
                 _s("limb_tip_bottom", "limb_tip_bottom", "the tip of the lower limb")],
     "bow": {"arrow_projectile_id": "arrow", "arrow_asset_id": "bench_arrow"},
     "weapon": {"fire_system": "drawn_launcher", "visual": "mesh"}},
    {"asset_id": "bench_arrow", "usage": "ammo",
     "sockets": [_s("nose", "projectile_nose", "the arrowhead at the front of the arrow"),
                 _s("nock", "arrow_nock", "the notched back end behind the fletching")]},
    {"asset_id": "bench_hammer", "usage": "held_weapon", "wielders": ["player"],
     "hold": {"kind": "one_hand", "primary_socket": "grip"},
     "sockets": [_s("grip", "hand_grip", "the lower part of the wooden handle where the hand "
                    "holds it"),
                 _s("strike_face", "strike_face", "the flat striking face of the hammer head")],
     "melee": {"blunt": True}},
    {"asset_id": "bench_rpg", "usage": "held_weapon", "wielders": ["player"],
     "hold": {"kind": "two_hand_line", "aim_aligned": True, "two_hand": "optional",
              "primary_socket": "grip", "support_socket": "front_grip",
              "finger_pose": "trigger"},
     "sockets": [_s("grip", "hand_grip", "the pistol grip with the trigger (on an RPG-7 "
                    "this is the FORWARD of the two grips under the tube)"),
                 _s("front_grip", "support_grip", "the other grip under the tube, behind "
                    "the trigger grip, that the second hand holds"),
                 _s("muzzle", "muzzle", "the narrow FRONT opening of the launch tube "
                    "where the rocket leaves (the end with the sights), NOT the wide "
                    "flared exhaust cone at the back")],
     "weapon": {"fire_system": "projectile", "projectile_id": "rocket"}},
    {"asset_id": "bench_bullet", "usage": "projectile",
     "sockets": [_s("nose", "projectile_nose", "the rounded tip of the bullet")]},
], "projectiles": [
    {"id": "r9", "kind": "bullet"}, {"id": "r556", "kind": "bullet", "speed_mps": 900},
    {"id": "bolt", "kind": "energy_bolt", "speed_mps": 60},
    {"id": "arrow", "kind": "arrow", "speed_mps": 70, "needs_mesh": True,
     "asset_id": "bench_arrow"},
    {"id": "rocket", "kind": "rocket", "speed_mps": 120, "needs_mesh": True}]}


def _ctx() -> PipelineContext:
    settings = Settings()
    store = ProjectStore(settings.projects_dir)
    return PipelineContext(project_id=PROJECT_ID, settings=settings, store=store,
                           bus=EventBus())


def setup() -> None:
    ctx = _ctx()
    d = ctx.project_dir
    for sub in ("images", "assets", "generated_code", "logs", "builds"):
        (d / sub).mkdir(parents=True, exist_ok=True)
    (d / "prompt.txt").write_text(PROMPT, encoding="utf-8")
    if not (d / "project_state.json").exists():
        ctx.store.save_state(ProjectState(project_id=PROJECT_ID, title="Socket benchmark",
                                          prompt=PROMPT, target_device=TargetDevice.quest))
    manifest = AssetManifest.model_validate({"prompt_id": PROJECT_ID, "items": ITEMS})
    ctx.store.save_asset_manifest(PROJECT_ID, manifest)
    design = InteractionDesign.model_validate(DESIGN)
    ctx.store.save_interaction_design(PROJECT_ID, design)
    print(f"set up {d}: {len(ITEMS)} items")


def generate() -> None:
    from ptv_orchestrator.pipeline import (  # noqa: PLC0415
        stage_asset_normalize,
        stage_reference_images,
        stage_tripo_models,
    )

    ctx = _ctx()
    for fn in (stage_reference_images.run, stage_tripo_models.run, stage_asset_normalize.run):
        print(fn.__module__, fn(ctx).message)


def sockets() -> None:
    from ptv_orchestrator.pipeline import stage_socket_generation  # noqa: PLC0415

    ctx = _ctx()
    res = stage_socket_generation.run(ctx)
    print(json.dumps({k: {n: s.get("confidence") for n, s in (v.get("sockets") or {}).items()}
                      for k, v in res.data.items()}, indent=1))


EXISTING = [
    ("20261023-205949-humans-vs-aliens-survival", "survival_pistol"),
    ("20260907-144543-shooting-range-game", "range_pistol"),
    ("20260907-144543-shooting-range-game", "range_rifle"),
    ("20261019-202355-fun-goblin-shooter-game", "laser_blaster"),
]


def existing() -> None:
    """socket_generation on shipped project weapons, into the benchmark folder
    (the projects themselves are untouched)."""
    from ptv_orchestrator.pipeline.interaction_rules import fallback_design  # noqa: PLC0415
    from ptv_orchestrator.pipeline.sockets.generate import generate_for_asset  # noqa: PLC0415
    from ptv_orchestrator.pipeline.stage_socket_generation import (  # noqa: PLC0415
        _ensure_tool,
        _vision_fns,
    )

    ctx = _ctx()
    vision, multi, _ = _vision_fns(ctx)
    tool = _ensure_tool(ctx)
    out_root = ctx.project_dir / "bench" / "existing"
    report = {}
    for pid, aid in EXISTING:
        proj = ctx.settings.projects_dir / pid
        manifest = AssetManifest.model_validate_json(
            (proj / "asset_manifest.json").read_text("utf-8"))
        item = fallback_design(manifest, "").item(aid)
        norm = json.loads((proj / "asset_normalization.json").read_text("utf-8"))[aid]
        glb = ctx.settings.repo_root / (norm.get("glb_source") or norm["glb_path"])
        a = next(i for i in manifest.items if i.id == aid)
        r = generate_for_asset(asset_id=aid, asset_name=a.name, summary=a.style_notes,
                               glb=glb, item=item, out_dir=out_root / aid, tool=tool,
                               target_scale_m=a.target_scale_meters,
                               view_size=ctx.settings.socket_view_size, vision=vision,
                               vision_multi=multi,
                               verify_rounds=ctx.settings.socket_verify_rounds,
                               repo_root=ctx.settings.repo_root, log=print)
        report[aid] = r.to_json()
        print(aid, {n: s["confidence"] for n, s in r.sockets.items()}, r.warnings)
    (out_root / "report.json").write_text(json.dumps(report, indent=1), encoding="utf-8")


def report() -> None:
    from PIL import Image, ImageDraw  # noqa: PLC0415

    ctx = _ctx()
    bench = ctx.project_dir / "bench"
    bench.mkdir(parents=True, exist_ok=True)
    rows = []
    sg = json.loads((ctx.project_dir / "socket_report.json").read_text("utf-8")) \
        if (ctx.project_dir / "socket_report.json").exists() else {}
    ex_path = bench / "existing" / "report.json"
    if ex_path.exists():
        sg.update(json.loads(ex_path.read_text("utf-8")))
    truth = json.loads((bench / "ground_truth.json").read_text("utf-8")) \
        if (bench / "ground_truth.json").exists() else {}
    metrics = {"assets": {}, "sockets_total": 0, "vision_verified": 0}
    for aid, data in sorted(sg.items()):
        socks = data.get("sockets") or {}
        metrics["sockets_total"] += len(socks)
        metrics["vision_verified"] += sum(1 for s in socks.values()
                                          if s.get("confidence") == "vision_verified")
        entry = {"confidence": {n: s.get("confidence") for n, s in socks.items()},
                 "warnings": data.get("warnings") or []}
        t = truth.get(aid) or {}
        if t:
            entry["review"] = {n: bool((t.get(n) or {}).get("ok")) for n in socks}
        metrics["assets"][aid] = entry
        imgs = [REPO / p for p in (data.get("overlays") or [])[:2]]
        imgs = [i for i in imgs if i.exists()]
        if imgs:
            rows.append((aid, imgs))
    reviewed = [ok for a in metrics["assets"].values() for ok in (a.get("review") or {}).values()]
    if reviewed:
        metrics["review_pass_rate"] = sum(reviewed) / len(reviewed)
    if rows:
        w = 384
        sheet = Image.new("RGB", (w * 2, (w + 24) * len(rows)), (40, 40, 40))
        draw = ImageDraw.Draw(sheet)
        for r, (aid, imgs) in enumerate(rows):
            y = r * (w + 24)
            draw.text((6, y + 4), aid, fill=(255, 255, 255))
            for c, path in enumerate(imgs):
                sheet.paste(Image.open(path).convert("RGB").resize((w, w)), (c * w, y + 24))
        sheet.save(bench / "contact_sheet.png")
    (bench / "metrics.json").write_text(json.dumps(metrics, indent=1), encoding="utf-8")
    print(json.dumps(metrics, indent=1))


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "report"
    {"setup": setup, "generate": generate, "sockets": sockets, "existing": existing,
     "report": report}[cmd]()
