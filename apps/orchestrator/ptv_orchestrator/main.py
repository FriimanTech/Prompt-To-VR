"""FastAPI app entrypoint."""

from __future__ import annotations

from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware

from .api.routes import router
from .settings import get_settings

app = FastAPI(
    title="Prompt-To-Native-VR orchestrator",
    version="0.1.0",
    description=(
        "Local control plane for the prompt-to-native-VR pipeline."
        " Runs Claude planning, Imagen/Gemini image generation, Tripo"
        " image-to-3D, deterministic scene compile, and CMake builds."
    ),
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
