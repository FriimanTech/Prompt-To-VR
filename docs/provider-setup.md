# Provider setup

The three providers used by the pipeline.

## Anthropic (Claude)

- Get an API key at <https://console.anthropic.com> → *API Keys*.
- Paste into `.env` as `ANTHROPIC_API_KEY`.
- Default models (configurable in `.env`):
  - `CLAUDE_HARD_MODEL=claude-opus-4-7` (BuildSpec).
  - `CLAUDE_LIGHT_MODEL=claude-sonnet-4-6` (AssetManifest).
- Structured outputs are GA on the Claude API. The wrapper passes the schema
  via `output_config.format = {"type": "json_schema", "schema": ...}`. No
  beta headers are required.

## OpenAI / ChatGPT (planning + codegen + fallback)

- **Required** for NPC plan, MechanicPlan, SceneSpec, code generation, and world
  generation (OpenAI Agents SDK). Also used as an automatic fallback when Claude
  is unavailable (no key, rate-limited, transient API error, etc.).
- Get an API key at <https://platform.openai.com/api-keys>.
- Paste into `.env` as `OPENAI_API_KEY`.
- **Every** OpenAI call in the pipeline runs `gpt-5.6-sol` (~1.05M context,
  128K max output). What varies per stage is `reasoning.effort`, which 5.6
  accepts as `none | low | medium (default) | high | xhigh | max`. Note that
  `minimal` was valid on gpt-5 and is **gone** in 5.6 — the orchestrator
  rejects it at startup rather than letting it 400 mid-run.
- Claude-fallback tiers (configurable in `.env`). Both are the same model, so
  effort is what distinguishes them:
  - `OPENAI_HARD_MODEL=gpt-5.6-sol` +
    `OPENAI_HARD_REASONING_EFFORT=high` — the Opus-equivalent path
    (BuildSpec, the master planner).
  - `OPENAI_LIGHT_MODEL=gpt-5.6-sol` +
    `OPENAI_LIGHT_REASONING_EFFORT=medium` — the Sonnet-equivalent path
    (AssetManifest). `medium` rather than `low` deliberately: this is the
    call that decides which assets get rigged and animated, at roughly 30
    and 20 Tripo credits each.
- Per-stage agent knobs (`NPC_PLAN_MODEL`, `MECHANIC_PLAN_MODEL`,
  `SCENE_SPEC_MODEL`, `WORLD_MODEL`, `CODEGEN_MODEL` and their
  `*_REASONING_EFFORT` partners) are documented in `.env.example`. Codegen
  has its own rather than borrowing the fallback tier: it always runs on
  OpenAI, so tuning the Claude fallback should not silently retune C++
  generation.
- Behaviour: every stage calls the shared `LlmClient`, which tries Claude
  first; on a missing key or a `ProviderCallError`, it transparently retries
  the same request against OpenAI. Model ids are translated (`claude-opus-4-7`
  → `OPENAI_HARD_MODEL`, `claude-sonnet-4-6` → `OPENAI_LIGHT_MODEL`).
- Structured outputs: the OpenAI wrapper uses `response_format={"type":
  "json_object"}` with the JSON Schema embedded in the system prompt; the
  orchestrator re-validates the result with the same Pydantic model Claude
  would have been validated against, so artifact integrity is preserved.

## Google (Gemini image generation)

- Get an API key at <https://aistudio.google.com/app/apikey>.
- Paste into `.env` as `GOOGLE_API_KEY` (the SDK also accepts
  `GEMINI_API_KEY`; both work).
- Default model:
  - `GEMINI_IMAGE_MODEL=gemini-3.1-flash-image` (text-to-image via
    `generate_content` with `response_modalities=["IMAGE"]`).
- Imagen 4 endpoints (`imagen-4.0-generate-001`, `imagen-4.0-ultra-*`,
  `imagen-4.0-fast-*`) are discontinued after **2026-08-17**. If your
  `.env` still has `IMAGEN_MODEL=...`, remove it and set
  `GEMINI_IMAGE_MODEL=gemini-3.1-flash-image` instead.
- Python SDK package: `google-genai`. Installed automatically when you
  `pip install -e packages/provider-clients`.

## Tripo (image-to-3D)

