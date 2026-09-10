"""FastAPI app entrypoint."""

from __future__ import annotations

import sys
from contextlib import asynccontextmanager

from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware

from .api.routes import router
from .runs import reconcile_interrupted_runs
from .settings import get_settings
from .storage import ProjectStore


@asynccontextmanager
async def lifespan(_app: FastAPI):
    """Repair projects the previous process left mid-run.

    ``ProjectState.status`` lives on disk while the in-flight run registry
    lives in memory, so Ctrl+C during a stage used to leave ``running``
    persisted with nothing running -- and the web panel disables every
    button while a project claims to be running, locking the user out of
    their own project until they hand-edited the JSON. A process that has
    just started owns no runs, so anything still marked running is by
    definition a leftover.
    """
    settings = get_settings()
    repaired = reconcile_interrupted_runs(ProjectStore(settings.projects_dir))
    if repaired:
        print(
            "[startup] cleared interrupted run state for "
            f"{len(repaired)} project(s): {', '.join(repaired)}",
            file=sys.stderr,
        )
    yield


app = FastAPI(
    title="Prompt-To-Native-VR orchestrator",
    version="0.1.0",
    description=(
        "Local control plane for the prompt-to-native-VR pipeline."
        " Runs Claude planning, Imagen/Gemini image generation, Tripo"
        " image-to-3D, deterministic scene compile, and CMake builds."
    ),
    lifespan=lifespan,
)

_settings = get_settings()

app.add_middleware(
    CORSMiddleware,
    allow_origins=[f"http://127.0.0.1:{_settings.web_port}", f"http://localhost:{_settings.web_port}"],
    allow_credentials=False,
    allow_methods=["*"],
    allow_headers=["*"],
)

app.include_router(router)


@app.get("/", include_in_schema=False)
def root():
    return {
        "service": "ptv-orchestrator",
        "docs": "/docs",
        "health": "/api/health",
    }


def main() -> None:
    import uvicorn

    s = get_settings()
    uvicorn.run(
        "ptv_orchestrator.main:app",
        host=s.orchestrator_host,
        port=s.orchestrator_port,
        reload=False,
    )


if __name__ == "__main__":
    main()
