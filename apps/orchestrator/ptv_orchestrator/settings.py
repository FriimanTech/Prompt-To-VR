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
    # Reference images (Stage 5): Gemini native image generation. Imagen 4
    # endpoints are discontinued after 2026-08-17; Google recommends
    # ``gemini-3.1-flash-image`` via ``generate_content``.
    gemini_image_model: str = "gemini-3.1-flash-image"
    tripo_model_version: str | None = None

    # --- LLM output budgets ---------------------------------------------
    # Caps the visible output (Claude) and the COMBINED reasoning +
    # visible output (OpenAI gpt-5 / o-series) per call. Pipeline stages
    # that produce large structured artifacts (BuildSpec, MechanicPlan,
    # SceneSpec, CodeTask) rely on these being generous enough that a
    # 900+ line user prompt does not silently truncate. The OpenAI
    # client also auto-retries with a doubled budget on
    # ``finish_reason='length'`` up to ``openai_max_tokens_ceiling``
    # (default 128K, the per-call max for ``gpt-5``).
    openai_max_tokens: int = Field(default=96_000, ge=4_000, le=200_000)
    openai_max_tokens_ceiling: int = Field(
        default=128_000, ge=4_000, le=200_000
    )
    claude_max_tokens: int = Field(default=32_000, ge=4_000, le=200_000)

    # Per-request HTTP timeout for the LLM providers, in seconds. The
    # OpenAI / Anthropic SDK defaults are 10 minutes which is enough to
    # make a stalled scene_spec / mechanic_plan call appear hung
    # indefinitely. We cap at 5 minutes by default; a complex gpt-5
    # reasoning call against a large schema finishes well under that
    # budget. Tune via the matching ``*_REQUEST_TIMEOUT_SECONDS`` env
    # variables when running behind a slow proxy.
    openai_request_timeout_seconds: float = Field(
        default=300.0, ge=30.0, le=1800.0
    )
    claude_request_timeout_seconds: float = Field(
        default=300.0, ge=30.0, le=1800.0
    )

    # --- Code-generation stage (Stage 8, multi-agent) --------------------
    # How many OpenAI Agents SDK sessions may be in flight at the same
    # time. Each worker claims one ``CodeTask`` from the shared
    # ``codegen_board.json`` ledger, runs one ``Runner.run_sync``
    # session (with the hosted ``WebSearchTool`` enabled, NO cap on
    # search calls), writes the multi-file result, and claims the next
    # eligible task. Default 3 matches Part B of the multi-agent
    # update spec.
    codegen_max_parallel_agents: int = Field(default=3, ge=1, le=16)

    # --- World-generation stage (agentic VR environment) -----------------
    # A single "world architect" agent (OpenAI Agents SDK) authors ALL the
    # C++ for the VR environment, bounded to the IWorldBuilder surface and
    # compiled + run offline in ptv_worldgen. Defaults pin the highest-tier
    # model + reasoning per the current SDK (gpt-5.5, reasoning effort xhigh).
    world_model: str = "gpt-5.5"
    # reasoning.effort: one of none|minimal|low|medium|high|xhigh (model-dep).
    world_reasoning_effort: str = "xhigh"
    # How many compile-and-repair rounds the stage will attempt before
    # falling back to the deterministic world so the project still boots.
    world_max_repair_rounds: int = Field(default=6, ge=1, le=20)
    # Turn budget for one Agents SDK run (the agent loop). Generous so the
    # architect can iterate over each part; not unlimited to bound cost.
    world_agent_max_turns: int = Field(default=80, ge=4, le=1000)

    # --- SceneSpec stage (Stage 4) --------------------------------------
    # SceneSpec emits one structured-output LLM call per
    # ``BuildSpec.scenes`` entry. A long user prompt can produce 10-15+
    # scenes, so running them serially makes the stage appear hung.
    # We fan out across a ``ThreadPoolExecutor`` capped by this knob
    # (default 3 mirrors the codegen worker default).
    scene_spec_max_parallel_workers: int = Field(default=3, ge=1, le=16)

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
