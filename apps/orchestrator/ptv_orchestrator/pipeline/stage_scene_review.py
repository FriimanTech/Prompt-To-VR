"""scene_review: look at the compiled scene before it is built.

The shooting range compiled, built and ran with duplicate tables, guns beside
the wrong table and lane-2 targets standing on the floor -- every check that
ran was about one thing at a time, and nothing ever LOOKED at the layout the
prompt described. This stage does, after ``scene_compile`` and before the
build:

1. ``ptv_scene_views`` renders each compiled scene (the same CPU rasteriser as
   ``ptv_glb_views``) from the player's spawn, an overview with the roof cut
   away, and two viewpoints per requirement that has entities -- one at eye
   level, one from above -- writing an entity-id buffer beside every image.
2. Deterministic checks first: a requirement whose asset never made it into
   the scene, requirement entities no viewpoint can see, a spawn that looks
   straight into a wall.
3. Then Gemini vision (``socket_vision_model``) judges each placement /
   layout / visual requirement from its own viewpoints: "four guns resting
   on the left table", "lane-2 targets hanging under red poles".

Every outcome is recorded in the requirements ledger (``source:
scene_review``) and in ``scene_review.json``; problems are flagged as
warnings and carried to the headset verification report. The stage never
fails the build: the scene is what it is by now, and a flagged APK the user
can look at beats none.
"""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path
from typing import Any, Optional

import numpy as np

from ptv_schemas import ProjectStage, ProjectStatus
from ptv_schemas.requirements import RequirementKind, RequirementOutcome

from . import requirements_ledger as rl
from .context import PipelineContext, StageResult

_STAGE = ProjectStage.scene_review
REPORT = "scene_review.json"

#: Requirement kinds whose truth shows in a picture of the scene.
VISIBLE_KINDS = {RequirementKind.asset, RequirementKind.visual, RequirementKind.placement,
                 RequirementKind.layout, RequirementKind.interaction, RequirementKind.npc}
#: Kinds the vision model is asked about (asset existence is a pixel count).
VISION_KINDS = {RequirementKind.placement, RequirementKind.layout, RequirementKind.visual}
#: Pixels of a requirement's entities, across its viewpoints, to count as seen.
MIN_VISIBLE_PX = 60
#: Closer than this at the centre of the spawn view is a wall in the face.
SPAWN_BLOCKED_M = 0.6

_HIDDEN_TAGS = {"template", "hidden", "player_rig", "player_system", "player_hand",
                "player_avatar"}

VISION_SCHEMA: dict[str, Any] = {
    "type": "object",
    "properties": {
        "satisfied": {"type": "string", "enum": ["yes", "no", "unclear"]},
        "observed": {"type": "string"},
        "issue": {"type": "string"},
    },
    "required": ["satisfied", "observed", "issue"],
}

VISION_PROMPT = (
    "You are reviewing the layout of a VR game level before it is built. The images are "
    "offline renders of the compiled scene (flat shading, no lighting effects). Things "
    "that only appear during play -- NPC waves, targets a challenge spawns later, "
    "projectiles -- are NOT in the renders; do not count their absence against the "
    "requirement. Judge ONLY this requirement from the game's prompt:\n"
    "  {statement}\n  (the prompt's words: \"{quote}\")\n"
    "Answer satisfied yes / no / unclear, describe what you observe in one or two "
    "sentences, and when it is not satisfied say exactly what is wrong (what is where, "
    "what is missing)."
)


# ---------------------------------------------------------------------------
# Scene geometry helpers
# ---------------------------------------------------------------------------

def _tags(e: dict) -> set[str]:
    return {str(t) for t in (e.get("tags") or [])}


def _pos(e: dict) -> np.ndarray:
    p = ((e.get("transform") or {}).get("position")) or [0, 0, 0]
    return np.array([float(v) for v in p[:3]], dtype=float)


def _size(e: dict) -> float:
    prim = e.get("primitive") or {}
    if prim.get("size"):
        return float(max(prim["size"][:3]))
    mesh = e.get("mesh") or {}
    return float(mesh.get("target_scale_meters") or 1.0)


