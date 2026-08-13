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
    # Leave empty to use the client's default (v3.1-20260211, Tripo's latest
    # image-to-3D checkpoint). V3 has no server-side default, so an empty
    # value here resolves to that constant rather than omitting the field.
    tripo_model_version: str | None = None
    # Tripo V3 base URL. Defaults to the international endpoint; set to
    # https://openapi.tripo3d.com/v3 for a China-mainland account (a key only
    # validates in its own region).
    tripo_base_url: str | None = None
    # Auto-rigging (Stage 6b). rig-check is free but each rig is ~30 credits,
    # so this exists to turn the whole stage off for a cost-sensitive run
    # without editing the pipeline. Assets stay un-rigged and render as static
    # geometry; nothing downstream breaks.
    tripo_rigging_enabled: bool = True
    # Animation retarget (Stage 6c). Billed PER ANIMATION (~20 credits each),
    # so the cap matters: four clips on six enemies is already ~480 credits.
    # Disabling leaves rigged assets as static skinned meshes.
    tripo_animation_enabled: bool = True
    tripo_animation_max_per_asset: int = 4

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

    # After all CodeTasks are written, verify the generated C++ actually
    # COMPILES by building the ``ptv_runtime`` target (which globs in every
    # generated mechanic). On failure, the compiler errors are fed back to
    # the owning task's agent to regenerate the file, then the stage rebuilds
    # -- up to ``codegen_max_repair_rounds`` times. This mirrors the
    # world-generation compile-and-repair loop and stops a mechanic that
    # merely passes include validation but does not compile from sinking the
    # later cmake_build stage. Set to False to skip verification (e.g. on a
    # machine without the C++ toolchain).
    codegen_verify_compile: bool = True
    # How many compile+repair rounds to attempt before giving up and failing
    # the stage (0 = verify once, never repair). Each round can repair several
    # tasks in parallel; 3 gives the exe-link verify room to heal multiple
    # independent defects (compile errors, cross-task duplicate symbols, and
    # signature mismatches) in a single code-generation run.
    codegen_max_repair_rounds: int = Field(default=3, ge=0, le=10)

    # --- MechanicPlan stage (Stage 3, agentic) ---------------------------
    # The mechanic planner is an OpenAI Agents SDK agent with no tools
    # (structured MechanicPlan output only). It classifies each BuildSpec
    # mechanic (builtin / builtin+config / builtin+config_code_edit /
    # generate), assigns controller bindings, and emits nested CodeTasks.
    mechanic_plan_model: str = "gpt-5.5"
    # reasoning.effort: one of none|minimal|low|medium|high|xhigh (model-dep).
    mechanic_plan_reasoning_effort: str = "medium"
    # Turn budget for the single planning run.
    mechanic_plan_agent_max_turns: int = Field(default=40, ge=4, le=200)

    # --- NPC plan stage (what every character is supposed to do) ---------
    # Runs before rigging, because "does this character need animation at
    # all" is the most expensive question in the pipeline: a rig is ~30
    # credits and each clip another ~20. Also feeds the mechanic planner,
    # the scene composer and codegen so all three agree on which characters
    # are combatants and what they can do.
    npc_plan_model: str = "gpt-5.5"
    npc_plan_reasoning_effort: str = "high"
    npc_plan_agent_max_turns: int = Field(default=30, ge=4, le=200)

    # --- World-generation stage (agentic VR environment) -----------------
    # A single "world architect" agent (OpenAI Agents SDK) authors ALL the
    # C++ for the VR environment, bounded to the IWorldBuilder surface and
    # compiled + run offline in ptv_worldgen. World gen runs once per project
    # and the C++ must compile against the IWorldBuilder API first try, so we
    # default to a high reasoning tier (raise to ``xhigh`` for max quality).
    world_model: str = "gpt-5.5"
    # reasoning.effort: one of none|minimal|low|medium|high|xhigh (model-dep).
    # NOTE: a ``WORLD_REASONING_EFFORT`` entry in .env overrides this default;
    # keep it at ``high`` or ``xhigh`` -- ``medium`` risks first-try compile
    # failures in the generated IWorldBuilder C++.
    world_reasoning_effort: str = "high"
    # How many compile-and-repair rounds the stage will attempt before
    # falling back to the deterministic world so the project still boots.
    world_max_repair_rounds: int = Field(default=6, ge=1, le=20)
    # Turn budget for one Agents SDK run (the agent loop). Generous so the
    # architect can iterate over each part; not unlimited to bound cost.
    world_agent_max_turns: int = Field(default=80, ge=4, le=1000)

    # --- SceneSpec stage (Stage 4, agentic) -----------------------------
    # SceneSpec composes one scene per ``BuildSpec.scenes`` entry via an
    # OpenAI Agents SDK agent (gpt-5.5, NO tools) with a deterministic
    # validate-and-repair loop, mirroring the mechanic_plan / world stages.
    # A long user prompt can produce 10-15+ scenes, so we fan out across a
    # ``ThreadPoolExecutor`` capped by this knob (default 3 mirrors the
    # codegen worker default). Each worker runs its own agent, so this many
    # gpt-5.5 SceneSpec agents may be in flight at once.
    scene_spec_max_parallel_workers: int = Field(default=3, ge=1, le=16)
    scene_spec_model: str = "gpt-5.5"
    # reasoning.effort: one of none|minimal|low|medium|high|xhigh (model-dep).
    # SceneSpec is a mapping/composition task (assets+mechanics -> entities),
    # not deep reasoning, so ``medium`` completes well under the request
    # timeout. Lower to ``low`` for faster/cheaper runs on simple scenes.
    scene_spec_reasoning_effort: str = "medium"
    # Turn budget for one Agents SDK run (the agent loop).
    scene_spec_agent_max_turns: int = Field(default=30, ge=4, le=200)
    # How many validate-and-repair rounds per scene before giving up and
    # failing the scene (0 = compose once, never repair).
    scene_spec_max_repair_rounds: int = Field(default=2, ge=0, le=6)

    # --- Build toolchain -------------------------------------------------
    vcpkg_root: str | None = None
    cmake_generator: str = "Visual Studio 18 2026"
    cmake_generator_platform: str = "x64"
    cmake_build_config: str = "Release"

    # --- OpenXR / Vulkan hints ------------------------------------------
    optional_vulkan_sdk_path: str | None = None
    optional_openxr_runtime_hint: str | None = None

    # --- Quest / Android build toolchain --------------------------------
    # The Quest target reuses the user's Android Studio toolchain via the
    # standard env vars (ANDROID_HOME, ANDROID_NDK_HOME, JAVA_HOME) and PATH
    # (adb, cmdline-tools). These knobs let those be overridden if needed.
    android_sdk_root: str | None = None  # defaults to $ANDROID_HOME
    android_ndk_home: str | None = None  # defaults to $ANDROID_NDK_HOME
    adb_path: str | None = None  # defaults to `adb` on PATH
    # APK identity + SDK levels. Package name is "<prefix>.<sanitized_id>".
    android_package_prefix: str = "com.prompttovr"
    android_min_sdk: int = 32
    android_target_sdk: int = 34
    android_compile_sdk: int = 34
    # Khronos OpenXR Android loader (Meta requires >= 1.0.34).
    android_openxr_loader_version: str = "1.0.34"
    # Pin the NDK so externalNativeBuild is deterministic. Defaults to the NDK
    # installed with the documented Android Studio setup; override if a
    # different NDK is on the machine (must be installed under $ANDROID_HOME/ndk).
    android_ndk_version: str = "30.0.15729638"


_cached: Settings | None = None


def get_settings() -> Settings:
    global _cached
    if _cached is None:
        _cached = Settings()
        _cached.projects_dir.mkdir(parents=True, exist_ok=True)
    return _cached
