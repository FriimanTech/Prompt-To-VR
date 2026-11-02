> **Shared content:** Identical to [PC_VR_Stages/01b_optimization_preferences](../../PC_VR_Stages/01b_optimization_preferences/README.md). Update both copies when this stage changes.

# Stage 1b: Optimization Preferences

> **Pipeline position:** after `build_spec` → before `asset_manifest`  
> **Target:** shared (PC VR and Quest)  
> **Primary code:** [`apps/orchestrator/ptv_orchestrator/pipeline/stage_optimization_preferences.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/stage_optimization_preferences.py)  
> **Primary artifacts:** `projects/<project_id>/optimization_preferences.json`

## Main stage summary

### What this stage is

Every Quest optimization is automatic by default. This stage reads any **explicit** optimization wording the user wrote — in the New Project page's collapsed **Custom Quest Optimizations** box or in the main prompt — and maps it onto a fixed set of user-controllable settings (`OptimizationPreferences`). Each recognised setting later becomes a **lock** the runtime quality governor never moves.

### Why it exists

Users sometimes ask for "no foveated rendering" or "120 Hz". Those requests must be honoured exactly, but they must not leak into gameplay planning, be invented from vague words ("high quality enemies"), or turn off memory safety. A separate, mostly deterministic stage keeps that boundary clear; the BuildSpec prompt is told to ignore such wording.

### Inputs

| Input | Source |
|-------|--------|
| Main prompt | `ProjectState.prompt` |
| Custom Quest Optimizations box | `ProjectState.custom_quest_optimizations` (empty by default) |

### Outputs

| Output | Location |
|--------|----------|
| `OptimizationPreferences` | `projects/<id>/optimization_preferences.json` |
| Stage record (message e.g. `"2 override(s), 1 unsupported"` or `"all optimizations automatic"`) | `project_state.json` |

### User-controllable settings

| Setting | Allowed values (`auto` = default, never a lock) |
|---------|------------------------------------------------|
| `ffr` | `off`, `fixed`, `max` (+ `level` 1 low / 2 medium / 3 high) |
| `resolution` | `off` (always native), `high` (up to 1.25x, never reduced) |
| `lod` | `detail`, `performance` |
| `texture_quality` | `high`, `memory_saver` |
| `draw_distance` | `far`, `performance` |
| `room_culling` | `off` |
| `anti_aliasing` | `high` (always 4x MSAA) |
| `frame_rate` | `90`, `120` (auto = 72 Hz) |

Effects of each value are in [`docs/quest-optimization.md`](../../../quest-optimization.md).

### Where output goes next

`asset_optimization` loads it (`load_preferences`), and when there are settings calls `apply_overrides(profile, prefs.to_locks())`, so each override lands in `performance.locks` (value, level, source, quote) of the scene's `PerformanceProfile`. `SceneLoader` / `QualityGovernor` respect the locks at runtime, and `quest_runtime_verify` checks they held. A missing or unreadable file means "everything automatic".

### Models, providers and effort

At most **one** structured-output call through `LlmClient`, passing `model=claude_light_model` (`claude-sonnet-4-6`); the OpenAI fallback is the light tier, `gpt-5.6-sol` at `openai_light_reasoning_effort` = `medium`. No call at all when the keyword gate finds no optimization wording.

### Failure behavior

This stage **never blocks the pipeline**. If the model call fails, it emits a warning ("every optimization stays automatic"), writes empty preferences (still with the deterministic unsupported detections) and succeeds. Overrides produce a warning, never a failure.

---

## Detailed stage deep dive

### Algorithm (`run` → `parse_preferences`)

1. **Keyword gate** (`keyword_gate`, regex `KEYWORDS`): foveat / FFR, resolution, render scale, supersampling, LOD / level of detail / model detail, texture, draw / render / view distance, cull, fps / Hz / frame rate / refresh rate, anti-alias / MSAA, graphics / visual / image / max quality, performance, optimise/optimize. No match in either text → `OptimizationPreferences(model_called=False)`, no client is even built.
2. **Cache**: results are cached in-process by SHA-256 of `prompt \0 box` (`_CACHE`), shared with the preview endpoint.
3. **Model call**: user message holds the box (source `custom_box`) and the main prompt (source `main_prompt`); system prompt `SYSTEM` lists the settings, allowed values and rules (gameplay or asset wording never counts; quotes must be verbatim; eye tracking, disabling compression and disabling memory protection go in `unsupported`). Output schema is the private `_RawPrefs` (`items[]` of setting/value/level/source/quote, `unsupported[]`).
4. **Validate** (`validate`), deterministic:
   - unknown settings and values (or `auto`) are dropped;
   - an item is kept only if its `quote` appears **verbatim** (whitespace/case-normalised) in the text it claims as its source;
   - `frame_rate` values are reduced to digits;
   - **clamps** (`_clamps`) recorded in `clamped`: resolution above 1.25x → `high`; frame rate above 120 Hz → `120`; texture 4096 / 4k / 8k → `high` (2048 max); MSAA 8x/16x → `high` (4x);
   - FFR `fixed` / `max` get a level (low 1, medium 2, high 3; default 2 for `fixed`, 1 for `max`);
   - **box wins**: a `main_prompt` item never replaces a `custom_box` item for the same setting;
   - unsupported entries are de-duplicated; a quote not found in its source is blanked.
5. **Deterministic unsupported detection** (`_merge_unsupported`), run with or without the model: eye tracking ("no eye tracking on Quest 3", closest: FFR), "no/disable texture compression" (closest: texture quality high, ASTC 4x4), "disable memory guard/protection/safety/limit" (never overridable).
6. Write `optimization_preferences.json`; emit one `info` line per setting (`summary_lines`), and the fixed `WARNING_TEXT` as a `warning` when anything overrides automatic.

### Preview endpoint

`POST /api/optimization-preferences/preview` (`api/routes.py`) runs the same `parse_preferences` (same cache) on the New Project page and returns `preview_payload`: the preferences, summary lines, `differs_from_auto` and the warning text. A preview error never fails the page, and Create stays enabled.

### Schema (`packages/schemas/ptv_schemas/optimization_preferences.py`)

`OptimizationPreferences`: one optional `PrefSetting` per setting (`value`, `level`, `source`, `quote`, `differs_from_auto`), `unsupported: [UnsupportedRequest]`, `clamped: [ClampedRequest]`, `model_called`. Helpers: `settings()`, `any_override()`, `to_locks()` → `PerformanceLocks`, `summary_lines()`. JSON schema: `packages/schemas/json/optimization_preferences.schema.json`.

### Important files

| File | Role |
|------|------|
| `pipeline/stage_optimization_preferences.py` | Gate, model call, validator, cache, `load_preferences`, `preview_payload` |
| `ptv_schemas/optimization_preferences.py` | Schema, `ALLOWED_VALUES`, `FFR_LEVELS` |
| `pipeline/stage_asset_optimize.py` | Applies the locks |
| `pipeline/prompts.py` | `_IGNORE_OPTIMIZATION_WORDING` keeps it out of BuildSpec |
| `api/routes.py` | Preview endpoint |

### Settings

No stage-specific settings. It uses `claude_light_model` (`CLAUDE_LIGHT_MODEL`, default `claude-sonnet-4-6`) and the shared fallback settings `openai_light_model` (`gpt-5.6-sol`) / `openai_light_reasoning_effort` (`medium`).

### Quest-specific notes

The settings only matter for Quest builds (the box is shown for the Quest target), but the stage is in the shared list and runs for every project; on PC VR it is normally a no-op with empty preferences.

## See also

- [`docs/quest-optimization.md`](../../../quest-optimization.md) — every setting's effect, hard limits, locks and the governor
- [`docs/rendering.md`](../../../rendering.md) — performance profile and LODs
- Next stage: [`02_asset_manifest`](../02_asset_manifest/README.md)
