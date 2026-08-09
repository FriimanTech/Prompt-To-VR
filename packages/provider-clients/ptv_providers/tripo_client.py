"""TripoClient: image-to-3D and text-to-3D against the Tripo **V3** API.

Talks to ``https://openapi.tripo3d.ai/v3`` directly over ``httpx``. The
official ``tripo3d`` Python SDK is deliberately NOT used: its primary
``BASE_URL`` is still ``https://api.tripo3d.ai/v2/openapi`` and it only
reaches V3 for file upload, opt-in task polling, and mesh segmentation, so
every generation call would keep running on V2 -- which is retired
2026-11-01 (no feature updates or support after 2026-10-01).

Flow (image-to-3D):

    POST /v3/files                      -> data.file_token  ("file_<uuid>")
    POST /v3/generation/image-to-model  -> data.task_id
    GET  /v3/tasks/{task_id}            -> poll to a terminal status
    GET  output.model_url               -> download the GLB

Quality defaults are unchanged from the V2 wrapper (textured + PBR +
image-aligned + Ultra-Mode geometry); only the transport moved. The one
behavioural fix: ``model`` is REQUIRED on V3 (V2 silently fell back to
``v2.5-20250123``), so the config always resolves to a concrete version
instead of omitting the field.

All public methods are synchronous.
"""

from __future__ import annotations

import os
import random
import time
import uuid
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any
from urllib.parse import urlsplit

import httpx

from .errors import ProviderCallError, ProviderConfigError


_DEFAULT_BASE_URL = "https://openapi.tripo3d.ai/v3"
"""International V3 base URL. China-mainland accounts use
``https://openapi.tripo3d.com/v3`` -- set ``TRIPO_BASE_URL`` to switch. A key
only validates in its own region; a 401 on the balance probe usually means the
wrong base URL rather than a bad key."""

_DEFAULT_MODEL_VERSION = "v3.1-20260211"
"""Tripo's latest image-to-3D checkpoint. v3.x is the first family that
exposes ``geometry_quality="detailed"`` ("Ultra Mode" -- maximum mesh detail),
and v3.1 is the current best-quality default.

Override with the ``TRIPO_MODEL_VERSION`` env var to roll back (e.g. for cost
or determinism)."""

_V3_MODEL_VERSIONS = frozenset(
    {
        "v3.1-20260211",
        "v3.0-20250812",
        "v2.5-20250123",
        "P1-20260311",
    }
)
"""The complete whitelist V3 accepts for the to-model endpoints. Anything else
is a 400 (code 1004) at request time, so we validate up front and fail with a
readable message instead of burning a run."""

# --- Auto-rigging ----------------------------------------------------------
#
# ``/v3/animations/rig-check`` and ``/v3/animations/rig`` both take a single
# ``input`` field (task_id | file_token | URL), like the to-model endpoints --
# NOT the ``original_model_task_id`` spelling the V2->V3 migration notes use.
# Verified against the live docs (developers.tripo3d.ai/en/docs/animations-rig
# and .../animations-rig-check).

_RIG_TYPES = frozenset(
    {
        "biped",
        "quadruped",
        "hexapod",
        "octopod",
        "avian",
        "serpentine",
        "aquatic",
    }
)
"""Every rig type ``rig-check`` can recommend. All of them describe a WHOLE
creature -- there is no rig type for a hand, a weapon or a vehicle, which is
why non-character assets come back ``riggable: false``."""

_RIG_MODEL_BIPED = "v1.0-20240301"
_RIG_MODEL_CREATURE = "v2.5-20260210"


def rig_model_for_type(rig_type: str) -> str:
    """The ``model`` version that can actually rig this skeleton shape.

    The two rigging checkpoints are not interchangeable: ``v1.0-20240301`` is
    humanoid-only (and the one with 90+ animation presets behind it), while
    ``v2.5-20260210`` handles the non-humanoid shapes. Sending a quadruped to
    the biped model does not error -- it produces a bad skeleton -- so this
    mapping is the only thing standing between a creature and a silently
    mangled rig.
    """

    key = (rig_type or "").strip().lower()
    if key not in _RIG_TYPES:
        raise ProviderConfigError(
            f"Unknown Tripo rig_type {rig_type!r}. Expected one of: "
            f"{', '.join(sorted(_RIG_TYPES))}."
        )
    return _RIG_MODEL_BIPED if key == "biped" else _RIG_MODEL_CREATURE


