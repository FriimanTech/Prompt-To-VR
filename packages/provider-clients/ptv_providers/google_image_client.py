"""GoogleImageClient: Gemini native image generation.

Verified June 2026 (Google Imagen 4 deprecation notice):
    * Imagen 4 endpoints (``imagen-4.0-generate-001``, ``imagen-4.0-ultra-*``,
      ``imagen-4.0-fast-*``) are discontinued after 2026-08-17. Google
      recommends migrating to ``gemini-3.1-flash-image``.
    * Python SDK package is ``google-genai`` (``pip install google-genai``).
    * Text-to-image uses ``client.models.generate_content`` with
      ``config=types.GenerateContentConfig(response_modalities=["IMAGE"],
      image_config=types.ImageConfig(aspect_ratio=...))``.
    * Client auth uses ``GEMINI_API_KEY`` or ``GOOGLE_API_KEY`` (both accepted
      by the SDK). We prefer ``GOOGLE_API_KEY`` because that's what we ask for
      in ``.env.example``.

The legacy ``generate_with_imagen`` helper is retained for callers that still
invoke it directly, but the pipeline's ``generate()`` entry point uses Gemini
only.
"""

from __future__ import annotations

import io
import os
import warnings
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

from .errors import ProviderCallError, ProviderConfigError

# Default model after Imagen 4 → Gemini migration (Google notice, Aug 2026).
DEFAULT_GEMINI_IMAGE_MODEL = "gemini-3.1-flash-image"


@dataclass(frozen=True)
class GoogleImageConfig:
    api_key: str | None = None
    gemini_image_model: str = DEFAULT_GEMINI_IMAGE_MODEL
    # Deprecated: Imagen endpoints are being removed. Kept so older configs
    # that still pass ``imagen_model=...`` do not crash on construction.
    imagen_model: str = "imagen-4.0-generate-001"

    @classmethod
    def from_env(cls) -> "GoogleImageConfig":
        return cls(
            api_key=os.environ.get("GOOGLE_API_KEY")
            or os.environ.get("GEMINI_API_KEY"),
            gemini_image_model=os.environ.get(
                "GEMINI_IMAGE_MODEL", DEFAULT_GEMINI_IMAGE_MODEL
            ),
            imagen_model=os.environ.get("IMAGEN_MODEL", "imagen-4.0-generate-001"),
        )


@dataclass
class ImageResult:
    """Single generated image persisted to disk."""

    path: Path
    bytes_len: int
    model: str
    mime_type: str = "image/png"
    metadata: dict = field(default_factory=dict)


