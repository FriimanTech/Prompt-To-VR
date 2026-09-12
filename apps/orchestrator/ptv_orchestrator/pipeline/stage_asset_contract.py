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

Paths. ``asset_normalization.json`` stores GLB paths RELATIVE TO THE REPO
ROOT, so they must be resolved against ``settings.repo_root`` -- never
against the process working directory. This stage once used ``Path.cwd()``,
and when the orchestrator happened to be started from ``apps/web`` every
single lookup became ``<repo>/apps/web/projects/...``, found nothing, and
reported SUCCESS with "0 with sockets, 14 skipped". The whole pipeline then
ran green on top of an empty contract and only fell over at scene_compile.
Hence two rules here: resolve against ``repo_root``, and FAIL LOUDLY when a
GLB that normalization vouched for is not readable.
"""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any

from ptv_schemas import ProjectStage, ProjectStatus
from ptv_schemas.asset_manifest import AssetCategory

from .context import PipelineContext, StageResult
from .proc import proc_hooks

_STAGE = ProjectStage.asset_contract

# Sockets the tool derives today. An asset's ``socket_hints`` may name others;
# those are reported as unsatisfied rather than silently dropped, so the gap
# between "what gameplay asked for" and "what geometry could supply" is
# visible in the artifact instead of surfacing as a wrong muzzle in the
# headset.
_DERIVED_SOCKETS = ("muzzle", "grip")


class AssetContractError(RuntimeError):
    """A socket the game asked for could not be derived at all.

    Raised only when the failure is this stage's own -- a GLB that
    ``asset_normalization`` vouched for is unreadable, or the derivation tool
    errored on an asset whose ``socket_hints`` say gameplay depends on it.
    Assets with no hints degrade quietly, as before.
    """


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
    category_by_id = {item.id: item.category for item in manifest.items}
    targets = _resolvable_assets(
        normalization, hints_by_id, ctx.settings.repo_root
    )

    report: dict[str, dict[str, Any]] = {}
    derived = 0
    skipped = 0

    tool: Path | None = None
    for asset_id, glb_path in targets:
        ctx.raise_if_cancelled()
        hints = hints_by_id.get(asset_id, [])
        category = category_by_id.get(asset_id)

        excuse = _not_socketable(category, hints)
        if excuse is not None:
            report[asset_id] = {
                "status": "skipped",
                "reason": excuse,
                "sockets": {},
                "socket_hints": hints,
            }
            skipped += 1
            continue

        if tool is None:
            tool = _ensure_tool(ctx)
        entry = _derive(tool, glb_path, hints)
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

    # Fail loudly, all at once. A contract full of empty socket maps is not a
    # degraded build, it is a build where every mechanic that fires from a
    # muzzle silently does nothing -- and the only place that used to surface
    # was scene_compile, six stages later.
    blocking = _blocking_failures(report, hints_by_id)
    if blocking:
        ctx.store.record_stage(
            state,
            _STAGE,
            ProjectStatus.failed,
            message=blocking[0],
            artifacts=[rel],
        )
        ctx.emit(_STAGE, "failed", blocking[0])
        raise AssetContractError(
            "could not derive sockets that gameplay depends on:\n"
            + "\n".join(f"  - {b}" for b in blocking)
            + "\n\nasset_normalization.json records GLB paths relative to the"
            " repo root; check that those files exist and that"
            " ptv_glb_sockets can read them."
        )

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


def _not_socketable(category: Any, hints: list[str]) -> str | None:
    """Why this asset gets no derived sockets, or ``None`` to derive them.

    Explicit ``socket_hints`` always win: if the manifest says an asset needs
    a named point, we try, whatever the category. Absent hints, only ``prop``
    assets get the implicit ``muzzle``+``grip`` fallback, because those are
    the only two names geometry can derive and they are only ever meaningful
    on a held or mounted object. Everything else in the engine attaches by a
    different mechanism entirely, so a derived "muzzle" on a character or a
    HUD panel is a number nothing will ever read -- and one that makes the
    contract look like it says something it does not.

    The returned string is written into the artifact as ``reason``, so
    ``asset_contract.json`` explains an empty socket map instead of leaving a
    bare ``{}`` that reads exactly like the path bug did.
    """
    if hints:
        return None

    name = getattr(category, "value", str(category))
    if category == AssetCategory.prop:
        return None
    if category == AssetCategory.character:
        return (
            "no sockets derived: characters attach via rig joints and the"
            " BodySlots taxonomy (hand_r, waist, wrist_l), not mesh sockets"
        )
    if category == AssetCategory.ui:
        return (
            "no sockets derived: UI attaches to a body slot (wrist_l) and is"
            " driven by IInteractive, not by a mesh socket"
        )
    if category == AssetCategory.fx:
        return (
            "no sockets derived: effects are spawned AT another asset's"
            " socket; the socket belongs to the emitting asset"
        )
    return (
        f"no sockets derived: {name} assets have no socket_hints and only"
        " prop assets get the implicit muzzle/grip fallback"
    )


def _blocking_failures(
    report: dict[str, dict[str, Any]],
    hints_by_id: dict[str, list[str]],
) -> list[str]:
    """Failures that must sink the stage rather than be reported as skips.

    Two kinds:

    * ``missing`` -- ``asset_normalization`` said this asset had a good GLB and
      the file is not where it said it was. That is a broken workspace no
      matter what the asset is for, so it blocks unconditionally.
    * ``failed`` -- the derivation tool errored. Only blocking when the asset
      carries ``socket_hints``, because that is gameplay stating in the
      manifest that it needs a named point on this mesh.
    """
    out: list[str] = []
    for asset_id in sorted(report):
        entry = report[asset_id]
        status = entry.get("status")
        reason = entry.get("reason") or "no reason recorded"
        if status == "missing":
            out.append(f"asset '{asset_id}': {reason}")
        elif status == "failed" and hints_by_id.get(asset_id):
            hints = hints_by_id[asset_id]
            out.append(
                f"asset '{asset_id}' needs sockets {hints} but socket"
                f" derivation failed: {reason}"
            )
    return out


def _resolvable_assets(
    normalization: dict[str, dict[str, Any]],
    hints_by_id: dict[str, list[str]],
    repo_root: Path,
) -> list[tuple[str, Path]]:
    """(asset_id, ABSOLUTE source GLB) pairs worth running the tool on.

    ``asset_normalization.json`` stores paths RELATIVE TO THE REPO ROOT, so
    they must be resolved against ``repo_root`` -- never against the process
    working directory. Resolving against the cwd is what silently produced
    ``<repo>/apps/web/projects/...`` for a whole pipeline run when the
    orchestrator happened to be started from the web app's directory: every
    GLB "went missing", every asset was skipped, and the stage still reported
    success. Every sibling stage anchors on ``repo_root``; so does this one.
    """
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
        path = Path(raw)
        out.append((asset_id, path if path.is_absolute() else repo_root / path))
    return out


def _derive(tool: Path, glb_path: Path, hints: list[str]) -> dict[str, Any]:
    """Run ptv_glb_sockets on one GLB and shape its output for the contract.

    ``glb_path`` is already absolute: see :func:`_resolvable_assets`.
    """
    if not glb_path.exists():
        # NOT a skip: we only get here for assets asset_normalization marked
        # ok, so the file is supposed to be there. See _blocking_failures.
        shown = str(glb_path).replace("\\", "/")
        return {
            "status": "missing",
            "reason": (
                "asset_normalization vouched for this GLB but it is not on"
                f" disk: {shown}"
            ),
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
    # Only publish sockets this asset actually asked for. A hint-less asset
    # reaching this point is a prop (see :func:`_not_socketable`) and gets the
    # full derived set: harmless on a held or mounted object, and it means a
    # mechanic bound late still finds a muzzle to fire from.
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
        **proc_hooks(ctx),
    )
    if result.exit_code != 0:
        raise RuntimeError(
            "could not build ptv_glb_sockets (see logs); asset_contract cannot "
            "derive sockets without it"
        )
    for candidate in (ctx.settings.engine_dir / "build").rglob(exe):
        return candidate
    raise RuntimeError("ptv_glb_sockets built but the executable was not found")