# --- Animation retargeting -------------------------------------------------
#
# Which preset ids exist depends on BOTH the rig model and the rig type, and
# the three lists barely overlap:
#
#   v1.0-20240301  biped only, 90+ presets, all namespaced ``preset:biped:``
#   v2.5-20260210  biped, 11 presets, namespaced plain ``preset:``
#   v2.5-20260210  every other rig type: exactly ONE walk-ish preset
#
# Callers therefore work in semantic INTENTS ("walk", "attack") and let this
# module pick the preset. An agent choosing preset strings directly would have
# to know which of the three lists applies before rig-check has even run, and
# a wrong string is a paid call that fails.

_ANIMATION_INTENTS = (
    "idle",
    "walk",
    "run",
    "attack",
    "shoot",
    "hurt",
    "die",
    "jump",
    "turn",
    "climb",
    "dance",
    "wave",
    "sit",
)

# v1.0-20240301: biped only, but by far the richest list.
_PRESETS_BIPED_V1 = {
    "idle": "preset:biped:idle",
    "walk": "preset:biped:walk",
    "run": "preset:biped:run",
    "attack": "preset:biped:slash",
    "shoot": "preset:biped:shoot",
    "hurt": "preset:biped:hurt",
    "die": "preset:biped:fall",
    "jump": "preset:biped:jump",
    "turn": "preset:biped:turn",
    "climb": "preset:biped:climb",
    "dance": "preset:biped:dance_01",
    "wave": "preset:biped:wave_goodbye_01",
    "sit": "preset:biped:sit",
}

# v2.5-20260210 biped: the short list, un-namespaced.
_PRESETS_BIPED_V25 = {
    "idle": "preset:idle",
    "walk": "preset:walk",
    "run": "preset:run",
    "attack": "preset:slash",
    "shoot": "preset:shoot",
    "hurt": "preset:hurt",
    "die": "preset:fall",
    "jump": "preset:jump",
    "turn": "preset:turn",
    "climb": "preset:climb",
}

# v2.5-20260210 creatures: one locomotion preset each. ``walk`` and ``run``
# both resolve to it -- the runtime plays the same clip faster for a run --
# and the caller de-duplicates so only one animation is billed.
_PRESETS_CREATURE = {
    "quadruped": "preset:quadruped:walk",
    "hexapod": "preset:hexapod:walk",
    "octopod": "preset:octopod:walk",
    "serpentine": "preset:serpentine:march",
    "aquatic": "preset:aquatic:march",
}


def animation_intents() -> tuple[str, ...]:
    """The semantic vocabulary callers may request."""
    return _ANIMATION_INTENTS


def presets_for_rig(rig_type: str, model: str | None = None) -> dict[str, str]:
    """Intent -> preset id for one rig, containing only what actually exists.

    An intent missing from the returned map has no equivalent for that rig
    (a quadruped cannot ``shoot``), and the caller should drop it rather than
    substitute something that would animate wrongly.
    """

    key = (rig_type or "").strip().lower()
    if key not in _RIG_TYPES:
        raise ProviderConfigError(
            f"Unknown Tripo rig_type {rig_type!r}. Expected one of: "
            f"{', '.join(sorted(_RIG_TYPES))}."
        )
    resolved_model = (model or rig_model_for_type(key)).strip()

    if key == "biped":
        return dict(
            _PRESETS_BIPED_V1
            if resolved_model == _RIG_MODEL_BIPED
            else _PRESETS_BIPED_V25
        )

    preset = _PRESETS_CREATURE.get(key)
    if not preset:
        return {}
    # Only locomotion exists for creature rigs; run reuses walk's clip.
    return {"walk": preset, "run": preset}