def _visible_entity(e: dict) -> bool:
    return isinstance(e, dict) and not (_tags(e) & _HIDDEN_TAGS) and not e.get("attached_to") \
        and bool(e.get("mesh") or e.get("primitive"))


def _yaw_forward(rot: list[float]) -> np.ndarray:
    """-Z rotated by quaternion [x, y, z, w], flattened to the ground."""
    x, y, z, w = (list(rot) + [0, 0, 0, 1])[:4] if rot else (0, 0, 0, 1)
    f = np.array([-(2 * (x * z + w * y)), -(2 * (y * z - w * x)), -(1 - 2 * (x * x + y * y))])
    f[1] = 0.0
    n = np.linalg.norm(f)
    return f / n if n > 1e-6 else np.array([0.0, 0.0, -1.0])


def spawn_eye(doc: dict) -> tuple[np.ndarray, np.ndarray]:
    player = doc.get("player") or {}
    spawn = player.get("spawn") or {}
    p = np.array([float(v) for v in (spawn.get("position") or [0, 0, 0])[:3]])
    eye = p + np.array([0.0, float(player.get("height_meters") or 1.6) - 0.1, 0.0])
    return eye, _yaw_forward(spawn.get("rotation") or [0, 0, 0, 1])


# ---------------------------------------------------------------------------
# Requirement -> entities
# ---------------------------------------------------------------------------

def entities_for(req: Any, doc: dict, ledger: Any) -> list[dict]:
    """The scene entities a requirement is about: what claimed it (an asset,
    a scene entity, a mechanic binding), else entities named like its
    subjects."""
    ents = [e for e in doc.get("entities") or [] if isinstance(e, dict)]
    out: list[dict] = []

    def add(e: dict) -> None:
        if all(e is not o for o in out):
            out.append(e)

    claims = list(ledger.claims.get(req.id, [])) if ledger is not None else []
    if req.subjects and req.kind in (RequirementKind.placement, RequirementKind.visual,
                                     RequirementKind.npc):
        # Name matching claims whatever shares a word ("tactical" outfit ->
        # the equipment table): what a planner claimed on purpose wins, then
        # the claims that ARE the subject.
        explicit = [c for c in claims if c.note != "matched by name"]
        subject = rl.placed_subject(req)
        named = [c for c in claims if rl._names_subject(subject, c.item)]
        claims = explicit or named or claims
    for c in claims:
        for e in ents:
            mesh = e.get("mesh") or {}
            if c.stage in ("asset_manifest", "npc_plan") and mesh.get("asset_id") == c.item:
                add(e)
            elif c.stage == "scene_spec" and e.get("id") == c.item:
                add(e)
            elif c.stage == "mechanic_plan" and any(
                    isinstance(b, dict) and b.get("id") == c.item
                    for b in e.get("mechanic_bindings") or []):
                add(e)
    if not out:
        # Where something goes is about the THING, not the place: "goblins
        # at the front of the lane" must not pick up the lane's floor strips.
        about = ([rl.placed_subject(req)] if req.subjects and req.kind in (
            RequirementKind.placement, RequirementKind.visual, RequirementKind.npc)
            else req.subjects)
        for subject in about:
            for e in subject_matches(subject, ents):
                add(e)
    return out


def _ordered_tokens(text: str) -> list[str]:
    out: list[str] = []
    for t in rl._norm(text).split():
        if t in rl._STOP or len(t) < 3:
            continue
        t = t[:-1] if t.endswith("s") and len(t) > 4 else t
        if t not in out:
            out.append(t)
    return out


def subject_matches(subject: str, ents: list[dict]) -> list[dict]:
    """Entities named for a subject: its head noun ("button" of "red button")
    must be in the name; GLB entities beat world primitives that merely share
    a word (a red halo, a table's floor pad)."""
    toks = _ordered_tokens(subject)
    if not toks:
        return []
    head = toks[-1]
    found = [e for e in ents if isinstance(e, dict) and head in rl._tokens(
        f"{e.get('id', '')} {(e.get('mesh') or {}).get('asset_id', '')}".replace("_", " "))]
    meshes = [e for e in found if e.get("mesh")]
    return meshes or found


