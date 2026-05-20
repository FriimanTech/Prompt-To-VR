"""TripoClient: image-to-3D and text-to-3D with textured + PBR defaults.

Verified April 2026 against tripo3d==0.3.12 (VAST-AI-Research/tripo-python-sdk):
    * ``TripoClient.image_to_model(...)`` and ``text_to_model(...)`` are
      async. They return a ``task_id`` string; poll with ``wait_for_task``
      and download with ``download_task_models`` (returns a dict with keys
      ``model``, ``base_model``, ``pbr_model`` -- all GLB files).
    * Highest-fidelity output is enforced here by default (verified Apr 2026
      against https://platform.tripo3d.ai/docs/generation):
      ``model_version="v3.1-20260211"`` (latest image-to-3D checkpoint),
      ``texture=True``, ``pbr=True``, ``texture_alignment="original_image"``,
      ``texture_quality="detailed"``, ``geometry_quality="detailed"``
      ("Ultra Mode" mesh reconstruction, v3.x only),
      ``enable_image_autofix=True`` (Tripo cleans the reference image
      before generation), and ``orientation="align_image"`` (image-to-3D
      only). Older SDK releases that don't list every parameter still
      work because :func:`_accepted_kwargs` filters by signature and
      forwards only what the installed SDK accepts.
    * The Tripo SDK lists ``aiohttp`` as an OPTIONAL dependency on PyPI even
      though the async HTTP paths it uses for upload / polling / download
      actually require it. We re-declare ``aiohttp>=3.8.0`` as a direct
      dependency of ``ptv-provider-clients`` so the orchestrator venv always
      has it, and ``_ensure_sdk`` below double-checks at runtime.

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


_DEFAULT_MODEL_VERSION = "v3.1-20260211"
"""Tripo's latest image-to-3D checkpoint (per
https://platform.tripo3d.ai/docs/generation). v3.x is the first family
that exposes ``geometry_quality="detailed"`` ("Ultra Mode" -- maximum
mesh detail), and v3.1 is the current best-quality default. Earlier
checkpoints (v2.5 etc.) reject ``geometry_quality``, so we still gate
that parameter via :func:`_model_supports_geometry_quality` below.

