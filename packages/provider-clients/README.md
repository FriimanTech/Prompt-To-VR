# ptv-provider-clients

Thin, verified wrappers around the three external providers used by the
Prompt-To-Native-VR pipeline:

| Provider | Wrapper | Default model / version |
|---|---|---|
| Anthropic | `AnthropicClient` | `claude-opus-4-7` / `claude-sonnet-4-6` |
| Google GenAI | `GoogleImageClient` | `gemini-3.1-flash-image` via `generate_content` |
| Tripo | `TripoClient` | defaults to the SDK's latest `model_version`; always textured + PBR + image-aligned |

All three wrappers are safe to import even if the corresponding SDK is not
installed; the `requires()` helper on each client lazy-imports the SDK and
raises a clear error when first used.

The clients are deliberately thin. Anything pipeline-related (retries,
artifact persistence, schema validation) lives in the orchestrator.