def resolve_animations(
    intents: list[str], rig_type: str, model: str | None = None
) -> tuple[list[str], dict[str, str], list[str]]:
    """Turn requested intents into the preset list to actually send.

    Returns ``(presets, intent_to_preset, dropped)``:

    * ``presets`` is de-duplicated and order-preserving, because Tripo bills
      per animation and two intents can legitimately share one preset.
    * ``intent_to_preset`` keeps BOTH intents pointing at that shared preset,
      so a quadruped still answers a "run" request with its walk clip.
    * ``dropped`` lists intents this rig has no preset for.
    """

    table = presets_for_rig(rig_type, model)
    presets: list[str] = []
    mapping: dict[str, str] = {}
    dropped: list[str] = []
    for raw in intents or []:
        intent = (raw or "").strip().lower()
        if not intent:
            continue
        preset = table.get(intent)
        if not preset:
            dropped.append(intent)
            continue
        mapping[intent] = preset
        if preset not in presets:
            presets.append(preset)
    return presets, mapping, dropped

# Terminal task states. V3 collapsed V2's 8-state enum down to 5: the old
# ``banned`` and ``expired`` are now ``failed`` carrying error_code 2008 and
# 2018 respectively.
_TERMINAL_STATUSES = frozenset({"success", "failed", "cancelled"})

# Business-level error codes worth retrying. 1007 is the per-minute creation
# quota and 2000 is the concurrent-task cap; both clear on their own.
_RETRYABLE_CODES = frozenset({1007, 2000})
# Everything else raises on the first response. The ones worth calling out:
# 1004 is a malformed request (retrying re-sends the same bad payload), 2008 is
# a moderation rejection, and 2010 is insufficient credit (HTTP 403). See
# _fatal_hint for the guidance attached to each.


def _model_forbids_geometry_quality(model_version: str) -> bool:
    """True when ``geometry_quality`` must be OMITTED for this checkpoint.

    Two separate reasons a version rejects it:

    * P1 (the low-poly/PBR workflow) rejects ``quad``, ``smart_low_poly``,
      ``generate_parts`` and ``geometry_quality`` when present AT ALL -- the
      international service 400s even on an explicit ``false``.
    * Pre-v3 checkpoints (v2.5 and older) predate the parameter.
    """

    v = model_version.strip().lower()
    if v.startswith("p1"):
        return True
    try:
        major = int(v.lstrip("v").split("-", 1)[0].split(".", 1)[0])
    except (ValueError, IndexError):
        # Unrecognised shape: be conservative and leave the field off.
        return True
    return major < 3


def _normalize_base_url(raw: str | None) -> str:
    """Return a base URL that ends in exactly one ``/v3``.

    Tripo documents the base as ``https://openapi.tripo3d.ai/v3`` while the
    endpoint tables spell paths as ``/v3/generation/...``, which makes it very
    easy to configure a doubled or missing ``/v3``. Accept either spelling.
    """

    base = (raw or _DEFAULT_BASE_URL).strip().rstrip("/")
    if not base:
        base = _DEFAULT_BASE_URL
    if not base.endswith("/v3"):
        base = f"{base}/v3"
    return base