def claimed_assets(req: Any, ledger: Any) -> list[str]:
    if ledger is None:
        return []
    return [c.item for c in ledger.claims.get(req.id, []) if c.stage == "asset_manifest"]


# ---------------------------------------------------------------------------
# Cameras
# ---------------------------------------------------------------------------

def _cam(cid: str, eye: np.ndarray, target: np.ndarray, fov: float = 70.0,
         max_y: Optional[float] = None) -> dict:
    c = {"id": cid, "eye": [round(float(v), 3) for v in eye],
         "target": [round(float(v), 3) for v in target], "fov_deg": fov}
    if max_y is not None:
        c["max_y"] = round(float(max_y), 3)
    return c


def _regions(doc: dict) -> list[dict]:
    return [r for r in ((doc.get("navigation") or {}).get("regions") or [])
            if isinstance(r, dict) and r.get("min") and r.get("max")]


def _walkable(doc: dict, p: np.ndarray, margin: float = 0.3) -> bool:
    regions = _regions(doc)
    if not regions:
        return True
    return any(float(r["min"][0]) + margin <= p[0] <= float(r["max"][0]) - margin
               and float(r["min"][2]) + margin <= p[2] <= float(r["max"][2]) - margin
               for r in regions)


def _eye_level_spot(doc: dict, target: np.ndarray, towards: np.ndarray, d: float) -> np.ndarray:
    """Where a person would stand to look at ``target``: approaching from the
    player's side first, then turning around it, on walkable floor (a camera
    behind the outer wall sees the wall)."""
    for scale in (1.0, 0.6):
        for deg in (0, 45, -45, 90, -90, 135, -135, 180):
            a = np.radians(deg)
            dirv = np.array([towards[0] * np.cos(a) - towards[2] * np.sin(a), 0.0,
                             towards[0] * np.sin(a) + towards[2] * np.cos(a)])
            p = target - dirv * d * scale
            if _walkable(doc, p):
                return p
    return target - towards * d


def plan_cameras(doc: dict, groups: dict[str, list[dict]]) -> list[dict]:
    """Spawn view, overview, and an eye-level + an above view per requirement."""
    eye, fwd = spawn_eye(doc)
    cams = [_cam("spawn", eye, eye + fwd * 10.0 - np.array([0, 0.6, 0]), 80.0)]
    pts = [_pos(e) for e in doc.get("entities") or [] if _visible_entity(e)
           and "ground" not in _tags(e) and _size(e) < 30.0]
    floor = float(eye[1]) - 1.6
    if pts:
        arr = np.array(pts)
        lo, hi = np.percentile(arr, 5, axis=0), np.percentile(arr, 95, axis=0)
        centre = (lo + hi) / 2.0
        span = float(max(hi[0] - lo[0], hi[2] - lo[2], 8.0))
        back = centre - (centre - eye) * np.array([1, 0, 1]) / max(
            1e-6, float(np.linalg.norm((centre - eye) * np.array([1, 0, 1])))) * 0.6 * span
        cams.append(_cam("overview", back + np.array([0, 0.9 * span, 0]),
                         np.array([centre[0], floor, centre[2]]), 60.0, floor + 2.8))
    for rid, ents in groups.items():
        if not ents:
            continue
        arr = np.array([_pos(e) for e in ents])
        c = arr.mean(0)
        extent = float(max(np.max(np.linalg.norm(arr - c, axis=1)),
                           max(_size(e) for e in ents) * 0.5))
        d = max(2.5, 1.6 * extent + 1.5)
        flat = (c - eye) * np.array([1, 0, 1])
        n = float(np.linalg.norm(flat))
        towards = flat / n if n > 1e-6 else fwd
        near = _eye_level_spot(doc, c, towards, d)
        near[1] = max(float(c[1]) + max(1.0, 0.4 * extent), floor + 1.2)
        cams.append(_cam(f"{rid}__eye", near, c, 65.0))
        above = c - towards * (0.35 * d) + np.array([0.0, max(4.0, 1.4 * d), 0.0])
        cams.append(_cam(f"{rid}__above", above, c, 60.0, float(c[1]) + 2.6))
    return cams


