# Provider setup

The three providers used by the pipeline.

## Anthropic (Claude)

- Get an API key at <https://console.anthropic.com> → *API Keys*.
- Paste into `.env` as `ANTHROPIC_API_KEY`.
- Default models (configurable in `.env`):
  - `CLAUDE_HARD_MODEL=claude-opus-4-7` (hardest planning + hardest C++ gen).
  - `CLAUDE_LIGHT_MODEL=claude-sonnet-4-6` (routine transforms).
- Structured outputs are GA on the Claude API. The wrapper passes the schema
  via `output_config.format = {"type": "json_schema", "schema": ...}`. No
  beta headers are required.

## OpenAI / ChatGPT (automatic fallback)

- Optional. Only needed if you want the pipeline to keep working when Claude
  is unavailable (no key, rate-limited, transient API error, etc.).
- Get an API key at <https://platform.openai.com/api-keys>.
- Paste into `.env` as `OPENAI_API_KEY`.
- Default models (configurable in `.env`):
  - `OPENAI_HARD_MODEL=gpt-5` (hard planning + C++ generation).
  - `OPENAI_LIGHT_MODEL=gpt-5-mini` (routine transforms).
  - If you have Responses-API access to the newest coding model, set
    `OPENAI_HARD_MODEL=gpt-5.3-codex` (verify availability first; Responses
    API rollout was still in progress at the time of writing).
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
  - `texture_quality="detailed"` — high-resolution texture maps
    (overridable per asset).
  - `geometry_quality="detailed"` — "Ultra Mode" mesh reconstruction.
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
  "cmake_generator": "Visual Studio 18 2026",
  "vcpkg_root": "C:\\src\\vcpkg"
}
```

The web control panel shows the same status as pills in the top-right header.
Pills are red with ✗ if a key is missing.

## What gets sent where

| Stage | Primary | Fallback (if Claude unavailable) | Structured? |
|---|---|---|---|
| BuildSpec | Anthropic `claude-opus-4-7` | OpenAI `gpt-5` | JSON schema |
| AssetManifest | Anthropic `claude-sonnet-4-6` | OpenAI `gpt-5-mini` | JSON schema |
| MechanicPlan | OpenAI Agents SDK `gpt-5.5` (no tools) | — (OpenAI-only) | JSON schema |
| SceneSpec (per scene) | OpenAI Agents SDK `gpt-5.5` (no tools) + validate-and-repair | — (OpenAI-only) | JSON schema |
| Reference image | Google `gemini-3.1-flash-image` | — | — |
| Image-to-3D | Tripo (default latest, configurable) | — | textured + PBR |
| C++ module | Anthropic `claude-opus-4-7` | OpenAI `gpt-5` | free-form, guarded by boundaries |

The orchestrator never sends binary assets back to a provider; images go
Gemini → disk → Tripo, and C++ generation is prompt-only.

## Troubleshooting

### `openai.BadRequestError: 'temperature' does not support 0.0 with this model`

You are calling a **reasoning** model (`gpt-5`, `gpt-5-mini`, `gpt-5-nano`,
`gpt-5-codex`, or the `o1`/`o3`/`o4` families). Those models reject
`temperature`, `top_p`, `presence_penalty`, `frequency_penalty`, and
`logprobs`. The bundled `OpenAIClient` detects this automatically and omits
the kwargs; if you still see the error, make sure you're on the current
build (pull the repo) or pin a non-reasoning model like
`OPENAI_HARD_MODEL=gpt-5-chat-latest` in your `.env`.

### `ProviderCallError: OpenAI response was truncated (finish_reason='length')`

Reasoning models spend tokens "thinking" before they emit the final JSON.
The default cap is 16K tokens. If a complex BuildSpec or long CodeTask hits
it, either:

* raise `OpenAIConfig.max_tokens` (or pass `max_tokens=...` on the call), or
* set `OPENAI_HARD_MODEL=gpt-5-mini` (faster, cheaper, smaller reasoning budget needed for our schemas).

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