- Get an API key at <https://developers.tripo3d.ai> (sign up, create a key).
- Paste into `.env` as `TRIPO_API_KEY`.
- Uses the **V3 API** (`https://openapi.tripo3d.ai/v3`) over plain HTTP via
  `httpx`. The official `tripo3d` SDK is deliberately **not** used: its
  primary base URL is still `api.tripo3d.ai/v2/openapi`, so generation calls
  would keep running on V2, which is retired **2026-11-01** (no feature
  updates or support after 2026-10-01).
- China-mainland accounts: set `TRIPO_BASE_URL=https://openapi.tripo3d.com/v3`.
  A key only validates in its own region, so a 401 on the balance check
  usually means the wrong base URL rather than a bad key.
- Endpoints used: `POST /v3/files` (upload the reference image),
  `POST /v3/generation/image-to-model` (or `text-to-model`),
  `GET /v3/tasks/{task_id}` (poll), and `GET /v3/account/balance` (stage
  preflight).
- The wrapper **always** sends, by default:
  - `model="v3.1-20260211"` — Tripo's latest image-to-3D checkpoint
    (highest fidelity). Override with the `TRIPO_MODEL_VERSION` env var.
    V3 **requires** this field and has no server-side default, so leaving
    the env var blank resolves to the constant above rather than omitting
    it. Accepted values: `v3.1-20260211`, `v3.0-20250812`, `v2.5-20250123`,
    `P1-20260311` — anything else is rejected at startup.
  - `texture=True` — colored output.
  - `pbr=True` — emit BaseColor / Metallic / Roughness / Normal maps.
  - `texture_alignment="original_image"` — UVs match the reference.
  - `texture_quality="standard"` — balanced texture maps
    (overridable per asset; opt up to `"detailed"` when needed).
  - `geometry_quality="standard"` — balanced mesh reconstruction
    (opt up to `"detailed"` / Ultra Mode when needed).
    Omitted for `v2.5-20250123` (predates the parameter) and for
    `P1-20260311` (which rejects it outright, even as an explicit `false`).
  - `enable_image_autofix=True` — Tripo pre-cleans the reference image
    (background removal, contrast, framing) before generation.
  - `auto_size=True` — approximate real-world metric scale.
  - `orientation="align_image"` (image-to-3D only) — output mesh faces
    the camera in the same way as the reference render.
  - `texture_alignment` is image-to-3D only; Tripo fixes it server-side for
    text-to-3D, so the wrapper does not send it there.
- Uploads are self-paced to roughly one every 6.5s: V3 tightened the upload
  quota to about 10/min (V2 allowed ~10/s). This costs nothing in practice
  because the stage is sequential and each generation already takes 30-90s.
- Rate-limit (`1007`), concurrency (`2000`), 429 and 5xx responses are
  retried with backoff. Malformed requests (`1004`), moderation rejections
  (`2008`) and insufficient credit (`2010`) fail immediately with a hint.

### Auto-rigging (`asset_rigging`)

Two endpoints, always in this order:

| Call | Cost | Returns |
|---|---|---|
| `POST /v3/animations/rig-check` | free | `riggable`, `rig_type` |
| `POST /v3/animations/rig` | ~30 credits | the rigged GLB |

Both take a single `input` field — a generation `task_id`, a `file_token`, or
a public URL. This is **not** V2's `original_model_task_id`, and the rig
version field is `model`, not `model_version`; the migration notes in
`Helper_Folder/TRIPOV3_SKILL.md` were wrong about both and have been
corrected.

The `model` must match the rig type, because the two checkpoints are not
interchangeable and the wrong pairing produces a bad skeleton rather than an
error:

| `rig_type` | `model` |
|---|---|
| `biped` | `v1.0-20240301` (humanoid only; 90+ animation presets) |
| `quadruped`, `hexapod`, `octopod`, `avian`, `serpentine`, `aquatic` | `v2.5-20260210` |

`spec="tripo"` and `out_format="glb"` always. Tripo's native bone naming is
required because `/animations/retarget` rejects Mixamo-spec skeletons with
error 1004 ("does not support mixamo skeleton retarget"). Generated C++ plays
clips by semantic intent via `ISkeleton::play_clip`, so Mixamo joint names are
not needed for NPC animation.

Notice that every rig type describes a **whole creature**. Props, weapons,
vehicles and disembodied body parts are not riggable, which is exactly why the
free pre-check runs first: it turns "this was never going to work" from a
30-credit mistake into a no-op.

