"""GeminiVisionClient: structured image understanding for socket generation.

Verified October 2026 against the Gemini API "Image understanding" docs
(Interactions API) and the installed ``google-genai`` 2.18.1:

* ``client.interactions.create(model=..., input=[...], response_format=...,
  generation_config=...)`` -- keyword body arguments; the SDK signature is
  ``create(*, request=None, ..., **body)``.
* Inline images are ``{"type": "image", "data": <base64>, "mime_type":
  "image/png"}``; text parts are ``{"type": "text", "text": ...}``. The text
  prompt goes BEFORE the image (docs, "Tips and best practices").
* Structured output: ``response_format={"type": "text", "mime_type":
  "application/json", "schema": <JSON schema>}``; the answer is
  ``interaction.output_text``.
* Detection boxes are ``box_2d = [ymin, xmin, ymax, xmax]`` normalised to
  0-1000; segmentation masks are polygons of ``[x, y]`` normalised to 0-1000.
  For segmentation the docs recommend ``thinking_level: "minimal"``, but
  ``gemini-3.8-flash`` rejects ``minimal`` with a 400 (measured 2026-10-30),
  so the client steps up to the lowest level the model accepts.
* Inline data caps one request at 20 MB; a 768 px PNG is well under that.
* Model: ``gemini-3.8-flash`` (stable; models page, 2026-10-28).
"""

from __future__ import annotations

import base64
import json
import os
import time
from dataclasses import dataclass
from typing import Any, Mapping, Optional, Sequence

from .errors import ProviderCallError, ProviderConfigError

DEFAULT_GEMINI_VISION_MODEL = "gemini-3.8-flash"

#: Next level up when a model rejects a thinking level.
_THINKING_FALLBACK = {"minimal": "low", "low": "medium", "medium": "high"}


@dataclass(frozen=True)
class GeminiVisionConfig:
    api_key: str | None = None
    model: str = DEFAULT_GEMINI_VISION_MODEL
    timeout_s: float = 120.0
    max_attempts: int = 3

    @classmethod
    def from_env(cls) -> "GeminiVisionConfig":
        return cls(
            api_key=os.environ.get("GOOGLE_API_KEY") or os.environ.get("GEMINI_API_KEY"),
            model=os.environ.get("GEMINI_VISION_MODEL", DEFAULT_GEMINI_VISION_MODEL),
        )


@dataclass
class VisionImage:
    data: bytes
    mime_type: str = "image/png"
    #: Shown to the model before the image ("View 3 of 10: top").
    caption: str = ""


class GeminiVisionClient:
    """Thin wrapper: images + instructions in, validated JSON out."""

    def __init__(self, config: GeminiVisionConfig | None = None) -> None:
        self.config = config or GeminiVisionConfig.from_env()
        self._client = None
        self._thinking_override: Optional[str] = None

    def is_available(self) -> bool:
        if not self.config.api_key:
            return False
        try:
            import google.genai  # noqa: F401  # type: ignore
        except Exception:
            return False
        return True

    def _get_client(self) -> Any:
        if self._client is not None:
            return self._client
        if not self.config.api_key:
            raise ProviderConfigError("GOOGLE_API_KEY is not set; Gemini vision unavailable")
        try:
            from google import genai  # type: ignore
        except Exception as exc:  # pragma: no cover - import error
            raise ProviderConfigError("the google-genai package is not installed") from exc
        self._client = genai.Client(api_key=self.config.api_key)
        return self._client

    def analyze(
        self,
        *,
        prompt: str,
        images: Sequence[VisionImage],
        schema: Mapping[str, Any],
        thinking_level: str = "minimal",
        system: Optional[str] = None,
    ) -> dict[str, Any]:
        """One structured call. Returns the parsed JSON object.

        Retries transient failures (network, 5xx, unparsable JSON) up to
        ``max_attempts`` with exponential backoff; raises
        :class:`ProviderCallError` when every attempt failed.
        """

        client = self._get_client()
        parts: list[dict[str, Any]] = []
        if system:
            parts.append({"type": "text", "text": system})
        parts.append({"type": "text", "text": prompt})
        for img in images:
            if img.caption:
                parts.append({"type": "text", "text": img.caption})
            parts.append({
                "type": "image",
                "data": base64.b64encode(img.data).decode("ascii"),
                "mime_type": img.mime_type,
            })
        body: dict[str, Any] = {
            "model": self.config.model,
            "input": parts,
            "response_format": {
                "type": "text",
                "mime_type": "application/json",
                "schema": dict(schema),
            },
            "generation_config": {"thinking_level": thinking_level},
        }
        if self._thinking_override is not None:
            body["generation_config"]["thinking_level"] = self._thinking_override
        last: Exception | None = None
        attempt = 0
        while attempt < max(1, self.config.max_attempts):
            try:
                interaction = client.interactions.create(
                    timeout=self.config.timeout_s, **body)
                text = getattr(interaction, "output_text", None)
                if not text:
                    raise ProviderCallError("Gemini returned no output_text")
                return json.loads(text)
            except json.JSONDecodeError as exc:
                last = ProviderCallError(f"Gemini returned non-JSON: {exc}")
            except ProviderCallError as exc:
                last = exc
            except Exception as exc:  # pragma: no cover - network
                msg = str(exc)
                # The docs recommend "minimal" for segmentation, but not every
                # model accepts it (gemini-3.8-flash answers 400). Step up to
                # the lowest level it does accept and remember it.
                level = body["generation_config"]["thinking_level"]
                if "thinking level" in msg.lower() and "not supported" in msg.lower() \
                        and level in _THINKING_FALLBACK:
                    body["generation_config"]["thinking_level"] = _THINKING_FALLBACK[level]
                    self._thinking_override = _THINKING_FALLBACK[level]
                    continue
                if "Error code: 400" in msg or "invalid_request" in msg:
                    raise ProviderCallError(f"Gemini vision request rejected: {msg}") from exc
                last = ProviderCallError(f"Gemini vision request failed: {msg}")
            attempt += 1
            time.sleep(min(8.0, 1.5 * (2 ** attempt)))
        assert last is not None
        raise last
