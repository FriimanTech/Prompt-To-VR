"""Stage 7b: derive each asset's named sockets from its geometry.

A socket is a named LOCAL TRANSFORM on an asset -- "the muzzle is here,
pointing that way". It is what lets the code-generation agent write ONE
weapon-agnostic mechanic::

    const Mat4 m = ctx.sockets->socket_world(self, "muzzle");
    spawn_projectile(Vec3(m[3]), Vec3(m[2]) * speed);

and have it fire from the correct point on a pistol, a rifle and a rocket
launcher alike. The agent never sees the mesh and so can never know where any
particular barrel ends; that is the whole point of the split. This stage has
the geometry open and produces the numbers behind the named contract.

Deterministic by construction: the derivation is pure geometry (principal-axis
PCA plus mass asymmetry) run by the ``ptv_glb_sockets`` C++ tool. No model
calls, no randomness, so re-running a project never silently moves a muzzle.

Ordering. This runs immediately after ``asset_normalization`` and BEFORE
``code_generation``, because codegen consumes the contract. Sockets are
derived from the SOURCE GLB in unit space (normalized by the longest-axis
extent), which is why running before ``asset_optimization`` is safe: the
optimizer preserves the source coordinate system, and unit-space coordinates
are immune to the small AABB drift decimation introduces. The scene compiler
multiplies by ``target_scale_meters`` to get metres, exactly as it already
does for ``bounds_radius_m``.
"""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any

from ptv_schemas import ProjectStage, ProjectStatus

from .context import PipelineContext, StageResult

_STAGE = ProjectStage.asset_contract

# Sockets the tool derives today. An asset's ``socket_hints`` may name others;
# those are reported as unsatisfied rather than silently dropped, so the gap
# between "what gameplay asked for" and "what geometry could supply" is
# visible in the artifact instead of surfacing as a wrong muzzle in the
# headset.
_DERIVED_SOCKETS = ("muzzle", "grip")