# ---------------------------------------------------------------------------
# Deterministic checks
# ---------------------------------------------------------------------------

def deterministic_issues(doc: dict, ledger: Any, groups: dict[str, list[dict]],
                         views: dict[str, dict]) -> list[dict]:
    issues: list[dict] = []
    reqs = ledger.by_id() if ledger is not None else {}
    present = {(e.get("mesh") or {}).get("asset_id") for e in doc.get("entities") or []
               if isinstance(e, dict)}
    for rid, ents in groups.items():
        req = reqs.get(rid)
        if req is None:
            continue
        claimed = claimed_assets(req, ledger)
        missing = [a for a in claimed if a not in present]
        # Name matching claims effects too (a "laser blaster" claims the beam
        # fx the weapon draws at runtime): only NOTHING present is a failure.
        if missing and len(missing) == len(claimed) and not ents and req.kind in (
                RequirementKind.asset, RequirementKind.placement, RequirementKind.interaction):
            issues.append({"requirement": rid, "check": "asset_missing", "severity": "fail",
                           "detail": f"claimed asset(s) {missing} are not in the scene"})
        elif missing:
            issues.append({"requirement": rid, "check": "asset_not_placed", "severity": "warn",
                           "detail": f"claimed asset(s) {missing} are not placed (fine for "
                                     "what appears only at runtime)"})
        visible = [e for e in ents if _visible_entity(e)]
        if not visible:
            if not ents and not missing and req.kind in VISIBLE_KINDS:
                issues.append({"requirement": rid, "check": "no_entities", "severity": "warn",
                               "detail": "no scene entity found for this requirement"})
            continue
        ids = {str(e.get("id")) for e in visible}
        px = sum(int(n) for cid in (f"{rid}__eye", f"{rid}__above")
                 for eid, n in ((views.get(cid) or {}).get("visible") or {}).items() if eid in ids)
        if px < MIN_VISIBLE_PX:
            issues.append({"requirement": rid, "check": "not_visible", "severity": "warn",
                           "detail": f"its {len(ids)} entities show {px} px from its viewpoints"})
    spawn = views.get("spawn") or {}
    depth = spawn.get("centre_depth_m")
    if depth is not None and float(depth) < SPAWN_BLOCKED_M:
        issues.append({"requirement": None, "check": "spawn_blocked", "severity": "warn",
                       "detail": f"the spawn looks into '{spawn.get('centre_entity')}' "
                                 f"{float(depth):.2f} m away"})
    return issues


# ---------------------------------------------------------------------------
# Rendering + vision
# ---------------------------------------------------------------------------

def _tool(ctx: PipelineContext) -> Path:
    exe = "ptv_scene_views.exe" if sys.platform == "win32" else "ptv_scene_views"
    build = ctx.settings.engine_dir / "build"
    if build.exists():
        found = sorted(build.rglob(exe), key=lambda p: p.stat().st_mtime, reverse=True)
        if found:
            return found[0]
    ctx.emit(_STAGE, "info", "Building ptv_scene_views (first run)")
    from ..build import configure_and_build
    from .proc import proc_hooks

    result = configure_and_build(
        source_dir=ctx.settings.engine_dir, build_dir=build / "vs2026-x64",
        generator=ctx.settings.cmake_generator, platform=ctx.settings.cmake_generator_platform,
        config=ctx.settings.cmake_build_config, vcpkg_root=ctx.settings.vcpkg_root,
        build_target="ptv_scene_views", on_line=lambda m: ctx.emit(_STAGE, "log", m[:400]),
        **proc_hooks(ctx))
    if result.exit_code != 0:
        raise RuntimeError("could not build ptv_scene_views (see logs)")
    for candidate in build.rglob(exe):
        return candidate
    raise RuntimeError("ptv_scene_views built but the executable was not found")


