"""Socket generation for ONE asset: render, segment, fuse, solve, verify.

Shared by the ``socket_generation`` stage and the benchmark script, so the
numbers in the benchmark are exactly the ones the pipeline ships.
"""

from __future__ import annotations

import hashlib
import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Optional

import numpy as np

from ptv_schemas import ItemInteraction, SocketKind

from .fallback import heuristic_regions
from .fusion import Fuser, Region
from .solvers import SolveResult, solve_item
from .verify import markers_for, overlay_paths, pick_overlay_views, verify, write_markers
from .views import ViewSet, load_views, render_views
from .vision import VisionFn, dump_raw, parts_for, segment_views

#: Bump when renders, fusion or solvers change in a way that moves sockets,
#: so cached results are recomputed rather than trusted.
SOCKET_PIPELINE_VERSION = "2"

MultiVisionFn = Callable[[str, list[bytes], dict], dict]


@dataclass
class AssetSocketResult:
    asset_id: str
    status: str                          # ok | failed
    sockets: dict = field(default_factory=dict)     # unit space, contract shape
    hold: Optional[dict] = None
    part_regions: dict = field(default_factory=dict)
    warnings: list[str] = field(default_factory=list)
    overlays: list[str] = field(default_factory=list)
    longest: float = 0.0
    method: str = "vision"               # vision | heuristic
    verified: dict = field(default_factory=dict)
    cross_section_unit: Optional[float] = None
    missing_required: list[str] = field(default_factory=list)

    def to_json(self) -> dict:
        return {
            "asset_id": self.asset_id, "status": self.status, "sockets": self.sockets,
            "hold": self.hold, "part_regions": self.part_regions,
            "warnings": self.warnings, "overlays": self.overlays,
            "longest": self.longest, "method": self.method, "verified": self.verified,
            "cross_section_unit": self.cross_section_unit,
            "missing_required": self.missing_required,
            "version": SOCKET_PIPELINE_VERSION,
        }


def cache_key(glb: Path, item: ItemInteraction, model: str) -> str:
    h = hashlib.sha256()
    h.update(glb.read_bytes())
    h.update(item.model_dump_json(include={"sockets", "hold", "usage", "weapon",
                                           "melee", "bow"}).encode())
    h.update(f"{SOCKET_PIPELINE_VERSION}|{model}".encode())
    return h.hexdigest()


def _to_unit(res: SolveResult, inv_longest: float, verified: dict) -> dict:
    out: dict = {}
    for name, s in res.sockets.items():
        conf = s.confidence
        ok = verified.get(name)
        if conf == "vision" and ok is not None and ok[0]:
            conf = "vision_verified"
        entry = {
            "position": [float(v) * inv_longest for v in s.position],
            "rotation": s.quat_xyzw(),
            "kind": s.kind,
            "confidence": conf,
        }
        ex = s.extras
        if "bore_diameter" in ex:
            entry["bore_diameter_unit"] = float(ex["bore_diameter"]) * inv_longest
        if "handle_thickness" in ex:
            entry["handle_thickness_unit"] = float(ex["handle_thickness"]) * inv_longest
        if "mirror" in ex:
            entry["mirror"] = ex["mirror"]
        if "lateral_offset" in ex:
            entry["lateral_offset_unit"] = float(ex["lateral_offset"]) * inv_longest
        if "wrap_of" in ex:
            entry["wrap_of"] = ex["wrap_of"]
        out[name] = entry
    return out


def _hold_json(item: ItemInteraction, res: SolveResult, sockets: dict) -> Optional[dict]:
    h = item.hold
    if h.kind.value == "none":
        return None
    out = {
        "kind": h.kind.value, "aim_aligned": h.aim_aligned, "two_hand": h.two_hand.value,
        "primary_socket": h.primary_socket if h.primary_socket in sockets else "",
        "support_socket": h.support_socket if h.support_socket in sockets else "",
        "forearm_socket": h.forearm_socket if h.forearm_socket in sockets else "",
        "finger_pose": h.finger_pose.value,
    }
    if res.forward is not None:
        out["forward"] = [float(v) for v in res.forward]
    if res.up is not None:
        out["up"] = [float(v) for v in res.up]
    return out


def _cross_section(vs: ViewSet, res: SolveResult) -> Optional[float]:
    """Projectiles: diameter across the travel axis, mesh units."""
    if res.forward is None:
        return None
    pts = vs.mesh.positions
    f = res.forward / (np.linalg.norm(res.forward) or 1.0)
    rel = pts - pts.mean(0)
    flat = rel - np.outer(rel @ f, f)
    return float(2.0 * np.percentile(np.linalg.norm(flat, axis=1), 98))