def run(ctx: PipelineContext) -> StageResult:
    ctx.emit(_STAGE, "running", "Deriving asset sockets")
    state = ctx.store.load_state(ctx.project_id)
    ctx.store.record_stage(state, _STAGE, ProjectStatus.running)

    manifest = ctx.asset_manifest or ctx.store.load_asset_manifest(ctx.project_id)
    norm_path = ctx.project_dir / "asset_normalization.json"
    normalization: dict[str, dict[str, Any]] = {}
    if norm_path.exists():
        try:
            normalization = json.loads(norm_path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            normalization = {}

    hints_by_id = {item.id: list(item.socket_hints) for item in manifest.items}
    targets = _resolvable_assets(normalization, hints_by_id)

    report: dict[str, dict[str, Any]] = {}
    derived = 0
    skipped = 0

    if targets:
        tool = _ensure_tool(ctx)
        for asset_id, glb_path in targets:
            entry = _derive(tool, glb_path, hints_by_id.get(asset_id, []))
            report[asset_id] = entry
            if entry.get("sockets"):
                derived += 1
            else:
                skipped += 1

    # Assets with no usable GLB still get an entry, so the scene compiler's
    # socket validator can say "this asset has no contract" rather than
    # silently finding nothing.
    for item in manifest.items:
        report.setdefault(
            item.id,
            {
                "status": "skipped",
                "reason": "no valid GLB in asset_normalization.json",
                "sockets": {},
                "socket_hints": hints_by_id.get(item.id, []),
            },
        )

    out = ctx.project_dir / "asset_contract.json"
    out.write_text(json.dumps(report, indent=2, sort_keys=True), encoding="utf-8")
    rel = str(out.relative_to(ctx.settings.repo_root)).replace("\\", "/")

    ctx.store.record_stage(
        state,
        _STAGE,
        ProjectStatus.succeeded,
        message=f"{derived} with sockets, {skipped} skipped",
        artifacts=[rel],
    )
    ctx.emit(_STAGE, "succeeded", f"derived={derived} skipped={skipped}")
    return StageResult(stage=_STAGE, artifacts=[rel], data=report)


# ---------------------------------------------------------------------------
# Derivation
# ---------------------------------------------------------------------------


def _resolvable_assets(
    normalization: dict[str, dict[str, Any]],
    hints_by_id: dict[str, list[str]],
) -> list[tuple[str, Path]]:
    """(asset_id, source GLB) pairs worth running the tool on."""
    out: list[tuple[str, Path]] = []
    for asset_id in hints_by_id:
        info = normalization.get(asset_id) or {}
        if info.get("status") not in ("ok", "warn_no_material"):
            continue
        # ``glb_source`` is set by asset_optimization when it repoints
        # ``glb_path`` at LOD0. We want the undecimated source either way, and
        # normally this stage runs first so only ``glb_path`` exists.
        raw = info.get("glb_source") or info.get("glb_path")
        if not raw:
            continue
        out.append((asset_id, Path(raw)))
    return out


def _derive(tool: Path, glb_path: Path, hints: list[str]) -> dict[str, Any]:
    """Run ptv_glb_sockets on one GLB and shape its output for the contract."""
    if not glb_path.is_absolute():
        glb_path = Path.cwd() / glb_path
    if not glb_path.exists():
        return {
            "status": "skipped",
            "reason": f"GLB not found: {glb_path}",
            "sockets": {},
            "socket_hints": hints,
        }

    with tempfile.TemporaryDirectory() as tmp:
        report_file = Path(tmp) / "sockets.json"
        proc = subprocess.run(
            [
                str(tool),
                "--input",
                str(glb_path),
                "--output",
                str(report_file),
            ],
            capture_output=True,
            text=True,
        )
        if proc.returncode != 0 or not report_file.exists():
            return {
                "status": "failed",
                "reason": (proc.stderr or proc.stdout or "tool failed").strip()[:400],
                "sockets": {},
                "socket_hints": hints,
            }
        raw = json.loads(report_file.read_text(encoding="utf-8"))

    sockets = raw.get("sockets") or {}
    # Only publish sockets this asset actually asked for. A prop with no hints
    # gets the full derived set: harmless, and it means a mechanic bound late
    # still finds a muzzle to fire from.
    if hints:
        wanted = {h for h in hints if h in sockets}
        sockets = {k: v for k, v in sockets.items() if k in wanted}

    unsatisfied = [h for h in hints if h not in sockets]
    return {
        "status": "ok",
        # Unit space: divide by longest-axis extent. Scene compile multiplies
        # by target_scale_meters, mirroring bounds_radius_m.
        "space": "unit",
        "sockets": sockets,
        "socket_confidence": {
            k: (raw.get("socket_confidence") or {}).get(k, "heuristic")
            for k in sockets
        },
        "socket_hints": hints,
        "unsatisfied_hints": unsatisfied,
        "bounds": {
            "aabb_min": raw.get("aabb_min"),
            "aabb_max": raw.get("aabb_max"),
            "longest_axis": raw.get("longest_axis"),
        },
        "principal_axis": raw.get("principal_axis"),
        "source_glb": str(glb_path).replace("\\", "/"),
    }


# ---------------------------------------------------------------------------
# Tool discovery
# ---------------------------------------------------------------------------


def _ensure_tool(ctx: PipelineContext) -> Path:
    """Locate ``ptv_glb_sockets``, building it once if necessary.

    Building here also warms ``ptv_runtime``, which the code-generation
    stage's compile-verify step needs immediately afterwards -- so paying for
    it at this point in the pipeline costs nothing overall.
    """
    exe = "ptv_glb_sockets.exe" if sys.platform == "win32" else "ptv_glb_sockets"
    engine_build = ctx.settings.engine_dir / "build"
    if engine_build.exists():
        for candidate in engine_build.rglob(exe):
            return candidate

    ctx.emit(_STAGE, "info", "Building ptv_glb_sockets (first run)")
    from ..build import configure_and_build

    result = configure_and_build(
        source_dir=ctx.settings.engine_dir,
        build_dir=engine_build / "vs2026-x64",
        generator=ctx.settings.cmake_generator,
        platform=ctx.settings.cmake_generator_platform,
        config=ctx.settings.cmake_build_config,
        vcpkg_root=ctx.settings.vcpkg_root,
        build_target="ptv_glb_sockets",
        on_line=lambda m: ctx.emit(_STAGE, "log", m[:400]),
    )
    if result.exit_code != 0:
        raise RuntimeError(
            "could not build ptv_glb_sockets (see logs); asset_contract cannot "
            "derive sockets without it"
        )
    for candidate in (ctx.settings.engine_dir / "build").rglob(exe):
        return candidate
    raise RuntimeError("ptv_glb_sockets built but the executable was not found")