@dataclass(frozen=True)
class TripoConfig:
    """Generation defaults tuned for the highest-fidelity Tripo output.

    Quality knobs (see :meth:`TripoClient._common_body`):

    * ``texture=True``        -> emit a coloured mesh.
    * ``pbr=True``            -> emit BaseColor / Metallic / Roughness /
                                 Normal maps so the runtime can shade with
                                 PBR lighting.
    * ``texture_quality="standard"``
                              -> balanced texture maps (opt up to "detailed"
                                 / "extreme" per call when needed).
    * ``geometry_quality="standard"``
                              -> balanced mesh reconstruction. Sent only for
                                 checkpoints that accept it; opt up to
                                 "detailed" (Ultra Mode) when needed.
    * ``enable_image_autofix=True``
                              -> Tripo pre-cleans the reference image
                                 (background removal / framing / contrast),
                                 which materially improves messy inputs.
    * ``auto_size=True``      -> approximate real-world metric scale so VR
                                 placement looks plausible.
    """

    api_key: str | None = None
    base_url: str = _DEFAULT_BASE_URL
    # Always resolves to a concrete version -- see __post_init__. V3 has no
    # server-side default and rejects a request without one.
    model_version: str | None = _DEFAULT_MODEL_VERSION
    texture: bool = True
    pbr: bool = True
    texture_quality: str = "standard"
    geometry_quality: str = "standard"
    enable_image_autofix: bool = True
    auto_size: bool = True
    poll_interval_s: float = 3.0
    timeout_s: float = 600.0
    # V3 tightened uploads to roughly 10/min (V2 allowed ~10/s). The pipeline
    # uploads one reference image per asset and a manifest may hold dozens, so
    # we self-pace to stay under the limit. In practice this costs nothing:
    # the stage is sequential and each generation already takes 30-90s, so
    # consecutive uploads are naturally further apart than this.
    min_upload_interval_s: float = 6.5
    max_retries: int = 4

    def __post_init__(self) -> None:
        # Callers routinely pass ``model_version=<setting> or None`` when the
        # setting is unset, which would otherwise defeat the field default and
        # omit the parameter entirely. On V2 that silently downgraded to the
        # server default (v2.5); on V3 it is a hard 400. Resolve it here so
        # there is exactly one place that decides the version.
        resolved = (self.model_version or _DEFAULT_MODEL_VERSION).strip()
        if resolved not in _V3_MODEL_VERSIONS:
            raise ProviderConfigError(
                f"Unsupported Tripo model version {resolved!r}. "
                f"V3 accepts: {', '.join(sorted(_V3_MODEL_VERSIONS))}. "
                "Set TRIPO_MODEL_VERSION to one of these (or leave it empty "
                f"for the default {_DEFAULT_MODEL_VERSION})."
            )
        object.__setattr__(self, "model_version", resolved)
        object.__setattr__(self, "base_url", _normalize_base_url(self.base_url))

    @classmethod
    def from_env(cls) -> "TripoConfig":
        return cls(
            api_key=os.environ.get("TRIPO_API_KEY"),
            base_url=_normalize_base_url(os.environ.get("TRIPO_BASE_URL")),
            model_version=(
                os.environ.get("TRIPO_MODEL_VERSION") or _DEFAULT_MODEL_VERSION
            ),
        )


@dataclass
class TripoRigCheck:
    """Result of ``/v3/animations/rig-check``.

    Costs no credits, which is what makes it worth calling unconditionally
    before every rig: a rig is 30 credits and most assets in a project are
    props that can never be rigged at all.
    """

    task_id: str
    riggable: bool
    rig_type: str
    raw_task: Any | None = None

    @property
    def rig_model(self) -> str:
        """The rigging checkpoint matching ``rig_type``."""
        return rig_model_for_type(self.rig_type)