def generate_for_asset(*, asset_id: str, asset_name: str, summary: str, glb: Path,
                       item: ItemInteraction, out_dir: Path, tool: Path,
                       target_scale_m: float, view_size: int,
                       vision: Optional[VisionFn], vision_multi: Optional[MultiVisionFn],
                       verify_rounds: int = 1, max_parallel: int = 6,
                       repo_root: Optional[Path] = None,
                       log: Callable[[str], None] = lambda _m: None) -> AssetSocketResult:
    views_dir = out_dir / "views"
    render_views(tool, glb, views_dir, view_size)
    vs = load_views(views_dir)
    longest = vs.longest
    mpu = target_scale_m / longest if longest > 0 else 1.0
    inv_longest = 1.0 / longest
    result = AssetSocketResult(asset_id=asset_id, status="ok", longest=longest)
    parts = parts_for(item)
    usage_desc = {s.name: s.part_description for s in item.sockets}

    def rel(p: Path) -> str:
        if repo_root is not None:
            try:
                return str(p.relative_to(repo_root)).replace("\\", "/")
            except ValueError:
                pass
        return str(p).replace("\\", "/")

    def solve_with(regions: dict[str, Region]) -> SolveResult:
        return solve_item(item, regions, vs.mesh, metres_per_unit=mpu)

    res: Optional[SolveResult] = None
    regions: dict[str, Region] = {}
    note = ""
    verified: dict = {}
    if vision is not None:
        fuser = Fuser(vs)
        for round_ in range(max(0, verify_rounds) + 1):
            masks = segment_views(vs, parts, asset_name=asset_name, summary=summary,
                                  vision=vision, max_parallel=max_parallel,
                                  extra_note=note)
            (out_dir / f"vision_round{round_}.json").write_text(dump_raw(masks),
                                                               encoding="utf-8")
            regions = {p.part_id: fuser.region(p.part_id, masks,
                                               largest_only=p.primary) for p in parts}
            if "muzzle" not in regions:
                ms = next((s for s in item.sockets if s.kind == SocketKind.muzzle), None)
                if ms is not None and ms.name in regions:
                    regions["muzzle"] = regions[ms.name]
            res = solve_with(regions)
            if vision_multi is None or verify_rounds <= 0 or not res.sockets:
                break
            markers = write_markers(out_dir / "markers.json",
                                    markers_for(res.sockets, mpu, longest * mpu))
            render_views(tool, glb, views_dir, view_size, overlay=markers,
                         up=res.up, forward=res.forward)
            imgs = overlay_paths(views_dir, pick_overlay_views(len(vs.views)))
            # A synthesized socket marks a part the mesh does not have (a
            # bowstring the engine draws, straps Tripo did not model): there
            # is nothing for the verifier to look at.
            checkable = {n: s for n, s in res.sockets.items()
                         if s.confidence != "synthesized"}
            try:
                verified = verify(asset_name, checkable, imgs, usage_desc, vision_multi)
            except Exception as exc:  # noqa: BLE001
                result.warnings.append(f"verification unavailable: {str(exc)[:160]}")
                verified = {}
                break
            bad = {n: p for n, (ok, p) in verified.items() if not ok}
            if not bad:
                break
            log(f"{asset_id}: verification round {round_ + 1} failed: {bad}")
            if round_ >= verify_rounds:
                result.warnings.append("verification still failing: " + "; ".join(
                    f"{n}: {p}" for n, p in bad.items()))
                # The geometry fallback gets the same check; per socket, the
                # answer that passes wins, and a socket neither passes keeps
                # the vision solve, labelled low confidence.
                h_res = solve_with(heuristic_regions(vs, item))
                alt = {n: h_res.sockets[n] for n in bad if n in h_res.sockets}
                alt_ok: dict = {}
                if alt:
                    write_markers(out_dir / "markers_alt.json",
                                  markers_for(alt, mpu, longest * mpu))
                    alt_dir = out_dir / "views_alt"
                    render_views(tool, glb, alt_dir, view_size,
                                 overlay=out_dir / "markers_alt.json",
                                 up=res.up, forward=res.forward)
                    try:
                        alt_ok = verify(asset_name, alt, overlay_paths(
                            alt_dir, pick_overlay_views(len(vs.views))), usage_desc,
                            vision_multi)
                    except Exception:  # noqa: BLE001
                        alt_ok = {}
                for n in bad:
                    if n in alt and alt_ok.get(n, (False, ""))[0]:
                        s = alt[n]
                        s.confidence = "heuristic"
                        res.sockets[n] = s
                        verified[n] = alt_ok[n]
                    elif n in res.sockets:
                        res.sockets[n].confidence = "low"
                break
            note = ("A previous attempt was checked and was WRONG: " + "; ".join(
                f"{n}: {p}" for n, p in bad.items()) + ". Segment those parts again, "
                "carefully.")
        result.method = "vision"
    if res is None or not res.sockets:
        if vision is not None:
            result.warnings.append("vision pass produced no sockets; using geometry")
        regions = heuristic_regions(vs, item)
        res = solve_with(regions)
        for s in res.sockets.values():
            s.confidence = "heuristic"
        result.method = "heuristic"

    result.warnings += res.warnings
    result.sockets = _to_unit(res, inv_longest, verified)
    result.verified = {n: {"ok": ok, "problem": p} for n, (ok, p) in verified.items()}
    result.hold = _hold_json(item, res, result.sockets)
    for pid, reg in regions.items():
        if reg.empty:
            continue
        path = out_dir / f"region_{pid}.json"
        path.write_text(json.dumps({"triangles": reg.triangles.tolist()}), encoding="utf-8")
        result.part_regions[pid] = rel(path)
    if item.usage.value in ("projectile", "ammo"):
        cs = _cross_section(vs, res)
        if cs is not None:
            result.cross_section_unit = cs * inv_longest
    # Final overlays for review (always, so the UI can show them).
    markers = write_markers(out_dir / "markers.json",
                            markers_for(res.sockets, mpu, longest * mpu))
    render_views(tool, glb, views_dir, view_size, overlay=markers, up=res.up,
                 forward=res.forward)
    result.overlays = [rel(p) for p in overlay_paths(views_dir, pick_overlay_views(len(vs.views)))]
    result.missing_required = [s.name for s in item.sockets
                               if s.required and s.name not in result.sockets]
    if result.missing_required:
        result.status = "failed"
    return result