def render(tool: Path, scene: Path, root: Path, cams: list[dict], out_dir: Path,
           size: int) -> dict[str, dict]:
    out_dir.mkdir(parents=True, exist_ok=True)
    cam_path = out_dir / "cameras.json"
    cam_path.write_text(json.dumps({"cameras": cams}, indent=2), encoding="utf-8")
    proc = subprocess.run([str(tool), "--scene", str(scene), "--root", str(root), "--cameras",
                           str(cam_path), "--out-dir", str(out_dir), "--size", str(size)],
                          capture_output=True, text=True, timeout=900)
    if proc.returncode != 0:
        raise RuntimeError((proc.stderr or proc.stdout or "ptv_scene_views failed")[-400:])
    meta = json.loads((out_dir / "views.json").read_text(encoding="utf-8"))
    return {v["id"]: v for v in meta.get("views") or []}


def vision_check(vision: Any, req: Any, images: list[tuple[Path, str]]) -> dict:
    from ptv_providers.gemini_vision_client import VisionImage

    try:
        return vision.analyze(
            prompt=VISION_PROMPT.format(statement=req.statement, quote=req.quote[:300]),
            schema=VISION_SCHEMA, thinking_level="low",
            images=[VisionImage(data=p.read_bytes(), caption=cap) for p, cap in images])
    except Exception as exc:  # noqa: BLE001 - recorded as unverified
        return {"satisfied": "unclear", "observed": "", "issue": f"vision failed: {exc}"[:300]}


def _vision_client(ctx: PipelineContext):
    if not (ctx.settings.scene_review_vision and ctx.settings.google_api_key):
        return None
    from ptv_providers.gemini_vision_client import GeminiVisionClient, GeminiVisionConfig

    v = GeminiVisionClient(GeminiVisionConfig(api_key=ctx.settings.google_api_key,
                                              model=ctx.settings.socket_vision_model))
    return v if v.is_available() else None


#: Viewpoints the headset revisits for screenshots (the spawn first).
MAX_VERIFY_VIEWPOINTS = 10


#: Two viewpoints closer than this show the same thing; keep the first.
VIEWPOINT_SPACING_M = 0.75
#: A headset viewpoint is a standing player's eye: no higher over its floor.
MAX_VIEW_HEIGHT_M = 3.0


def verify_viewpoints(cams: list[dict], doc: Optional[dict] = None) -> list[dict]:
    """The spawn and each requirement's eye-level camera, for the
    interactions verification pass: one per place (three requirements about
    one pistol share a camera) and, given the scene, only where a player
    can stand (a camera over the outer wall shows the sky)."""
    ordered = [c for c in cams if c["id"] == "spawn"] + \
              [c for c in cams if c["id"].endswith("__eye")]
    picked: list[dict] = []
    for c in ordered:
        eye = np.array([float(v) for v in c["eye"][:3]])
        if doc is not None and c["id"] != "spawn":
            if not _walkable(doc, eye, 0.2):
                continue
            # A camera 19 m up stands the player on the roof below it.
            floors = [float(r.get("floor_y", r["min"][1])) for r in _regions(doc)
                      if float(r["min"][0]) <= eye[0] <= float(r["max"][0])
                      and float(r["min"][2]) <= eye[2] <= float(r["max"][2])]
            if floors and eye[1] - max(f for f in floors if f <= eye[1] + 0.1) > MAX_VIEW_HEIGHT_M:
                continue
        if any(np.linalg.norm(eye - np.array(p["eye"][:3], dtype=float)) < VIEWPOINT_SPACING_M
               for p in picked):
            continue
        picked.append(c)
    return [{"id": c["id"], "eye": c["eye"], "target": c["target"]}
            for c in picked[:MAX_VERIFY_VIEWPOINTS]]