class GoogleImageClient:
    def __init__(self, config: GoogleImageConfig | None = None) -> None:
        self.config = config or GoogleImageConfig.from_env()
        self._genai = None
        self._types = None
        self._client = None

    # ------------------------------------------------------------------
    # Primary path: Gemini native image (gemini-3.1-flash-image)
    # ------------------------------------------------------------------
    def generate_with_gemini(
        self,
        *,
        prompt: str,
        out_dir: str | os.PathLike[str],
        number_of_images: int = 1,
        filename_prefix: str = "ref",
        aspect_ratio: str = "1:1",
        negative_prompt: Optional[str] = None,
    ) -> list[ImageResult]:
        """Text-to-image via ``generate_content`` on a Gemini image model."""

        self._ensure_sdk()
        client = self._get_client()
        types = self._types

        composed_prompt = _compose_gemini_prompt(prompt, negative_prompt)
        count = max(1, int(number_of_images))
        out = Path(out_dir)
        out.mkdir(parents=True, exist_ok=True)
        results: list[ImageResult] = []

        for i in range(count):
            cfg = types.GenerateContentConfig(
                response_modalities=["IMAGE"],
                image_config=types.ImageConfig(aspect_ratio=aspect_ratio),
            )
            try:
                resp = client.models.generate_content(
                    model=self.config.gemini_image_model,
                    contents=composed_prompt,
                    config=cfg,
                )
            except Exception as e:  # pragma: no cover - network
                raise ProviderCallError(
                    f"Gemini image request failed ({self.config.gemini_image_model}): {e}"
                ) from e

            batch = _extract_inline_images(
                resp,
                out_dir=out,
                filename_prefix=filename_prefix,
                start_index=i,
                model=self.config.gemini_image_model,
            )
            if not batch:
                raise ProviderCallError(
                    f"Gemini image model {self.config.gemini_image_model!r} "
                    "returned no inline image parts."
                )
            results.extend(batch)
            if len(results) >= count:
                break

        if not results:
            raise ProviderCallError(
                f"Gemini image model {self.config.gemini_image_model!r} "
                "returned zero images."
            )
        return results[:count]

    def generate(
        self,
        *,
        prompt: str,
        out_dir: str | os.PathLike[str],
        number_of_images: int = 1,
        aspect_ratio: str = "1:1",
        negative_prompt: Optional[str] = None,
        filename_prefix: str = "ref",
        prefer: str = "gemini",
    ) -> list[ImageResult]:
        """Generate reference images using the Gemini native image model.

        ``prefer`` is accepted for backward compatibility. Values other than
        ``"gemini"`` emit a deprecation warning and still route through
        Gemini because Imagen 4 endpoints are being discontinued.
        """

        if prefer not in ("gemini", "imagen"):
            raise ValueError(f"Unknown prefer={prefer!r}; use 'gemini'.")
        if prefer == "imagen":
            warnings.warn(
                "prefer='imagen' is deprecated; Imagen 4 endpoints are being "
                "removed. Using gemini-3.1-flash-image instead.",
                DeprecationWarning,
                stacklevel=2,
            )
        return self.generate_with_gemini(
            prompt=prompt,
            out_dir=out_dir,
            number_of_images=number_of_images,
            aspect_ratio=aspect_ratio,
            negative_prompt=negative_prompt,
            filename_prefix=filename_prefix,
        )

    # ------------------------------------------------------------------
    # Legacy path: Imagen (deprecated — do not use in new code)
    # ------------------------------------------------------------------
    def generate_with_imagen(
        self,
        *,
        prompt: str,
        out_dir: str | os.PathLike[str],
        number_of_images: int = 1,
        aspect_ratio: str = "1:1",
        negative_prompt: Optional[str] = None,
        filename_prefix: str = "ref",
    ) -> list[ImageResult]:
        """Deprecated. Imagen 4 endpoints are discontinued after 2026-08-17.

        New code should call :meth:`generate_with_gemini` or :meth:`generate`.
        This method remains for tests and explicit legacy callers.
        """

        warnings.warn(
            "generate_with_imagen is deprecated; migrate to "
            f"generate_with_gemini (model={DEFAULT_GEMINI_IMAGE_MODEL}).",
            DeprecationWarning,
            stacklevel=2,
        )

        self._ensure_sdk()
        client = self._get_client()
        types = self._types

        cfg_kwargs: dict = {"number_of_images": number_of_images}
        if aspect_ratio:
            cfg_kwargs["aspect_ratio"] = aspect_ratio
        if negative_prompt:
            cfg_kwargs["negative_prompt"] = negative_prompt

        def _call(cfg_kwargs: dict):
            return client.models.generate_images(
                model=self.config.imagen_model,
                prompt=prompt,
                config=types.GenerateImagesConfig(**cfg_kwargs),
            )

        try:
            resp = _call(cfg_kwargs)
        except Exception as e:  # pragma: no cover - network error surfacing
            msg = str(e).lower()
            if "negative_prompt" in cfg_kwargs and (
                "negative_prompt" in msg
                or "not supported" in msg
                or "unknown field" in msg
                or "400" in msg
            ):
                cfg_kwargs = {
                    k: v for k, v in cfg_kwargs.items() if k != "negative_prompt"
                }
                try:
                    resp = _call(cfg_kwargs)
                except Exception as e2:
                    raise ProviderCallError(f"Imagen request failed: {e2}") from e2
            else:
                raise ProviderCallError(f"Imagen request failed: {e}") from e

        out = Path(out_dir)
        out.mkdir(parents=True, exist_ok=True)
        results: list[ImageResult] = []
        for i, gen in enumerate(resp.generated_images or []):
            data = _read_image_bytes(gen.image)
            path = out / f"{filename_prefix}_{i:02d}.png"
            path.write_bytes(data)
            results.append(
                ImageResult(
                    path=path,
                    bytes_len=len(data),
                    model=self.config.imagen_model,
                )
            )
        if not results:
            raise ProviderCallError("Imagen returned zero images.")
        return results

    # ------------------------------------------------------------------
    # Internals
    # ------------------------------------------------------------------
    def _get_client(self):
        if self._client is not None:
            return self._client
        if not self.config.api_key:
            raise ProviderConfigError(
                "GOOGLE_API_KEY (or GEMINI_API_KEY) is not set."
            )
        self._client = self._genai.Client(api_key=self.config.api_key)
        return self._client

    def _ensure_sdk(self) -> None:
        if self._genai is not None:
            return
        try:
            from google import genai as _genai  # type: ignore
            from google.genai import types as _types  # type: ignore
        except Exception as e:  # pragma: no cover
            raise ProviderConfigError(
                "The `google-genai` Python package is not installed."
                " Run `pip install google-genai`."
            ) from e
        self._genai = _genai
        self._types = _types


def _compose_gemini_prompt(prompt: str, negative_prompt: Optional[str]) -> str:
    """Gemini image models do not accept Imagen's ``negative_prompt`` field.

    When an AssetRef carries one, fold it into the text prompt instead.
    """

    base = prompt.strip()
    if not negative_prompt or not str(negative_prompt).strip():
        return base
    return f"{base}\n\nAvoid in the image: {str(negative_prompt).strip()}"


def _extract_inline_images(
    resp,
    *,
    out_dir: Path,
    filename_prefix: str,
    start_index: int,
    model: str,
) -> list[ImageResult]:
    results: list[ImageResult] = []
    idx = start_index
    for cand in resp.candidates or []:
        parts = getattr(cand.content, "parts", None) or []
        for part in parts:
            inline = getattr(part, "inline_data", None)
            if inline and getattr(inline, "data", None):
                data = bytes(inline.data)
                mime = getattr(inline, "mime_type", "image/png") or "image/png"
                ext = ".png" if mime.endswith("png") else ".jpg"
                path = out_dir / f"{filename_prefix}_{idx:02d}{ext}"
                path.write_bytes(data)
                results.append(
                    ImageResult(
                        path=path,
                        bytes_len=len(data),
                        model=model,
                        mime_type=mime,
                    )
                )
                idx += 1
    return results


def _read_image_bytes(img_obj) -> bytes:
    """The google-genai SDK returns an Image object; extract raw bytes."""
    data = getattr(img_obj, "image_bytes", None)
    if data is not None:
        return bytes(data)
    try:  # pragma: no cover - depends on SDK internals
        from PIL import Image as _PIL

        pil: _PIL.Image.Image = img_obj._pil_image  # type: ignore[attr-defined]
        buf = io.BytesIO()
        pil.save(buf, format="PNG")
        return buf.getvalue()
    except Exception:
        raise ProviderCallError(
            "Could not extract bytes from google.genai image result."
        )
