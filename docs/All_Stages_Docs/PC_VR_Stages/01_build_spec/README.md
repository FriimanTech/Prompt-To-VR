# Stage 1: BuildSpec

> **Pipeline position:** start → before `asset_manifest`  
> **Target:** shared (PC VR and Quest)  
> **Primary code:** [`apps/orchestrator/ptv_orchestrator/pipeline/stage_build_spec.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/stage_build_spec.py)  
> **Primary artifacts:** `projects/<project_id>/build_spec.json`

## Main stage summary

### What this stage is

The **BuildSpec stage** turns your raw game prompt (plain text) into the first structured JSON plan for the entire project. A **BuildSpec** is a Pydantic-validated document that describes the game's title, scenes, mechanics list, target device, and other high-level decisions.

### Why it exists

Every later stage reads **validated JSON**, not the original prompt. That prevents drift: if the asset planner and the scene composer both re-read the same prose independently, they can disagree. BuildSpec is the single source of truth for "what game are we building?"

This is called **schema-first development**: you define a data shape once (`packages/schemas`), then every stage consumes and produces artifacts that match it.

### Inputs (what enters)

| Input | Location |
|-------|----------|
| User prompt | `projects/<id>/prompt.txt` (via `ProjectState.prompt`) |
| Target device | `pcvr` or `quest`, chosen at project creation |

### Outputs (what it produces)

| Output | Location |
|--------|----------|
| BuildSpec JSON | `projects/<id>/build_spec.json` |
| Stage record | Appended to `project_state.json` |

### Where output goes next

`asset_manifest`, `npc_plan`, `mechanic_plan`, `scene_spec`, `world_generation`, and `scene_compile` all read `build_spec.json`.

### Key concepts and terms

- **Schema** — A formal description of allowed JSON fields and types. Pydantic models in `packages/schemas/ptv_schemas/` are the source of truth; JSON Schema files are exported for LLM structured output.
- **Structured output** — The LLM must return JSON matching the schema, not free-form text. Claude receives the schema and validates the response before saving.
- **`target_device`** — Either `pcvr` (Windows `.exe` over Quest Link) or `quest` (standalone Android APK). **Not** an LLM decision: the value comes from the New Project toggle on `ProjectState.target_device`, and this stage overwrites whatever the model returned with it. The system prompt names the target so the rest of the plan is budgeted for the right GPU. Stage 16 branches on it.
- **`prompt_id`** — A short slug derived from the prompt text (e.g. `green-lantern-vr`). Used to namespace generated files.

### End-to-end flow (plain language)

1. Orchestrator loads the project prompt and marks stage `build_spec` as running.
2. It calls **Claude Opus** (`claude-opus-4-7`) via `LlmClient.generate_json()` with the BuildSpec JSON schema and system prompt `BUILD_SPEC_SYSTEM`.
3. The model returns JSON; Pydantic validates it into a `BuildSpec` object.
4. The artifact is saved to disk and the stage succeeds.
5. If Claude is unavailable, `LlmClient` transparently falls back to OpenAI.

---

## Detailed stage deep dive

### How the orchestrator invokes this stage

`orchestrator.run_pipeline()` walks `_SHARED_STAGES` starting here. It calls `stage_build_spec.run(ctx)` where `ctx` is a `PipelineContext` holding `project_id`, `settings`, `ProjectStore`, and the SSE event bus.

### Exact execution trace (`stage_build_spec.run`)

| Step | Code action | Side effect |
|------|-------------|-------------|
| 1 | `ctx.emit(build_spec, "running", …)` | UI: stage 1 active |
| 2 | `load_state()` + `record_stage(running)` | `project_state.json` gets new `StageRecord` |
| 3 | `BuildSpec.model_json_schema(by_alias=True)` | JSON Schema for LLM output |
| 4 | `make_llm(settings)` | Anthropic client, or OpenAI if Claude unavailable |
| 5 | Build user string: verbatim `state.prompt` + `prompt_id` hint | `_prompt_id_from_text()` slugifies prompt (max 48 chars) |
| 6 | `generate_json(..., system=BUILD_SPEC_SYSTEM)` | Single API call — **not** Agents SDK |
| 7 | `BuildSpec.model_validate(resp.data)` | Hard fail on schema mismatch |
| 8 | `ctx.build_spec = spec` | In-memory cache for same pipeline run |
| 9 | `save_build_spec()` | Writes `projects/<id>/build_spec.json` |
| 10 | `record_stage(succeeded)` + emit title | Returns `StageResult` |

No repair loop, no parallelism, no disk skip — every successful run re-calls the LLM and overwrites the JSON file.

### Artifact I/O map

| Direction | Path | Notes |
|-----------|------|-------|
| Read | `projects/<id>/prompt.txt` | Via `ProjectState.prompt` at create time |
| Read | `project_state.json` | Prior stage records |
| Write | `build_spec.json` | Canonical game plan artifact |
| Write | `project_state.json` | Appended `StageRecord` |

### SSE events emitted

| Status | When |
|--------|------|
| `running` | Stage start |
| `succeeded` | After save; message = game `title` |
| *(via orchestrator on exception)* | `failed` |

### `LlmClient` and structured output

`pipeline/llm.py` wraps Anthropic + OpenAI. BuildSpec uses the **hard** model (`settings.claude_hard_model` → `claude-opus-4-7`). The model must return JSON matching the exported Pydantic schema — not markdown, not prose.

If the Anthropic key is missing or the API errors, the client transparently falls back to OpenAI (`openai_hard_model`, `gpt-5.6-sol` at `openai_hard_reasoning_effort` = `high`). From this stage's perspective both paths look identical: `generate_json()` → dict → `model_validate()`.

### BuildSpec fields and who reads them

| Field | Downstream consumer |
|-------|---------------------|
| `prompt_id` | npc_plan, namespaces in `engine/generated/` |
| `target_device` | Mirror of `ProjectState.target_device`; the orchestrator picks `_PCVR_TAIL` vs `_QUEST_TAIL` at stage 16 from the project state |
| `title` | scene.json, Windows exe name (`PTV_APP_NAME`), Quest app label |
| `scenes[]` | scene_spec (one composer per scene), scene_compile |
| `mechanics[]` | mechanic_plan (must align 1:1 by id) |

Full field reference: [`docs/prompt-schema.md`](../../../prompt-schema.md).

### Resume behavior

`resume_pipeline()` finds the first stage whose **latest** record is not `succeeded`. If that is `build_spec`, this stage runs again from step 1 — **full LLM cost**, file overwritten.

There is no partial progress inside this stage (unlike scene_spec or codegen).

### Failure modes (exact)

| Condition | Pipeline behavior |
|-----------|-------------------|
| Pydantic `ValidationError` | Exception → stage `failed`, pipeline stops |
| `ProviderConfigError` | Missing keys or `dry_run` in tests |
| API timeout / HTTP error | Same — caught by orchestrator, recorded on `current_stage` |

### Important files

| File | Role |
|------|------|
| `stage_build_spec.py` | Stage entry |
| `pipeline/llm.py` | Provider wrapper |
| `pipeline/prompts.py` | `BUILD_SPEC_SYSTEM` |
| `packages/schemas/ptv_schemas/build_spec.py` | Source schema |
| `storage.py` | `save_build_spec()`, `record_stage()` |

### Settings

| Setting | Default |
|---------|---------|
| `claude_hard_model` | `claude-opus-4-7` |
| `anthropic_api_key` | env |
| `openai_api_key` | fallback env |

### ProjectStore integration

`save_build_spec()` uses `write_model()` which validates again, writes pretty JSON to `build_spec.json`, and returns repo-relative artifact path for the stage record. `ctx.build_spec` caches in memory for same pipeline invocation.

### What BuildSpec does NOT do

Does not list Tripo assets (stage 2), assign bindings (stage 4), or place entities (stage 5). It only establishes the skeleton other planners fill in.

---

## Extend this folder

Suggested follow-on docs (not written yet):

- `artifacts.md` — every BuildSpec field and where it is consumed
- `schemas.md` — Pydantic model walkthrough
- `failure-modes.md` — validation errors and resume behavior

## See also

- [`docs/prompt-schema.md`](../../../prompt-schema.md) — BuildSpec field reference
- [`docs/architecture.md`](../../../architecture.md) — schema-first pipeline overview
- [`docs/overview.md`](../../../overview.md) — high-level project map (partially stale on stage list)