def attach_viewpoints(project_dir: Path, scene_id: str, cams: list[dict],
                      doc: Optional[dict] = None) -> list[Path]:
    """Write ``verify_viewpoints`` into the scene documents the build ships
    (``scene.json`` and its ``scene.<id>.json`` siblings) for this scene."""
    written = []
    views = verify_viewpoints(cams, doc)
    for path in [project_dir / "scene.json", *sorted(project_dir.glob("scene.*.json"))]:
        if not path.is_file():
            continue
        try:
            doc = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            continue
        if str(doc.get("scene_id") or "") != scene_id:
            continue
        doc["verify_viewpoints"] = views
        path.write_text(json.dumps(doc, indent=2), encoding="utf-8")
        written.append(path)
    return written


def reattach_viewpoints(project_dir: Path) -> int:
    """After a re-run of scene_compile (the memory correction loop): put the
    reviewed viewpoints back without rendering or asking anything again."""
    try:
        report = json.loads((project_dir / REPORT).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return 0
    n = 0
    for sid, entry in (report.get("scenes") or {}).items():
        n += len(attach_viewpoints(project_dir, sid, entry.get("cameras") or []))
    return n


def _scene_files(project_dir: Path) -> list[Path]:
    d = project_dir / "builds" / "scene"
    if not d.exists():
        return []
    return sorted(p for p in d.glob("*.scene.json") if not p.name.startswith("world."))


# ---------------------------------------------------------------------------
# Stage
# ---------------------------------------------------------------------------

def run(ctx: PipelineContext) -> StageResult:
    ctx.emit(_STAGE, "running", "Reviewing the compiled scene layout")
    state = ctx.store.load_state(ctx.project_id)
    ctx.store.record_stage(state, _STAGE, ProjectStatus.running)
    if not ctx.settings.scene_review_enabled:
        ctx.store.record_stage(state, _STAGE, ProjectStatus.succeeded, message="disabled")
        ctx.emit(_STAGE, "succeeded", "disabled")
        return StageResult(stage=_STAGE, artifacts=[])

    project_dir = ctx.project_dir
    ledger = rl.load(project_dir)
    scenes = _scene_files(project_dir)
    if not scenes:
        ctx.store.record_stage(state, _STAGE, ProjectStatus.succeeded,
                               message="no compiled scene to review")
        ctx.emit(_STAGE, "succeeded", "no compiled scene")
        return StageResult(stage=_STAGE, artifacts=[])

    tool = _tool(ctx)
    vision = _vision_client(ctx)
    if vision is None:
        ctx.emit(_STAGE, "info", "vision review unavailable (no Google key or disabled); "
                 "deterministic checks only")
    reqs = [r for r in (ledger.requirements if ledger is not None else [])
            if r.kind in VISIBLE_KINDS]
    budget = int(ctx.settings.scene_review_max_vision_checks)
    report: dict[str, Any] = {"scenes": {}}
    artifacts: list[str] = []
    verdicts: dict[str, list[tuple[str, str]]] = {}
    total_issues = 0

    for scene_path in scenes:
        ctx.raise_if_cancelled()
        doc = json.loads(scene_path.read_text(encoding="utf-8"))
        sid = str(doc.get("scene_id") or scene_path.stem.split(".")[0])
        groups = {r.id: entities_for(r, doc, ledger) for r in reqs}
        cams = plan_cameras(doc, {rid: [e for e in ents if _visible_entity(e)]
                                  for rid, ents in groups.items()})
        out_dir = project_dir / "builds" / "scene_review" / sid
        views = render(tool, scene_path, ctx.settings.repo_root, cams, out_dir,
                       int(ctx.settings.scene_review_image_size))
        attach_viewpoints(project_dir, sid, cams, doc)
        issues = deterministic_issues(doc, ledger, groups, views)
        for i in issues:
            ctx.emit(_STAGE, "warning",
                     f"{sid}: {i['check']}"
                     + (f" ({i['requirement']})" if i["requirement"] else "")
                     + f": {i['detail']}"[:300])
        total_issues += len(issues)
        checks: list[dict] = []
        for r in reqs:
            ents = groups.get(r.id) or []
            failed = [i for i in issues if i["requirement"] == r.id and i["severity"] == "fail"]
            if failed:
                verdicts.setdefault(r.id, []).append(("fail", f"{sid}: {failed[0]['detail']}"))
                continue
            if not ents:
                continue
            if not any(_visible_entity(e) for e in ents):
                # Only hidden templates: it appears during play (a wave, a
                # challenge's targets), which an offline render cannot show.
                if r.kind == RequirementKind.asset:
                    verdicts.setdefault(r.id, []).append(
                        ("pass", f"{sid}: {len(ents)} runtime template(s) in the scene"))
                else:
                    verdicts.setdefault(r.id, []).append(
                        ("unverified", f"{sid}: appears only during play (spawned from a "
                                       "template); judged on the headset"))
                continue
            seen = not any(i["requirement"] == r.id and i["check"] == "not_visible"
                           for i in issues)
            if r.kind not in VISION_KINDS or vision is None or budget <= 0 or not seen:
                if r.kind == RequirementKind.asset and seen:
                    verdicts.setdefault(r.id, []).append(
                        ("pass", f"{sid}: {len(ents)} entit(ies) present and visible"))
                continue
            budget -= 1
            images = [(out_dir / f"{r.id}__eye.png", "eye-level view"),
                      (out_dir / f"{r.id}__above.png", "view from above")]
            if r.kind == RequirementKind.layout and (out_dir / "overview.png").exists():
                images.append((out_dir / "overview.png", "overview of the whole level"))
            answer = vision_check(vision, r, [(p, c) for p, c in images if p.exists()])
            checks.append({"requirement": r.id, **answer})
            sat = answer.get("satisfied")
            outcome = "pass" if sat == "yes" else "fail" if sat == "no" else "unverified"
            evidence = f"{sid}: {answer.get('observed', '')}"
            if outcome == "fail":
                evidence += f" -- {answer.get('issue', '')}"
                ctx.emit(_STAGE, "warning",
                         f"{sid}: {r.id} looks unmet: {answer.get('issue', '')}"[:300])
            verdicts.setdefault(r.id, []).append((outcome, evidence))
        report["scenes"][sid] = {"scene": str(scene_path.name), "cameras": cams,
                                 "views": views, "issues": issues, "vision": checks,
                                 "images_dir": str(out_dir.relative_to(ctx.settings.repo_root))
                                 .replace("\\", "/")}

    # One outcome per requirement across scenes: any pass wins (it is met
    # somewhere), else any fail, else unverified.
    passed = failed = 0
    if ledger is not None:
        rl.clear_results(ledger, "scene_review")
        for rid, vs in verdicts.items():
            outcomes = [o for o, _ in vs]
            outcome = ("pass" if "pass" in outcomes else
                       "fail" if "fail" in outcomes else "unverified")
            passed += outcome == "pass"
            failed += outcome == "fail"
            rl.record_result(ledger, rid, RequirementOutcome(outcome),
                             [e for _, e in vs], "scene_review")
        rl.save(project_dir, ledger)
        artifacts.append(str(rl.write_report(project_dir, ledger)))
    report["summary"] = {"issues": total_issues, "pass": passed, "fail": failed,
                         "reviewed": len(verdicts)}
    out = project_dir / REPORT
    out.write_text(json.dumps(report, indent=2), encoding="utf-8")
    artifacts.append(str(out))
    message = (f"{len(scenes)} scene(s): {passed} requirement(s) look met, {failed} look "
               f"unmet, {total_issues} layout issue(s)")
    ctx.store.record_stage(state, _STAGE, ProjectStatus.succeeded, message=message,
                           artifacts=artifacts)
    ctx.emit(_STAGE, "succeeded", message)
    return StageResult(stage=_STAGE, artifacts=artifacts)