Override with the ``TRIPO_MODEL_VERSION`` env var if you need to roll
back to an older checkpoint (e.g. for cost or determinism)."""


def _model_supports_geometry_quality(model_version: str | None) -> bool:
    """True when ``model_version`` accepts the ``geometry_quality`` kwarg.

    Tripo documents that ``geometry_quality`` was introduced with the
    v3.0 family (first checkpoint ``v3.0-20250812``). We parse the
    leading ``vX.Y`` segment and return True for v3.x or higher.
    """

    if not model_version:
        # When we don't know, assume yes because our default is v3.0.
        return True
    v = model_version.lower().lstrip("v")
    try:
        major_str = v.split("-", 1)[0].split(".", 1)[0]
        return int(major_str) >= 3
    except (ValueError, IndexError):
        return False


@dataclass(frozen=True)
class TripoConfig:
    """Generation defaults tuned for the highest fidelity Tripo3D output
    that the API + SDK currently expose. Verified Apr 2026 against
    https://platform.tripo3d.ai/docs/generation.

    Quality knobs (see :meth:`TripoClient._common_kwargs`):

    * ``texture=True``        -> emit a colored mesh (always on).
    * ``pbr=True``            -> emit BaseColor / Metallic / Roughness /
                                 Normal maps so the runtime can shade with
                                 PBR lighting.
    * ``texture_quality="detailed"``
                              -> high-resolution texture maps; "standard"
                                 is Tripo's lower-cost default.
    * ``geometry_quality="detailed"``
                              -> "Ultra Mode" mesh reconstruction (max
                                 detail). v3.x checkpoints only.
    * ``enable_image_autofix=True``
                              -> Tripo pre-cleans the input image
                                 (background removal / framing / contrast)
                                 before generation, which materially
                                 improves results for messy reference
                                 images. Documented in the platform docs;
                                 newer SDK releases forward it as a
                                 first-class parameter, older ones drop
                                 it silently via ``_accepted_kwargs``.
    * ``auto_size=True``      -> approximate real-world metric scale so
                                 VR placement looks plausible.
    """

    api_key: str | None = None
    # Default to v3.1, Tripo's latest+best image-to-3D checkpoint as of
    # Apr 2026. Override with TRIPO_MODEL_VERSION to roll back if needed.
    model_version: str | None = _DEFAULT_MODEL_VERSION
    texture: bool = True
    pbr: bool = True
    texture_quality: str = "detailed"
    # Only sent when the selected model supports it. "detailed" is the
    # highest tier Tripo exposes (internally "Ultra Mode"); standard is
    # the default.
    geometry_quality: str = "detailed"
    # Tripo's image preprocessing pass -- materially improves outputs
    # for less-than-pristine reference renders.
    enable_image_autofix: bool = True
    # Ask Tripo to try to auto-scale the mesh to real-world dimensions.
    # Big quality win in VR because every asset shows up at a plausible
    # meter-scale instead of a unit cube. Safe default, callers can
    # still override per-asset via the Asset Manifest if needed.
    auto_size: bool = True
    poll_interval_s: float = 3.0
    timeout_s: float = 600.0

    @classmethod
    def from_env(cls) -> "TripoConfig":
        return cls(
            api_key=os.environ.get("TRIPO_API_KEY"),
            model_version=(
                os.environ.get("TRIPO_MODEL_VERSION") or _DEFAULT_MODEL_VERSION
            ),
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
        # ``orientation="align_image"`` tells Tripo to orient the generated
        # mesh to match the reference image's front-facing view, which is
        # exactly what we want so the VR scene places the asset the way the
        # concept art depicted it. This is only valid on image_to_model (the
        # text_to_model endpoint in v0.3.12 does not take an ``orientation``
        # kwarg and will reject it).
        kwargs["orientation"] = "align_image"

        return self._run_async(lambda: self._image_to_model_async(out_dir, kwargs))

    def get_balance(self) -> dict[str, float]:
        """Return the Tripo account balance without spending any credits.

        Hits ``GET /user/balance`` via the SDK's ``TripoClient.get_balance``
        and returns ``{"balance": <credits>, "frozen": <credits>}``.
        Useful as a preflight check at the start of the Tripo stage so the
        orchestrator can fail fast with a clear "insufficient credits"
        message instead of firing N image-to-3D requests that will all
        fail with a 402 / empty-result response and each still count
        against rate limits / quotas.
        """

        self._ensure_sdk()
        self._require_key()
        return self._run_async(self._get_balance_async)

    async def _get_balance_async(self) -> dict[str, float]:
        TripoClientCls = self._sdk.TripoClient
        async with TripoClientCls(api_key=self._require_key()) as client:
            b = await client.get_balance()
            return {
                "balance": float(getattr(b, "balance", 0.0) or 0.0),
                "frozen": float(getattr(b, "frozen", 0.0) or 0.0),
            }

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
        # These flags force the Tripo pipeline to output a fully textured,
        # PBR-shaded GLB aligned to the reference image. ``download_task_models``
        # will return an entry under ``pbr_model`` which is the fully coloured
        # asset we want for the VR scene.
        #
        # Quality knobs (verified Apr 2026 against Tripo's public API docs at
        # https://platform.tripo3d.ai/docs/generation and the community API
        # reference at 3daistudio.com/Platform/API/Documentation/3d-generation/tripo):
        #
        #   model_version="v3.1-20260211" -> Tripo's latest image-to-3D
        #                                  checkpoint (highest fidelity).
        #   texture=True                -> coloured output (default).
        #   pbr=True                    -> emit PBR maps (BaseColor/Metallic/
        #                                  Roughness/Normal) so the VR renderer
        #                                  can light the asset correctly.
        #   texture_quality="detailed"  -> HD texture resolution (+20 credits).
        #   geometry_quality="detailed" -> "Ultra Mode" higher-res mesh,
        #                                  v3.0+ only (+40 credits). Passed
        #                                  conditionally below so older
        #                                  model versions like v2.5 don't
        #                                  reject the request.
        #   enable_image_autofix=True   -> Tripo pre-cleans the reference
        #                                  image (bg removal, contrast,
        #                                  framing) before generation.
        #                                  Documented platform parameter;
        #                                  silently dropped by older SDKs
        #                                  via ``_accepted_kwargs``.
        #   texture_alignment="original_image" and orientation="align_image"
        #                                  (image_to_model only) keep the UVs
        #                                  + pose matching the reference art.
        #   auto_size=True              -> approximate real-world metric scale
        #                                  so VR placement looks plausible.
        kwargs: dict[str, Any] = {
            "texture": self.config.texture,
            "pbr": self.config.pbr,
            "texture_alignment": "original_image",
            "texture_quality": texture_quality or self.config.texture_quality,
            "enable_image_autofix": self.config.enable_image_autofix,
            "auto_size": self.config.auto_size,
        }
        if self.config.model_version:
            kwargs["model_version"] = self.config.model_version
        if _model_supports_geometry_quality(self.config.model_version):
            kwargs["geometry_quality"] = self.config.geometry_quality
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
            accepted = _accepted_kwargs(client.image_to_model, kwargs)
            task_id = await client.image_to_model(**accepted)
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
            accepted = _accepted_kwargs(client.text_to_model, kwargs)
            task_id = await client.text_to_model(**accepted)
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
                " Run `pip install 'tripo3d>=0.3.9' 'aiohttp>=3.8.0'`."
            ) from e
        # Tripo lists ``aiohttp`` as optional but its async code path imports
        # it eagerly. If aiohttp is missing the SDK fails deep inside with a
        # cryptic ``ModuleNotFoundError: No module named 'aiohttp'`` and every
        # asset in the manifest shows that error in the Tripo stage log.
        # Surface a clear, actionable message here instead.
        try:
            import aiohttp  # type: ignore  # noqa: F401
        except Exception as e:  # pragma: no cover
            raise ProviderConfigError(
                "The `aiohttp` Python package is required by the tripo3d SDK"
                " but is not installed in this environment."
                " Run `pip install 'aiohttp>=3.8.0'`."
            ) from e
        self._sdk = tripo3d


def _accepted_kwargs(func, kwargs: dict[str, Any]) -> dict[str, Any]:
    """Filter ``kwargs`` to only those accepted by ``func``'s signature.

    Tripo has added new params (``geometry_quality``, ``auto_size``, etc.)
    across SDK releases. Rather than pinning to the newest release and
    breaking users on older ones, we introspect the SDK's ``image_to_model``
    / ``text_to_model`` methods at call time and only pass through kwargs
    the installed SDK actually accepts. If the signature has a ``**kwargs``
    catch-all we pass everything (the SDK will forward them to the API).
    """

    import inspect

    try:
        sig = inspect.signature(func)
    except (TypeError, ValueError):
        return kwargs

    params = sig.parameters
    # If the callable has **kwargs, trust the caller and pass everything.
    if any(p.kind == inspect.Parameter.VAR_KEYWORD for p in params.values()):
        return kwargs
    return {k: v for k, v in kwargs.items() if k in params}


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