Which assets are candidates comes from `AssetItem.rigged`, but the authority
on that flag is the **`npc_plan` stage**, not the asset-manifest agent. The
manifest agent guesses from a style note; the NPC director reads the prompt's
actual intent and reconciles the manifest against it. A prompt that says the
ducks should just slide clears `rigged` and `animations` on the duck before
this stage ever sees it, which is the whole reason `npc_plan` runs first — a
duck that stays marked rigged costs a 30-credit rig plus 20 per clip for
animation nothing will ever play. See [npc-and-ai.md](npc-and-ai.md).

Results are recorded in `asset_rig.json` so a Resume never re-bills, and the
rigged mesh lands as `model_rigged.glb` beside the original so a re-run rigs
the source rather than an already-rigged mesh. Set
`PTV_TRIPO_RIGGING_ENABLED=false` to skip the stage entirely; assets then
render as static geometry and nothing downstream breaks.

### Animation retarget (`asset_animation`)

`POST /v3/animations/retarget`, billed **per animation** (~20 credits each,
at most 5 per call). `input` must be the `task_id` of a successful **rig**
task — unlike rig-check there is no upload fallback, so an asset rigged
before the pipeline recorded a `rig_task_id` has to be re-rigged first.

| Parameter | Value | Why |
|---|---|---|
| `animation` / `animations` | one, or a list | Mutually exclusive; the client picks based on count |
| `out_format` | `glb` | |
| `bake_animation` | `true` | Writes the motion into the file so nothing depends on Tripo at runtime |
| `export_with_geometry` | `true` | The runtime loads mesh, skin and clips from ONE GLB |
| `animate_in_place` | `true` | See below. Tripo defaults this to `false`, so it is always sent explicitly |

**Why in-place is forced.** A locomotion clip normally carries root motion:
the character's forward travel is baked into the root joint. Play that while
the engine is also driving the entity's `CTransform` and the two stack — the
character covers double the ground, then snaps back to the origin every time
the clip loops. With `animate_in_place: true` only the limbs cycle and
`CTransform` stays the single source of truth for position. The "enemy runs
on the spot" case uses the same flag; you simply do not move the entity.

**Presets are chosen from intents, not by hand.** Which ids exist depends on
the rig: `v1.0-20240301` (biped) has 90+ namespaced `preset:biped:*`,
`v2.5-20260210` biped has 11 un-namespaced `preset:*`, and each creature rig
has exactly one locomotion preset. `AssetItem.animations` therefore holds
semantic intents and `ptv_providers.resolve_animations` does the mapping from
the `rig_type` and `model` in `asset_rig.json`. For a creature, `walk` and
`run` resolve to the same preset — billed once, and the runtime plays it
faster for a run.

**Clip names come back from Tripo, so the stage reads them.** After download
it parses the GLB's JSON chunk for the `animations[].name` values that are
actually present and records intent → real name in `asset_animation.json`.
If a batch returns fewer clips than were paid for, that is logged loudly and
only the clips that exist are exposed; nothing downstream silently references
a clip that is not there.

Set `PTV_TRIPO_ANIMATION_ENABLED=false` to skip the stage, or
`PTV_TRIPO_ANIMATION_MAX_PER_ASSET` (default 4) to cap the bill.

## Uthana (humanoid rig with fingers + locomotion)

- Create an API key at https://uthana.com and paste it into `.env` as
  `UTHANA_API_KEY`. The client loads it with python-dotenv.
- Install the SDK in the venv: `uv pip install "uthana>=0.5.1"` (it is a
  dependency of `packages/provider-clients`).
- Billing is pay-as-you-go per generated second; downloads (including
  retargeting an existing motion onto another character) are free.
- `UTHANA_IDLE_MOTION_ID` (default `msf9nhQ4yyTJ`) names the existing idle
  motion every Uthana character is given.
- `UTHANA_ENABLED=false` routes every character to Tripo (the avatar then
  has no finger curls).

See [uthana.md](uthana.md) for the full flow: routing, auto-rig, the motion
naming convention and reuse, the shared idle, and the GLB merge.

## Verifying the keys

```powershell
curl http://127.0.0.1:8787/api/health
```

Example response:

```json
{
  "ok": true,
  "has_anthropic_key": true,
  "has_openai_key": true,
  "has_google_key": true,
  "has_tripo_key": true,
  "has_uthana_key": true,
  "cmake_generator": "Visual Studio 18 2026",
  "vcpkg_root": "C:\\src\\vcpkg"
}
```

