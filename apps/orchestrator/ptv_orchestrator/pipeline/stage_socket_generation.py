"""Stage 7a: find every socket interaction_design asked for on the real mesh.

For each item with sockets: ``ptv_glb_views`` renders ten views plus a
triangle-id buffer per view; Gemini (``socket_vision_model``) segments the
parts the sockets live on in every view; the masks are fused into triangle
regions by multi-view voting; per-kind solvers turn the regions into exact
transforms (a handle's axis and palm-height centre, a bore's centre and
diameter, a shield's straps); a render-and-verify pass draws the result on
the mesh and asks the model whether a person would hold it that way.

Results are written per asset under ``assets/<id>/sockets/`` and summarised in
``socket_report.json``; ``asset_contract`` merges them into the contract.
Everything is cached by GLB hash + socket spec, so a resume never re-bills a
vision call and never moves a socket.

Without a Google key, or with ``socket_vision_enabled`` off, the same solvers
run on geometry-only regions and every socket is labelled ``heuristic``.
A REQUIRED socket that cannot be produced at all fails the stage, listing
every one.
"""

from __future__ import annotations

import json
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from typing import Any, Optional

from ptv_providers import GeminiVisionClient, GeminiVisionConfig, VisionImage
from ptv_schemas import ProjectStage, ProjectStatus

from .context import PipelineContext, StageResult
from .interaction_rules import fallback_design
from .proc import proc_hooks
from .sockets.generate import AssetSocketResult, cache_key, generate_for_asset

_STAGE = ProjectStage.socket_generation


class SocketGenerationError(RuntimeError):
    """A socket gameplay requires could not be produced."""


def run(ctx: PipelineContext) -> StageResult:
    return run_with_vision(ctx)


def _vision_fns(ctx: PipelineContext):
    if not ctx.settings.socket_vision_enabled or not ctx.settings.google_api_key:
        return None, None, ""
    client = GeminiVisionClient(GeminiVisionConfig(
        api_key=ctx.settings.google_api_key, model=ctx.settings.socket_vision_model,
        timeout_s=ctx.settings.socket_vision_timeout_s))

    def one(prompt: str, png: bytes, schema: dict) -> dict:
        return client.analyze(prompt=prompt, images=[VisionImage(png)], schema=schema,
                              thinking_level="minimal")

    def multi(prompt: str, pngs: list[bytes], schema: dict) -> dict:
        return client.analyze(prompt=prompt, schema=schema, thinking_level="minimal",
                              images=[VisionImage(p, caption=f"Image {i + 1}:")
                                      for i, p in enumerate(pngs)])

    return one, multi, ctx.settings.socket_vision_model


