"""FastAPI routes."""

from __future__ import annotations

import asyncio
import json
from pathlib import Path
from typing import AsyncIterator

from fastapi import APIRouter, BackgroundTasks, HTTPException, Response
from fastapi.responses import FileResponse
from sse_starlette.sse import EventSourceResponse

from ..engine_catalog import builtin_catalog, promoted_catalog
from ..events import PipelineEvent, get_bus
from ..pipeline import (
    resume_from_stage,
    resume_pipeline,
    run_pipeline,
    run_stage,
)
from ..settings import get_settings
from ..storage import ProjectStore
from .launch import find_starter_exe, launch_exe
from .models import CreateProjectRequest, LaunchRequest, RunStageRequest

router = APIRouter(prefix="/api", tags=["projects"])


def _store() -> ProjectStore:
    return ProjectStore(get_settings().projects_dir)


# ---------------------------------------------------------------------------
# Health / config
# ---------------------------------------------------------------------------

@router.get("/health")
def health():
    s = get_settings()
    return {
        "ok": True,
        "has_anthropic_key": bool(s.anthropic_api_key),
        "has_openai_key": bool(s.openai_api_key),
        "has_google_key": bool(s.google_api_key),
        "has_tripo_key": bool(s.tripo_api_key),
        "cmake_generator": s.cmake_generator,
        "vcpkg_root": s.vcpkg_root,
    }


@router.get("/settings")
def public_settings():
    s = get_settings()
    return {
        "claude_hard_model": s.claude_hard_model,
        "claude_light_model": s.claude_light_model,
        "openai_hard_model": s.openai_hard_model,
        "openai_light_model": s.openai_light_model,
        "gemini_image_model": s.gemini_image_model,
        "cmake_generator": s.cmake_generator,
        "projects_dir": str(s.projects_dir),
    }


# ---------------------------------------------------------------------------
# Projects
# ---------------------------------------------------------------------------

@router.get("/projects")
def list_projects():
    return [
        {
            "project_id": p.project_id,
            "title": p.title,
            "current_stage": p.current_stage.value,
            "status": p.status.value,
            "updated_at": p.updated_at.isoformat(),
        }
        for p in _store().list_projects()
    ]


@router.post("/projects", status_code=201)
def create_project(req: CreateProjectRequest):
    from ptv_schemas import PlayerExperience, TargetDevice

    try:
        device = TargetDevice(req.target_device)
    except ValueError:
        raise HTTPException(status_code=400, detail="unknown target_device")
    try:
        experience = PlayerExperience(req.player_experience)
    except ValueError:
        raise HTTPException(status_code=400, detail="unknown player_experience")
    state = _store().create(
        req.title, req.prompt, target_device=device,
        player_experience=experience,
    )
    return {
        "project_id": state.project_id,
        "target_device": device.value,
        "player_experience": experience.value,
    }


@router.get("/projects/{project_id}")
def get_project(project_id: str):
    try:
        state = _store().load_state(project_id)
    except FileNotFoundError:
        raise HTTPException(status_code=404, detail="project not found")
    return state.model_dump(mode="json")


@router.get("/projects/{project_id}/artifacts")
def list_artifacts(project_id: str):
    return [a.model_dump(mode="json") for a in _store().list_artifacts(project_id)]


@router.get("/projects/{project_id}/logs")
def list_logs(project_id: str):
    """Names of every ``.log`` file this project has produced.

    The UI uses this to render a tab per real log instead of a hard-coded
    list, so newly added stages (e.g. ``world``) show up automatically and no
    log is silently left out.
    """
    logs_dir = _store().dir_for(project_id) / "logs"
    if not logs_dir.exists():
        return []
    return sorted(p.stem for p in logs_dir.glob("*.log") if p.is_file())


@router.get("/projects/{project_id}/logs/{name}")
def get_log(project_id: str, name: str):
    store = _store()
    path = store.dir_for(project_id) / "logs" / f"{name}.log"
    if not path.exists():
        raise HTTPException(status_code=404, detail="log not found")
    return Response(content=path.read_text(encoding="utf-8"), media_type="text/plain")


@router.get("/projects/{project_id}/file")
def get_file(project_id: str, path: str):
    """Serve a raw artifact file (reference image, GLB, JSON, ...) so the UI
    can preview it.

    ``path`` is the artifact's repo-relative path (as returned by the
    ``/artifacts`` endpoint, e.g. ``projects/<id>/images/<asset>/ref.png``).
    We resolve it and hard-fail unless it lands *inside* this project's own
    directory -- no path traversal, no reading other projects or the repo.
    """
    store = _store()
    proj_dir = store.dir_for(project_id).resolve()
    repo_root = get_settings().projects_dir.parent
    # Accept both repo-relative (projects/<id>/...) and project-relative paths.
    for base in (repo_root, proj_dir):
        candidate = (base / path).resolve()
        try:
            candidate.relative_to(proj_dir)
        except ValueError:
            continue
        if candidate.is_file():
            return FileResponse(candidate)
    raise HTTPException(status_code=404, detail="file not found")