The web control panel shows the same status as pills in the top-right header.
Pills are red with ✗ if a key is missing.

## What gets sent where

| Stage | Primary | Fallback (if Claude unavailable) | Structured? |
|---|---|---|---|
| BuildSpec | Anthropic `claude-opus-4-7` | OpenAI `gpt-5.6-sol` @ `high` | JSON schema |
| AssetManifest | Anthropic `claude-sonnet-4-6` | OpenAI `gpt-5.6-sol` @ `medium` | JSON schema |
| NPC plan | OpenAI Agents SDK `gpt-5.6-sol` @ `high` (no tools) | — (OpenAI-only) | JSON schema |
| MechanicPlan | OpenAI Agents SDK `gpt-5.6-sol` @ `high` (no tools) | — (OpenAI-only) | JSON schema |
| SceneSpec (per scene) | OpenAI Agents SDK `gpt-5.6-sol` @ `medium` (no tools) + validate-and-repair | — (OpenAI-only) | JSON schema |
| Reference image | Google `gemini-3.1-flash-image` | — | — |
| Image-to-3D | Tripo (default latest, configurable) | — | textured + PBR |
| Character routing | Tripo rig-check (free) + OpenAI Agents SDK `gpt-5.6-sol` @ `high` (reference images attached) | deterministic rules | JSON schema |
| Uthana auto-rig | Uthana `create_character` (fingers) | Tripo rig | — |
| Auto-rigging | Tripo rig-check + rig | — | — |
| Animation retarget | Tripo retarget | — | — |
| Uthana motion plan | OpenAI Agents SDK `gpt-5.6-sol` @ `medium` + code validation | deterministic defaults | JSON schema |
| Uthana animation | Uthana list / create_locomotion / rename / download | — | — |
| C++ modules (code_generation) | OpenAI Agents SDK `codegen_model` (default `gpt-5.6-sol`) @ `high` + `WebSearchTool` | — (OpenAI-only) | free-form, guarded by boundaries |
| World generation | OpenAI Agents SDK `world_model` (default `gpt-5.6-sol`) @ `high` + `WebSearchTool` | — (OpenAI-only) | C++ source, compile-verify |
| Engine promotion | — (deterministic index) | — | — |

The orchestrator never sends binary assets back to a provider; images go
Gemini → disk → Tripo, and C++ generation is prompt-only.

## Troubleshooting

### `openai.BadRequestError: 'temperature' does not support 0.0 with this model`

You are calling a **reasoning** model (the whole `gpt-5*` family including
`gpt-5.6-sol`, or the `o1`/`o3`/`o4` families). Those models reject
`temperature`, `top_p`, `presence_penalty`, `frequency_penalty`, and
`logprobs`. The bundled `OpenAIClient` detects this automatically and omits
the kwargs; if you still see the error, make sure you're on the current
build (pull the repo) or pin a non-reasoning model like
`OPENAI_HARD_MODEL=gpt-5-chat-latest` in your `.env`.

### `ProviderCallError: OpenAI response was truncated (finish_reason='length')`

Reasoning models spend tokens "thinking" before they emit the final JSON,
and `max_completion_tokens` caps the TOTAL of reasoning plus visible output.
The default cap is 96K with an auto-retry up to 128K (exactly
`gpt-5.6-sol`'s max output). If a complex BuildSpec or long CodeTask still
hits it, either:

* raise `OPENAI_MAX_TOKENS` (or pass `max_tokens=...` on the call), or
* lower the tier's reasoning effort (e.g.
  `OPENAI_HARD_REASONING_EFFORT=medium`) so fewer of the 128K tokens are
  spent on invisible reasoning.

### `negative_prompt` on reference images

Asset manifests may carry a `negative_prompt` field (a holdover from the old
Imagen path). Gemini image models do not accept a separate negative-prompt
parameter. The client folds it into the text prompt as
``Avoid in the image: …`` instead.

### Claude says nothing, OpenAI is used instead

If `ANTHROPIC_API_KEY` is unset OR invalid, you'll see
`Claude unavailable (...); falling back to OpenAI.` in the orchestrator
logs. That's by design. Once a valid Claude key is present the next run
returns to Claude automatically.

