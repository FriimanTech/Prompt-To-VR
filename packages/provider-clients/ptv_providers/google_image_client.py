"""GoogleImageClient: Imagen + Gemini multimodal image fallback.

Verified April 2026:
    * Python SDK package is ``google-genai`` (``pip install google-genai``).
    * Imagen: ``client.models.generate_images(model="imagen-4.0-generate-001",
      prompt=..., config=types.GenerateImagesConfig(number_of_images=...))``.
    * Gemini multimodal image fallback:
      ``client.models.generate_content(model="gemini-2.5-flash-image",
      contents=..., config=types.GenerateContentConfig(response_modalities=["IMAGE"]))``.
    * Client auth uses ``GEMINI_API_KEY`` or ``GOOGLE_API_KEY`` (both accepted
      by the SDK). We prefer ``GOOGLE_API_KEY`` because that's what we ask for
      in ``.env.example``.
"""

from __future__ import annotations

import io
import os
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable, Optional

from .errors import ProviderCallError, ProviderConfigError


@dataclass(frozen=True)
class GoogleImageConfig:
    api_key: str | None = None
    imagen_model: str = "imagen-4.0-generate-001"
    gemini_image_model: str = "gemini-2.5-flash-image"

    @classmethod
    def from_env(cls) -> "GoogleImageConfig":
        return cls(
            api_key=os.environ.get("GOOGLE_API_KEY")
            or os.environ.get("GEMINI_API_KEY"),
            imagen_model=os.environ.get("IMAGEN_MODEL", "imagen-4.0-generate-001"),
            gemini_image_model=os.environ.get(
                "GEMINI_IMAGE_MODEL", "gemini-2.5-flash-image"
            ),
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
    # Primary path: Imagen
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
        """Call Imagen and persist PNGs to ``out_dir``."""

        self._ensure_sdk()
        client = self._get_client()
        types = self._types

        cfg_kwargs: dict = {"number_of_images": number_of_images}
        if aspect_ratio:
            cfg_kwargs["aspect_ratio"] = aspect_ratio
        if negative_prompt:
            # negative_prompt is Vertex-AI only on imagen-4*. If the Developer
            # API rejects it we retry without it below.
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
            # Retry once without negative_prompt if that's what the server
            # refused; this covers the Gemini Developer API which drops that
            # kwarg while Vertex AI keeps it.
            if "negative_prompt" in cfg_kwargs and (
                "negative_prompt" in msg
                or "not supported" in msg
                or "unknown field" in msg
                or "400" in msg
            ):
                cfg_kwargs = {k: v for k, v in cfg_kwargs.items() if k != "negative_prompt"}
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
    # Fallback path: Gemini multimodal image
    # ------------------------------------------------------------------
    def generate_with_gemini(
        self,
        *,
        prompt: str,
        out_dir: str | os.PathLike[str],
        filename_prefix: str = "ref",
        aspect_ratio: str = "1:1",
    ) -> list[ImageResult]:
        """Fast iterative multimodal image generation via Gemini Flash Image."""

        self._ensure_sdk()
        client = self._get_client()
        types = self._types

        cfg = types.GenerateContentConfig(
            response_modalities=["IMAGE"],
            image_config=types.ImageConfig(aspect_ratio=aspect_ratio),
        )
        try:
            resp = client.models.generate_content(
                model=self.config.gemini_image_model,
                contents=prompt,
                config=cfg,
            )
        except Exception as e:  # pragma: no cover - network
            raise ProviderCallError(f"Gemini image request failed: {e}") from e

        out = Path(out_dir)
        out.mkdir(parents=True, exist_ok=True)
        results: list[ImageResult] = []
        idx = 0
        for cand in resp.candidates or []:
            parts = getattr(cand.content, "parts", None) or []
            for part in parts:
                inline = getattr(part, "inline_data", None)
                if inline and getattr(inline, "data", None):
                    data = bytes(inline.data)
                    mime = getattr(inline, "mime_type", "image/png") or "image/png"
                    ext = ".png" if mime.endswith("png") else ".jpg"
                    path = out / f"{filename_prefix}_{idx:02d}{ext}"
                    path.write_bytes(data)
                    results.append(
                        ImageResult(
                            path=path,
                            bytes_len=len(data),
                            model=self.config.gemini_image_model,
                            mime_type=mime,
                        )
                    )
                    idx += 1
        if not results:
            raise ProviderCallError(
                "Gemini image model returned no inline image parts."
            )
        return results

    def generate(
        self,
        *,
        prompt: str,
        out_dir: str | os.PathLike[str],
        number_of_images: int = 1,
        aspect_ratio: str = "1:1",
        negative_prompt: Optional[str] = None,
        filename_prefix: str = "ref",
        prefer: str = "imagen",
    ) -> list[ImageResult]:
        """Smart path: try Imagen first, fall back to Gemini on failure."""

        order: Iterable[str] = (
            ("imagen", "gemini") if prefer == "imagen" else ("gemini", "imagen")
        )
        last_err: Exception | None = None
        for choice in order:
            try:
                if choice == "imagen":
                    return self.generate_with_imagen(
                        prompt=prompt,
                        out_dir=out_dir,
                        number_of_images=number_of_images,
                        aspect_ratio=aspect_ratio,
                        negative_prompt=negative_prompt,
                        filename_prefix=filename_prefix,
                    )
                return self.generate_with_gemini(
                    prompt=prompt,
                    out_dir=out_dir,
                    filename_prefix=filename_prefix,
                    aspect_ratio=aspect_ratio,
                )
            except Exception as e:
                last_err = e
                continue
        assert last_err is not None
        raise ProviderCallError(f"All Google image paths failed: {last_err}")

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


def _read_image_bytes(img_obj) -> bytes:
    """The google-genai SDK returns an Image object; extract raw bytes."""
    data = getattr(img_obj, "image_bytes", None)
    if data is not None:
        return bytes(data)
    # Fallback: save via PIL
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