def run_with_vision(ctx: PipelineContext, *, vision=None, vision_multi=None,
                    tool: Optional[Path] = None) -> StageResult:
    ctx.emit(_STAGE, "running", "Finding sockets on each held item")
    state = ctx.store.load_state(ctx.project_id)
    ctx.store.record_stage(state, _STAGE, ProjectStatus.running)

    manifest = ctx.asset_manifest or ctx.store.load_asset_manifest(ctx.project_id)
    design = ctx.interaction_design or ctx.store.load_interaction_design(ctx.project_id)
    if design is None:
        # A project from before interaction_design: derive holds from names,
        # but only for the assets that asked for sockets.
        hinted = {i.id for i in manifest.items if i.socket_hints}
        design = fallback_design(manifest, "")
        design.items = [i for i in design.items if i.asset_id in hinted]
        ctx.emit(_STAGE, "info", "no interaction_design.json; holds derived from names")

    model = ""
    if vision is None and vision_multi is None:
        vision, vision_multi, model = _vision_fns(ctx)
    else:
        model = "injected"
    if vision is None:
        ctx.emit(_STAGE, "warning", "no vision model (GOOGLE_API_KEY unset or disabled); "
                                    "sockets come from geometry alone")

    norm = _normalization(ctx)
    items = {i.id: i for i in manifest.items}
    todo = [it for it in design.items if it.sockets and it.asset_id in items]
    report: dict[str, Any] = {}
    failures: list[str] = []
    artifacts: list[str] = []

    if todo and tool is None:
        tool = _ensure_tool(ctx)

    def work(it) -> AssetSocketResult | dict:
        asset = items[it.asset_id]
        glb = _glb_for(ctx, norm.get(it.asset_id) or {})
        if glb is None or not glb.exists():
            return {"asset_id": it.asset_id, "status": "failed",
                    "warnings": ["no normalized GLB on disk"],
                    "missing_required": [s.name for s in it.sockets if s.required]}
        out_dir = ctx.project_dir / "assets" / it.asset_id / "sockets"
        key = cache_key(glb, it, model or "geometry")
        cached = out_dir / "result.json"
        if cached.exists():
            try:
                data = json.loads(cached.read_text(encoding="utf-8"))
                if data.get("cache_key") == key:
                    data["cached"] = True
                    return data
            except json.JSONDecodeError:
                pass
        out_dir.mkdir(parents=True, exist_ok=True)
        res = generate_for_asset(
            asset_id=it.asset_id, asset_name=asset.name, summary=asset.style_notes,
            glb=glb, item=it, out_dir=out_dir, tool=tool,
            target_scale_m=asset.target_scale_meters,
            view_size=ctx.settings.socket_view_size, vision=vision,
            vision_multi=vision_multi, verify_rounds=ctx.settings.socket_verify_rounds,
            max_parallel=ctx.settings.socket_vision_max_parallel,
            repo_root=ctx.settings.repo_root,
            log=lambda m: ctx.store.append_log(ctx.project_id, "socket_generation", m))
        data = res.to_json()
        data["cache_key"] = key
        data["source_glb"] = str(glb).replace("\\", "/")
        cached.write_text(json.dumps(data, indent=1), encoding="utf-8")
        return data

    # Each asset already fans out across views; two assets at a time keeps
    # the request rate reasonable.
    with ThreadPoolExecutor(max_workers=2) as pool:
        for data in pool.map(work, todo):
            ctx.raise_if_cancelled()
            aid = data["asset_id"]
            report[aid] = data
            names = ", ".join(f"{n} ({s.get('confidence')})"
                              for n, s in (data.get("sockets") or {}).items())
            ctx.emit(_STAGE, "info", f"{aid}: {names or 'no sockets'}"
                     + (" [cached]" if data.get("cached") else ""))
            for w in data.get("warnings") or []:
                ctx.emit(_STAGE, "warning", f"{aid}: {w}"[:240])
            if data.get("missing_required"):
                failures.append(f"asset '{aid}' needs sockets {data['missing_required']} "
                                "that could not be found")

    out = ctx.project_dir / "socket_report.json"
    out.write_text(json.dumps(report, indent=2, sort_keys=True), encoding="utf-8")
    rel = str(out.relative_to(ctx.settings.repo_root)).replace("\\", "/")
    artifacts.append(rel)

    if failures:
        msg = "; ".join(failures)
        ctx.store.record_stage(state, _STAGE, ProjectStatus.failed, message=msg[:400],
                               artifacts=artifacts)
        ctx.emit(_STAGE, "failed", msg[:400])
        raise SocketGenerationError("required sockets missing:\n" + "\n".join(
            f"  - {f}" for f in failures))

    verified = sum(1 for d in report.values() for s in (d.get("sockets") or {}).values()
                   if s.get("confidence") == "vision_verified")
    total = sum(len(d.get("sockets") or {}) for d in report.values())
    message = (f"{len(report)} item(s), {total} socket(s), {verified} vision-verified"
               + ("" if vision is not None else " (geometry only)"))
    ctx.store.record_stage(state, _STAGE, ProjectStatus.succeeded, message=message,
                           artifacts=artifacts)
    ctx.emit(_STAGE, "succeeded", message)
    return StageResult(stage=_STAGE, artifacts=artifacts, data=report)


def _normalization(ctx: PipelineContext) -> dict[str, dict[str, Any]]:
    path = ctx.project_dir / "asset_normalization.json"
    if not path.exists():
        return {}
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return {}


def _glb_for(ctx: PipelineContext, info: dict[str, Any]) -> Optional[Path]:
    if info.get("status") not in ("ok", "warn_no_material"):
        return None
    raw = info.get("glb_source") or info.get("glb_path")
    if not raw:
        return None
    p = Path(raw)
    return p if p.is_absolute() else ctx.settings.repo_root / p


def _ensure_tool(ctx: PipelineContext) -> Path:
    exe = "ptv_glb_views.exe" if sys.platform == "win32" else "ptv_glb_views"
    engine_build = ctx.settings.engine_dir / "build"
    if engine_build.exists():
        found = sorted(engine_build.rglob(exe), key=lambda p: p.stat().st_mtime,
                       reverse=True)
        if found:
            return found[0]
    ctx.emit(_STAGE, "info", "Building ptv_glb_views (first run)")
    from ..build import configure_and_build

    result = configure_and_build(
        source_dir=ctx.settings.engine_dir,
        build_dir=engine_build / "vs2026-x64",
        generator=ctx.settings.cmake_generator,
        platform=ctx.settings.cmake_generator_platform,
        config=ctx.settings.cmake_build_config,
        vcpkg_root=ctx.settings.vcpkg_root,
        build_target="ptv_glb_views",
        on_line=lambda m: ctx.emit(_STAGE, "log", m[:400]),
        **proc_hooks(ctx),
    )
    if result.exit_code != 0:
        raise RuntimeError("could not build ptv_glb_views (see logs)")
    for candidate in engine_build.rglob(exe):
        return candidate
    raise RuntimeError("ptv_glb_views built but the executable was not found")