@router.post("/projects/{project_id}/run")
def run(project_id: str, req: RunStageRequest, background: BackgroundTasks):
    store = _store()
    try:
        store.load_state(project_id)
    except FileNotFoundError:
        raise HTTPException(status_code=404, detail="project not found")
    if req.stage:
        from ptv_schemas import ProjectStage

        try:
            stage = ProjectStage(req.stage)
        except ValueError:
            raise HTTPException(status_code=400, detail="unknown stage")
        background.add_task(run_stage, project_id, stage)
        return {"ok": True, "mode": "stage", "stage": stage.value}
    if req.resume_from:
        from ptv_schemas import ProjectStage

        try:
            stage = ProjectStage(req.resume_from)
        except ValueError:
            raise HTTPException(status_code=400, detail="unknown stage")
        background.add_task(resume_from_stage, project_id, stage)
        return {"ok": True, "mode": "resume_from", "stage": stage.value}
    if req.resume:
        background.add_task(resume_pipeline, project_id)
        return {"ok": True, "mode": "resume"}
    background.add_task(run_pipeline, project_id)
    return {"ok": True, "mode": "full"}


@router.post("/projects/{project_id}/abort")
def abort(project_id: str):
    """Stop the in-flight run, or unstick a project that only looks busy.

    Two cases, both of which used to leave the user with a project they
    could not touch:

    * A run IS in flight -- set its cancel flag and kill the child
      processes it spawned, so the stage stops at its next checkpoint
      instead of finishing a twenty-minute build first.
    * Nothing is running but the persisted state says ``running`` -- the
      orchestrator was killed mid-stage. Stamp it ``aborted`` so the UI
      re-enables Run and Resume. Startup does this automatically; the
      button is here for the case where it somehow did not.

    Requests already sent to a provider cannot be recalled, and their
    artifacts are still written -- a Resume then reuses them.
    """
    from ptv_schemas import ProjectStatus

    from ..runs import ABORT_MESSAGE, get_registry

    store = _store()
    try:
        state = store.load_state(project_id)
    except FileNotFoundError:
        raise HTTPException(status_code=404, detail="project not found")

    if get_registry().abort(project_id):
        # The run itself records the ``aborted`` status when it unwinds, so
        # that the stage the user actually interrupted is the one blamed.
        return {"ok": True, "aborted": True, "reconciled": False}

    if state.status == ProjectStatus.running:
        store.record_stage(
            state,
            state.current_stage,
            ProjectStatus.aborted,
            message=f"{ABORT_MESSAGE} (no run was in flight)",
        )
        get_bus().publish(
            PipelineEvent(
                project_id=project_id,
                stage=state.current_stage.value,
                status="aborted",
                message="cleared a stale running state",
            )
        )
        return {"ok": True, "aborted": False, "reconciled": True}

    return {"ok": True, "aborted": False, "reconciled": False}


@router.post("/projects/{project_id}/launch")
def launch(project_id: str, req: LaunchRequest):
    from ptv_schemas import TargetDevice

    from ..quest_ids import quest_package_name
    from .launch import install_and_launch_apk

    store = _store()
    try:
        state = store.load_state(project_id)
    except FileNotFoundError:
        raise HTTPException(status_code=404, detail="project not found")

    device = getattr(state, "target_device", TargetDevice.pcvr)

    # Quest: install + launch the APK on the connected headset via adb.
    if device == TargetDevice.quest:
        apk = Path(state.latest_build_apk) if state.latest_build_apk else None
        if apk is None or not apk.exists():
            raise HTTPException(
                status_code=400,
                detail="no APK; run the Quest build first",
            )
        s = get_settings()
        package = quest_package_name(s.android_package_prefix, project_id)
        try:
            install_and_launch_apk(apk, package=package, adb_path=s.adb_path)
        except Exception as e:  # adb not connected / device not authorized / etc.
            raise HTTPException(status_code=400, detail=str(e))
        return {"ok": True, "apk": str(apk), "package": package}

    # PC VR: launch the Windows exe (viewed over Quest Link).
    exe = Path(state.latest_build_exe) if state.latest_build_exe else None
    if exe is None or not exe.exists():
        raise HTTPException(status_code=400, detail="no build exe; run CMake build first")
    scene = Path(req.scene_json) if req.scene_json else (
        Path(state.latest_scene_json) if state.latest_scene_json else None
    )
    launch_exe(exe, scene_json=scene)
    return {"ok": True, "exe": str(exe)}


# ---------------------------------------------------------------------------
# Engine catalog (built-in + promoted mechanics)
# ---------------------------------------------------------------------------

@router.get("/engine/catalog")
def engine_catalog():
    """Return the live engine catalog.

    The mechanic-planner stage uses this exact data when prompting the LLM
    so what the UI shows here is what the planner actually sees.
    """
    s = get_settings()
    return {
        "builtin": builtin_catalog(),
        "promoted": promoted_catalog(s.repo_root),
    }


@router.post("/sample/launch")
def launch_sample():
    s = get_settings()
    exe = find_starter_exe(s.repo_root, s.cmake_build_config)
    if exe is None:
        raise HTTPException(
            status_code=400,
            detail=(
                "Starter-room executable not found. Build it first with"
                " `cmake --preset vs2026-x64 && cmake --build --preset vs2026-x64-release`."
            ),
        )
    launch_exe(exe)
    return {"ok": True, "exe": str(exe)}


# ---------------------------------------------------------------------------
# SSE events
# ---------------------------------------------------------------------------

@router.get("/projects/{project_id}/events")
async def events(project_id: str):
    bus = get_bus()

    async def _stream() -> AsyncIterator[dict]:
        async for ev in bus.subscribe(project_id):
            yield {"event": "pipeline", "data": ev.to_json()}

    return EventSourceResponse(_stream())


@router.get("/events")
async def global_events():
    bus = get_bus()

    async def _stream() -> AsyncIterator[dict]:
        async for ev in bus.subscribe("*"):
            yield {"event": "pipeline", "data": ev.to_json()}

    return EventSourceResponse(_stream())
