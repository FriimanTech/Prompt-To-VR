"""Configuration loaded from .env or the environment."""

from __future__ import annotations

from pathlib import Path

from pydantic import Field
from pydantic_settings import BaseSettings, SettingsConfigDict


REPO_ROOT = Path(__file__).resolve().parents[3]


class Settings(BaseSettings):
    model_config = SettingsConfigDict(
        env_file=str(REPO_ROOT / ".env"),
        env_file_encoding="utf-8",
        extra="ignore",
    )

    # --- Server ----------------------------------------------------------
    orchestrator_host: str = "127.0.0.1"
    orchestrator_port: int = 8787
    web_port: int = 5173

    # --- Repo paths ------------------------------------------------------
    repo_root: Path = REPO_ROOT
    projects_dir: Path = REPO_ROOT / "projects"
    engine_dir: Path = REPO_ROOT / "engine"
    samples_dir: Path = REPO_ROOT / "samples"

    # --- Provider keys ---------------------------------------------------
    anthropic_api_key: str | None = None
    # ChatGPT fallback used automatically when Claude is unavailable.
    openai_api_key: str | None = None
    google_api_key: str | None = None
    tripo_api_key: str | None = None

    # --- Provider models -------------------------------------------------
    claude_hard_model: str = "claude-opus-4-7"
    claude_light_model: str = "claude-sonnet-4-6"
    openai_hard_model: str = "gpt-5"
    openai_light_model: str = "gpt-5-mini"
    imagen_model: str = "imagen-4.0-generate-001"
    gemini_image_model: str = "gemini-2.5-flash-image"
    tripo_model_version: str | None = None

    # --- Code-generation stage (Stage 8, multi-agent) --------------------
    # How many OpenAI Agents SDK sessions may be in flight at the same
    # time. Each worker claims one ``CodeTask`` from the shared
    # ``codegen_board.json`` ledger, runs one ``Runner.run_sync``
    # session (with the hosted ``WebSearchTool`` enabled, NO cap on
    # search calls), writes the multi-file result, and claims the next
    # eligible task. Default 3 matches Part B of the multi-agent
    # update spec.
    codegen_max_parallel_agents: int = Field(default=3, ge=1, le=16)

    # --- Build toolchain -------------------------------------------------
    vcpkg_root: str | None = None
    cmake_generator: str = "Visual Studio 18 2026"
    cmake_generator_platform: str = "x64"
    cmake_build_config: str = "Release"

    # --- OpenXR / Vulkan hints ------------------------------------------
    optional_vulkan_sdk_path: str | None = None
    optional_openxr_runtime_hint: str | None = None


_cached: Settings | None = None


def get_settings() -> Settings:
    global _cached
    if _cached is None:
        _cached = Settings()
        _cached.projects_dir.mkdir(parents=True, exist_ok=True)
    return _cached
