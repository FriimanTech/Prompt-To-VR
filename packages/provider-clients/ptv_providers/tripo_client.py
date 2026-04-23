"""TripoClient: image-to-3D and text-to-3D with textured + PBR defaults.

Verified April 2026:
    * Python SDK: ``tripo3d`` (VAST-AI-Research/tripo-python-sdk).
    * ``TripoClient.image_to_model(...)`` and ``text_to_model(...)`` are
      async. They return a ``task_id`` string; poll with ``wait_for_task``
      and download with ``download_task_models``.
    * Textured + PBR + image-aligned output is enforced here:
      ``texture=True``, ``pbr=True``, ``texture_alignment="original_image"``,
      ``texture_quality="detailed"`` by default.

All public methods on this wrapper are synchronous. The wrapper spins up an
event loop internally so the orchestrator does not have to be async to use
Tripo.
"""

from __future__ import annotations

import asyncio
import os
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from .errors import ProviderCallError, ProviderConfigError


@dataclass(frozen=True)
class TripoConfig:
    api_key: str | None = None
    # If None, the SDK's default latest model is used.
    model_version: str | None = None
    texture_quality: str = "detailed"
    poll_interval_s: float = 3.0
    timeout_s: float = 600.0

    @classmethod
    def from_env(cls) -> "TripoConfig":
        return cls(
            api_key=os.environ.get("TRIPO_API_KEY"),
            model_version=(os.environ.get("TRIPO_MODEL_VERSION") or None),
        )


@dataclass
class TripoTaskResult:
    task_id: str
    status: str
    files: dict[str, Path] = field(default_factory=dict)
    raw_task: Any | None = None

    @property
    def primary_glb(self) -> Path | None:
        for key in ("pbr_model", "model", "base_model"):
            p = self.files.get(key)
            if p is not None:
                return p
        for p in self.files.values():
            if p.suffix.lower() in (".glb", ".gltf"):
                return p
        return None


class TripoClient:
    def __init__(self, config: TripoConfig | None = None) -> None:
        self.config = config or TripoConfig.from_env()
        self._sdk = None

    # ------------------------------------------------------------------
    # Public API
    # ------------------------------------------------------------------
    def image_to_model(
        self,
        *,
        image_path: str | os.PathLike[str],
        out_dir: str | os.PathLike[str],
        texture_quality: str | None = None,
        face_limit: int | None = None,
    ) -> TripoTaskResult:
        """Submit an image-to-3D job and block until it finishes."""

        self._ensure_sdk()
        self._require_key()
        kwargs = self._common_kwargs(
            texture_quality=texture_quality,
            face_limit=face_limit,
        )
        kwargs["image"] = str(image_path)

        return self._run_async(lambda: self._image_to_model_async(out_dir, kwargs))

    def text_to_model(
        self,
        *,
        prompt: str,
        out_dir: str | os.PathLike[str],
        negative_prompt: str | None = None,
        texture_quality: str | None = None,
        face_limit: int | None = None,
    ) -> TripoTaskResult:
        """Submit a text-to-3D job (fallback path when no concept art)."""

        self._ensure_sdk()
        self._require_key()
        kwargs = self._common_kwargs(
            texture_quality=texture_quality,
            face_limit=face_limit,
        )
        kwargs["prompt"] = prompt
        if negative_prompt:
            kwargs["negative_prompt"] = negative_prompt

        return self._run_async(lambda: self._text_to_model_async(out_dir, kwargs))

    # ------------------------------------------------------------------
    # Internals
    # ------------------------------------------------------------------
    def _common_kwargs(
        self,
        *,
        texture_quality: str | None,
        face_limit: int | None,
    ) -> dict[str, Any]:
        kwargs: dict[str, Any] = {
            "texture": True,
            "pbr": True,
            "texture_alignment": "original_image",
            "texture_quality": texture_quality or self.config.texture_quality,
        }
        if self.config.model_version:
            kwargs["model_version"] = self.config.model_version
        if face_limit is not None:
            kwargs["face_limit"] = int(face_limit)
        return kwargs

    async def _image_to_model_async(
        self,
        out_dir: str | os.PathLike[str],
        kwargs: dict[str, Any],
    ) -> TripoTaskResult:
        TripoClientCls = self._sdk.TripoClient
        async with TripoClientCls(api_key=self._require_key()) as client:
            task_id = await client.image_to_model(**kwargs)
            task = await client.wait_for_task(
                task_id,
                polling_interval=self.config.poll_interval_s,
                timeout=self.config.timeout_s,
                verbose=False,
            )
            files = await client.download_task_models(task, str(out_dir))
            return _to_result(task_id, task, files)

    async def _text_to_model_async(
        self,
        out_dir: str | os.PathLike[str],
        kwargs: dict[str, Any],
    ) -> TripoTaskResult:
        TripoClientCls = self._sdk.TripoClient
        async with TripoClientCls(api_key=self._require_key()) as client:
            task_id = await client.text_to_model(**kwargs)
            task = await client.wait_for_task(
                task_id,
                polling_interval=self.config.poll_interval_s,
                timeout=self.config.timeout_s,
                verbose=False,
            )
            files = await client.download_task_models(task, str(out_dir))
            return _to_result(task_id, task, files)

    def _run_async(self, coro_factory):
        """Run an async job from sync code.

        ``coro_factory`` is a zero-arg callable that returns a fresh coroutine,
        so we can retry with a new event loop if needed without re-awaiting.
        """
        try:
            return asyncio.run(coro_factory())
        except RuntimeError:
            loop = asyncio.new_event_loop()
            try:
                return loop.run_until_complete(coro_factory())
            finally:
                loop.close()

    def _require_key(self) -> str:
        if not self.config.api_key:
            raise ProviderConfigError("TRIPO_API_KEY is not set.")
        return self.config.api_key

    def _ensure_sdk(self) -> None:
        if self._sdk is not None:
            return
        try:
            import tripo3d  # type: ignore
        except Exception as e:  # pragma: no cover
            raise ProviderConfigError(
                "The `tripo3d` Python package is not installed."
                " Run `pip install tripo3d`."
            ) from e
        self._sdk = tripo3d


def _to_result(task_id: str, task, files: dict[str, str]) -> TripoTaskResult:
    paths: dict[str, Path] = {}
    for k, v in (files or {}).items():
        p = Path(v)
        if p.exists():
            paths[k] = p
    status = getattr(task, "status", "unknown")
    return TripoTaskResult(
        task_id=task_id,
        status=str(status),
        files=paths,
        raw_task=task,
    )