@dataclass
class TripoTaskResult:
    task_id: str
    status: str
    files: dict[str, Path] = field(default_factory=dict)
    raw_task: Any | None = None

    @property
    def primary_glb(self) -> Path | None:
        # V3 merges V2's pbr_model / model / base_model into a single
        # ``output.model_url``, so in practice there is one entry keyed
        # "model". The legacy key order is kept so older callers and any
        # cached results still resolve.
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
        self._http: httpx.Client | None = None
        self._last_upload_at: float = 0.0

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

        self._require_key()
        file_token = self._upload_file(image_path)

        body = self._common_body(
            texture_quality=texture_quality,
            face_limit=face_limit,
        )
        # V3's native single-string input form: accepts a file token, a public
        # URL, or a bare task UUID. We always hand it a freshly uploaded token.
        body["input"] = file_token
        # Orient the generated mesh to the reference image's front-facing
        # view, so the VR scene places the asset the way the concept art
        # depicted it. image-to-model only.
        body["orientation"] = "align_image"
        # UVs follow the source image rather than being re-derived from the
        # geometry. Server-fixed (and therefore omitted) for text-to-model.
        body["texture_alignment"] = "original_image"

        task_id = self._create_task("/generation/image-to-model", body)
        return self._await_and_download(task_id, out_dir)

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

        self._require_key()
        body = self._common_body(
            texture_quality=texture_quality,
            face_limit=face_limit,
        )
        body["prompt"] = prompt
        if negative_prompt:
            body["negative_prompt"] = negative_prompt

        task_id = self._create_task("/generation/text-to-model", body)
        return self._await_and_download(task_id, out_dir)

    # ------------------------------------------------------------------
    # Auto-rigging
    # ------------------------------------------------------------------
    def rig_check(
        self,
        *,
        task_id: str | None = None,
        glb_path: str | os.PathLike[str] | None = None,
    ) -> TripoRigCheck:
        """Ask whether a model can be rigged, and how.

        Free, so there is no reason not to call it before every rig. Returns
        ``riggable=False`` for anything that is not a whole creature -- props,
        weapons, scenery and disembodied body parts all land there.
        """

        self._require_key()
        body = {"input": self._rig_input(task_id, glb_path)}
        rig_task = self._create_task("/animations/rig-check", body)
        task = self._poll(rig_task)
        status = str(task.get("status") or "unknown").lower()
        if status != "success":
            code = task.get("error_code")
            message = task.get("error_message") or "no error_message returned"
            raise ProviderCallError(
                f"Tripo rig-check {rig_task} finished with status {status!r} "
                f"(error_code={code}): {message}"
            )

        output = task.get("output")
        output = output if isinstance(output, dict) else {}
        # Absent rig_type is normal when riggable is false; default to biped so
        # ``rig_model`` stays callable, and let the caller gate on ``riggable``.
        return TripoRigCheck(
            task_id=rig_task,
            riggable=bool(output.get("riggable")),
            rig_type=str(output.get("rig_type") or "biped").strip().lower(),
            raw_task=task,
        )

    def rig(
        self,
        *,
        out_dir: str | os.PathLike[str],
        task_id: str | None = None,
        glb_path: str | os.PathLike[str] | None = None,
        rig_type: str = "biped",
        spec: str = "tripo",
        out_format: str = "glb",
        model: str | None = None,
    ) -> TripoTaskResult:
        """Add a skeleton and skin weights to a model. Costs ~30 credits.

        ``spec="tripo"`` (Tripo's native bone naming) is required for the
        follow-on ``/animations/retarget`` call. Live Tripo rejects Mixamo-
        named skeletons with error 1004 ("does not support mixamo skeleton
        retarget"), so Mixamo naming is deliberately NOT the default even
        though Unity/Unreal docs still advertise it. Generated C++ plays
        clips by semantic intent via ``ISkeleton::play_clip`` and does not
        hard-code Mixamo joint names for NPCs.

        ``model`` is derived from ``rig_type`` unless overridden; see
        :func:`rig_model_for_type` for why that pairing is load-bearing.
        """

        self._require_key()
        body = {
            "input": self._rig_input(task_id, glb_path),
            "model": model or rig_model_for_type(rig_type),
            "rig_type": rig_type,
            "spec": spec,
            "out_format": out_format,
        }
        rig_task = self._create_task("/animations/rig", body)
        return self._await_and_download(rig_task, out_dir)

    def retarget(
        self,
        *,
        out_dir: str | os.PathLike[str],
        rig_task_id: str,
        animations: list[str],
        out_format: str = "glb",
        bake_animation: bool = True,
        export_with_geometry: bool = True,
        animate_in_place: bool = True,
    ) -> TripoTaskResult:
        """Retarget preset animations onto an already-rigged model.

        Billed PER ANIMATION (~20 credits each), so the caller decides how
        many are worth it -- this just sends what it is given.

        ``input`` must be a successful **rig** task id. There is deliberately
        no file-upload fallback the way ``rig_check`` has one: retargeting
        needs Tripo's own skeleton for that task, and uploading a rigged GLB
        would hand it a mesh with no task lineage. An asset whose rig task id
        was lost has to be re-rigged.

        ``animate_in_place=True`` is the default and should essentially always
        stay that way. With it false the clip carries baked root motion, so a
        character translates on its own ON TOP of whatever the engine does to
        its transform -- the entity covers double the ground and then snaps
        back to the origin every time the clip loops. In-place keeps
        ``CTransform`` the single source of truth for position. Note this
        inverts Tripo's own default of false, so it is always sent explicitly.

        ``export_with_geometry=True`` is required by this engine: the runtime
        loads one GLB per mesh and reads its skin AND its clips from that same
        file, so a geometry-less animation export would have nothing to skin.
        """

        self._require_key()
        if not rig_task_id:
            raise ProviderCallError(
                "Tripo retarget needs the task_id of a successful rig task; "
                "re-run rigging for this asset to obtain one."
            )
        cleaned = [a for a in (animations or []) if a]
        if not cleaned:
            raise ProviderCallError(
                "Tripo retarget needs at least one preset animation id."
            )

        body: dict[str, Any] = {
            "input": str(rig_task_id),
            "out_format": out_format,
            "bake_animation": bake_animation,
            "export_with_geometry": export_with_geometry,
            "animate_in_place": animate_in_place,
        }
        # ``animation`` and ``animations`` are mutually exclusive; sending
        # both is a 400.
        if len(cleaned) == 1:
            body["animation"] = cleaned[0]
        else:
            body["animations"] = cleaned

        task = self._create_task("/animations/retarget", body)
        return self._await_and_download(task, out_dir)

    def _rig_input(
        self,
        task_id: str | None,
        glb_path: str | os.PathLike[str] | None,
    ) -> str:
        """Resolve the single ``input`` field the rig endpoints accept.

        Prefers a generation task id: it is free and instant. Falls back to
        uploading the GLB, which is what keeps rigging usable after a resume
        or on a project directory copied between machines, where the original
        task id is gone or expired.
        """

        if task_id:
            return str(task_id)
        if glb_path is not None:
            return self._upload_file(glb_path)
        raise ProviderCallError(
            "Tripo rig call needs either a task_id or a glb_path to upload."
        )

    def get_balance(self) -> dict[str, float]:
        """Return the Tripo account balance without spending any credits.

        Used as a stage preflight so the orchestrator can fail fast with a
        clear "insufficient credits" message instead of firing N image-to-3D
        requests that all fail and still count against rate limits.

        V3 returns decimals here (V2 returned integers), hence the float
        parse.
        """

        self._require_key()
        data = self._request("GET", "/account/balance")
        return {
            "balance": float(data.get("balance") or 0.0),
            "frozen": float(data.get("frozen") or 0.0),
        }

    def close(self) -> None:
        if self._http is not None:
            self._http.close()
            self._http = None

    def __enter__(self) -> "TripoClient":
        return self

    def __exit__(self, *exc: object) -> bool:
        self.close()
        return False

    # ------------------------------------------------------------------
    # Request payloads
    # ------------------------------------------------------------------
    def _common_body(
        self,
        *,
        texture_quality: str | None,
        face_limit: int | None,
    ) -> dict[str, Any]:
        model_version = self.config.model_version or _DEFAULT_MODEL_VERSION
        body: dict[str, Any] = {
            # ``model`` is V3's native spelling of V2's ``model_version`` and
            # is REQUIRED here -- V3 has no server-side fallback.
            "model": model_version,
            "texture": self.config.texture,
            "pbr": self.config.pbr,
            "texture_quality": texture_quality or self.config.texture_quality,
            "enable_image_autofix": self.config.enable_image_autofix,
            "auto_size": self.config.auto_size,
        }
        if not _model_forbids_geometry_quality(model_version):
            body["geometry_quality"] = self.config.geometry_quality
        if face_limit is not None:
            body["face_limit"] = int(face_limit)
        return body

    # ------------------------------------------------------------------
    # HTTP plumbing
    # ------------------------------------------------------------------
    def _client(self) -> httpx.Client:
        if self._http is None:
            self._http = httpx.Client(
                base_url=self.config.base_url,
                timeout=httpx.Timeout(60.0, connect=15.0),
            )
        return self._http

    def _headers(self) -> dict[str, str]:
        return {
            "Authorization": f"Bearer {self._require_key()}",
            # V3 echoes this back and pairs it with X-Tripo-Trace-ID, which
            # makes support requests about a specific failure tractable.
            "X-Request-ID": uuid.uuid4().hex,
        }

    def _request(
        self,
        method: str,
        path: str,
        *,
        json_body: dict[str, Any] | None = None,
        files: dict[str, Any] | None = None,
    ) -> dict[str, Any]:
        """Send one request and return the unwrapped ``data`` object.

        Retries rate-limit / concurrency / transport / 5xx failures with
        exponential backoff; surfaces everything else immediately.
        """

        client = self._client()
        last_error: str = ""
        for attempt in range(self.config.max_retries):
            try:
                response = client.request(
                    method,
                    path,
                    headers=self._headers(),
                    json=json_body,
                    files=files,
                )
            except httpx.HTTPError as exc:
                # Transport-level failure (DNS, connect, read timeout).
                last_error = f"{type(exc).__name__}: {exc}"
                if attempt + 1 >= self.config.max_retries:
                    break
                self._sleep_backoff(attempt)
                continue

            # Auth must be detected by HTTP STATUS, not by body code: V2 used
            # code 1002 while V3 uses code 2, and some deployments still emit
            # the old value.
            if response.status_code == 401:
                raise ProviderConfigError(
                    "Tripo rejected the API key (HTTP 401). Check TRIPO_API_KEY, "
                    f"and confirm the region: this client is pointed at "
                    f"{self.config.base_url} and a key only validates in its "
                    "own region."
                )

            if response.status_code == 429 or response.status_code >= 500:
                last_error = f"HTTP {response.status_code}: {response.text[:200]}"
                if attempt + 1 >= self.config.max_retries:
                    break
                self._sleep_backoff(attempt, response=response)
                continue

            payload = self._decode(response)
            code = payload.get("code")
            if code == 0:
                data = payload.get("data")
                return data if isinstance(data, dict) else {}

            message = payload.get("message") or response.text[:200]
            suggestion = payload.get("suggestion")
            detail = f"Tripo error code {code}: {message}"
            if suggestion:
                detail += f" ({suggestion})"
            request_id = payload.get("request_id")
            if request_id:
                detail += f" [request_id={request_id}]"

            if code in _RETRYABLE_CODES and attempt + 1 < self.config.max_retries:
                last_error = detail
                self._sleep_backoff(attempt, response=response)
                continue
            raise ProviderCallError(detail + _fatal_hint(code))

        raise ProviderCallError(
            f"Tripo request {method} {path} failed after "
            f"{self.config.max_retries} attempts. Last error: {last_error}"
        )

    @staticmethod
    def _decode(response: httpx.Response) -> dict[str, Any]:
        try:
            payload = response.json()
        except ValueError as exc:
            raise ProviderCallError(
                f"Tripo returned a non-JSON response (HTTP "
                f"{response.status_code}): {response.text[:200]}"
            ) from exc
        if not isinstance(payload, dict):
            raise ProviderCallError(
                f"Tripo returned an unexpected response shape: {payload!r}"
            )
        return payload

    def _sleep_backoff(
        self, attempt: int, *, response: httpx.Response | None = None
    ) -> None:
        # Honour Retry-After when the server sends one; otherwise exponential
        # backoff with jitter so parallel callers don't resynchronise.
        if response is not None:
            retry_after = response.headers.get("Retry-After")
            if retry_after:
                try:
                    time.sleep(min(60.0, float(retry_after)))
                    return
                except ValueError:
                    pass
        time.sleep(min(30.0, (2.0**attempt) + random.uniform(0.0, 0.5)))

    # ------------------------------------------------------------------
    # Upload / create / poll / download
    # ------------------------------------------------------------------
    def _upload_file(self, path: str | os.PathLike[str]) -> str:
        """Upload one file and return its ``file_<uuid>`` token.

        The response key is ``data.file_token``; V2's ``data.image_token`` no
        longer exists, and V2-issued bare-UUID tokens do not resolve on V3.
        """

        src = Path(path)
        if not src.is_file():
            raise ProviderCallError(f"Tripo upload source not found: {src}")

        self._pace_upload()
        with src.open("rb") as handle:
            data = self._request(
                "POST",
                "/files",
                files={"file": (src.name, handle, _guess_mime(src))},
            )
        self._last_upload_at = time.monotonic()

        token = data.get("file_token")
        if not token:
            raise ProviderCallError(
                f"Tripo upload of {src.name} returned no file_token: {data!r}"
            )
        return str(token)

    def _pace_upload(self) -> None:
        if self._last_upload_at <= 0.0:
            return
        elapsed = time.monotonic() - self._last_upload_at
        wait = self.config.min_upload_interval_s - elapsed
        if wait > 0:
            time.sleep(wait)

    def _create_task(self, path: str, body: dict[str, Any]) -> str:
        data = self._request("POST", path, json_body=body)
        task_id = data.get("task_id")
        if not task_id:
            raise ProviderCallError(
                f"Tripo {path} returned no task_id: {data!r}"
            )
        return str(task_id)

    def _poll(self, task_id: str) -> dict[str, Any]:
        """Poll until the task reaches a terminal state, then return it.

        Polling GETs do not consume the creation rate-limit quota, so a
        few-second interval is fine.
        """

        deadline = time.monotonic() + self.config.timeout_s
        while True:
            task = self._request("GET", f"/tasks/{task_id}")
            status = str(task.get("status") or "").lower()
            # Anything unrecognised is treated as terminal-failed rather than
            # polled forever; V3's enum is only 5 values but "unknown" can
            # still leak through in rare edge cases.
            if status in _TERMINAL_STATUSES or status not in {"queued", "running"}:
                return task
            if time.monotonic() >= deadline:
                raise ProviderCallError(
                    f"Tripo task {task_id} did not finish within "
                    f"{self.config.timeout_s:.0f}s (last status: {status})."
                )
            time.sleep(self.config.poll_interval_s)

    def _await_and_download(
        self, task_id: str, out_dir: str | os.PathLike[str]
    ) -> TripoTaskResult:
        task = self._poll(task_id)
        status = str(task.get("status") or "unknown").lower()

        if status != "success":
            # V2's ``banned`` / ``expired`` statuses are now ``failed`` with
            # error_code 2008 (moderation) / 2018 (queue timeout), so the
            # code is the only way to tell those cases apart.
            code = task.get("error_code")
            message = task.get("error_message") or "no error_message returned"
            raise ProviderCallError(
                f"Tripo task {task_id} finished with status {status!r} "
                f"(error_code={code}): {message}"
            )

        output = task.get("output")
        output = output if isinstance(output, dict) else {}
        # V3 merges V2's pbr_model / model / base_model into ONE key.
        model_url = output.get("model_url")
        if not model_url:
            raise ProviderCallError(
                f"Tripo task {task_id} succeeded but returned no "
                f"output.model_url: {output!r}"
            )

        path = self._download(str(model_url), out_dir)
        return TripoTaskResult(
            task_id=task_id,
            status=status,
            files={"model": path},
            raw_task=task,
        )

    def _download(self, url: str, out_dir: str | os.PathLike[str]) -> Path:
        """Stream a signed output URL to disk.

        Output URLs are signed and expire in roughly 24 hours, so we always
        download immediately rather than storing the URL.
        """

        target_dir = Path(out_dir)
        target_dir.mkdir(parents=True, exist_ok=True)
        suffix = Path(urlsplit(url).path).suffix.lower() or ".glb"
        # Named so the pipeline's "is there already a GLB here?" checks in
        # stage_tripo_models / stage_asset_normalize (which prefer *pbr* then
        # *model* filenames) pick it up.
        target = target_dir / f"model{suffix}"

        try:
            with httpx.Client(timeout=httpx.Timeout(300.0, connect=15.0)) as dl:
                with dl.stream("GET", url) as response:
                    response.raise_for_status()
                    with target.open("wb") as handle:
                        for chunk in response.iter_bytes():
                            handle.write(chunk)
        except httpx.HTTPError as exc:
            raise ProviderCallError(
                f"Failed to download Tripo model from {url[:120]}: "
                f"{type(exc).__name__}: {exc}"
            ) from exc
        return target

    # ------------------------------------------------------------------
    # Misc
    # ------------------------------------------------------------------
    def _require_key(self) -> str:
        if not self.config.api_key:
            raise ProviderConfigError("TRIPO_API_KEY is not set.")
        return self.config.api_key


def _fatal_hint(code: Any) -> str:
    """Actionable next step for the error codes that never resolve on retry."""

    return {
        1004: (
            " -- malformed request. Most often an unsupported 'model' value;"
            f" V3 accepts {', '.join(sorted(_V3_MODEL_VERSIONS))}."
        ),
        2008: (
            " -- content moderation rejected this input or output. Rewrite the"
            " asset prompt; retrying the same request will not help."
        ),
        2010: (
            " -- insufficient Tripo credits. Top up at"
            " https://developers.tripo3d.ai then resume."
        ),
    }.get(code if isinstance(code, int) else -1, "")


def _guess_mime(path: Path) -> str:
    """Content type for the multipart upload part.

    V3 accepts webp/png/jpeg/bmp/tiff for images (plus glb/obj/fbx/stl/3mf for
    models); we only ever upload the Gemini reference render.
    """

    return {
        ".png": "image/png",
        ".jpg": "image/jpeg",
        ".jpeg": "image/jpeg",
        ".webp": "image/webp",
        ".bmp": "image/bmp",
        ".tif": "image/tiff",
        ".tiff": "image/tiff",
    }.get(path.suffix.lower(), "application/octet-stream")
