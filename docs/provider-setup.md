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

## Google (Gemini + Imagen)

- Get an API key at <https://aistudio.google.com/app/apikey>.
- Paste into `.env` as `GOOGLE_API_KEY` (the SDK also accepts
  `GEMINI_API_KEY`; both work).
- Default models:
  - `IMAGEN_MODEL=imagen-4.0-generate-001` (primary).
  - `GEMINI_IMAGE_MODEL=gemini-2.5-flash-image` (fallback).
- Python SDK package: `google-genai`. Installed automatically when you
  `pip install -e packages/provider-clients`.

## Tripo (image-to-3D)

- Get an API key at <https://platform.tripo3d.ai> (sign up, create a key).
- Paste into `.env` as `TRIPO_API_KEY`.
- Python SDK package: `tripo3d`. Installed automatically when you
  `pip install -e packages/provider-clients`.
- The wrapper **always** sends:
  - `texture=True`,
  - `pbr=True`,
  - `texture_alignment="original_image"`,
  - `texture_quality="detailed"` (overridable per asset),
  - `model_version` = `TRIPO_MODEL_VERSION` env var (SDK default if empty).

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
| MechanicPlan | Anthropic `claude-opus-4-7` | OpenAI `gpt-5` | JSON schema |
| SceneSpec (per scene) | Anthropic `claude-opus-4-7` | OpenAI `gpt-5` | JSON schema |
| Reference image (primary) | Google `imagen-4.0-generate-001` | Google `gemini-2.5-flash-image` | — |
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

### `Imagen request failed: ... negative_prompt ...`

`negative_prompt` is a Vertex-AI-only knob on `imagen-4.0-*`. The Gemini
Developer API (what `GOOGLE_API_KEY` typically points at) rejects it. The
client automatically retries without it on 400s, but if you supply a
`negative_prompt` in an `AssetRef` and see this in the logs, just leave the
field blank.

### Claude says nothing, OpenAI is used instead

If `ANTHROPIC_API_KEY` is unset OR invalid, you'll see
`Claude unavailable (...); falling back to OpenAI.` in the orchestrator
logs. That's by design. Once a valid Claude key is present the next run
returns to Claude automatically.

