"""Uthana wrapper: auto-rig characters and build their locomotion motions.

Uthana fills the gap Tripo leaves for humanoid characters: its auto-rigger
produces finger joints (so the player's avatar hands can curl), and its
locomotion generator makes a gait in any of ~110 styles at a chosen speed.

Two transport paths, chosen per call:

* ``create_character`` posts the documented GraphQL multipart mutation
  directly, because the published ``uthana`` client (0.5.1) still does not
  expose ``auto_rig_strip_namespace`` or ``auto_rig_feet_on_floor``
  (https://uthana.com/docs/api/capabilities/auto-rig-and-add-character).
* Everything else goes through the official SDK's ``*_sync`` methods.

The API key comes from ``UTHANA_API_KEY``, loaded from the repo-root ``.env``
with python-dotenv when the caller does not pass one.

Billing: generation is billed per generated second; downloads (and therefore
retargeting an existing motion onto another character) are free. That is why
every motion is named canonically and looked up before anything is created.
"""

from __future__ import annotations

import json
import os
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Optional

import httpx

from .errors import ProviderConfigError

GRAPHQL_URL = "https://uthana.com/graphql"

#: Uthana rejects uploads at or above this size.
MAX_UPLOAD_BYTES = 30 * 1024 * 1024

_CREATE_CHARACTER = """
mutation CreateCharacter(
    $name: String!,
    $file: Upload!,
    $auto_rig: Boolean,
    $auto_rig_front_facing: Boolean,
    $rerig_target: String,
    $include_fingers: Boolean,
    $auto_rig_strip_namespace: Boolean,
    $auto_rig_feet_on_floor: Boolean
) {
    create_character(
        name: $name,
        file: $file,
        auto_rig: $auto_rig,
        auto_rig_front_facing: $auto_rig_front_facing,
        rerig_target: $rerig_target,
        include_fingers: $include_fingers,
        auto_rig_strip_namespace: $auto_rig_strip_namespace,
        auto_rig_feet_on_floor: $auto_rig_feet_on_floor
    ) {
        character { id name }
        auto_rig_confidence
    }
}
"""

# Known error messages from the docs, paired with what to do about them.
_HINTS = (
    ("must be less than", "The GLB is over Uthana's 30 MB upload limit."),
    ("Maximum number of characters", "Your Uthana organisation has reached "
     "its character limit. Delete unused characters at uthana.com or upgrade."),
    ("Essential joints missing", "Uthana could not find a humanoid skeleton "
     "(pelvis, shoulders, hips). The model is probably not a biped humanoid."),
    ("Auto-rigging failed", "Uthana's auto-rigger rejected this mesh "
     "(non-humanoid shape, intersecting limbs, flowing clothing, or large "
     "extra appendages)."),
    ("not done after", "Auto-rigging timed out. A smaller or simpler mesh "
     "uploads faster."),
)


class UthanaClientError(RuntimeError):
    """A Uthana call failed. ``hint`` says what to do about it, when known."""

    def __init__(self, message: str, *, hint: str = "") -> None:
        super().__init__(f"{message}" + (f" -- {hint}" if hint else ""))
        self.hint = hint


def _hint_for(text: str) -> str:
    for needle, hint in _HINTS:
        if needle.lower() in text.lower():
            return hint
    return ""


def _repo_root() -> Path:
    here = Path(__file__).resolve()
    for candidate in here.parents:
        if (candidate / "AGENTS.md").is_file() and (candidate / "engine").is_dir():
            return candidate
    return here.parents[3]


def load_uthana_api_key(explicit: Optional[str] = None) -> str:
    """``explicit``, else ``UTHANA_API_KEY`` from the env or the repo .env."""
    if explicit and explicit.strip():
        return explicit.strip()
    key = (os.environ.get("UTHANA_API_KEY") or "").strip()
    if key:
        return key
    try:
        from dotenv import load_dotenv
    except ImportError:  # pragma: no cover - python-dotenv is a dependency
        load_dotenv = None  # type: ignore[assignment]
    if load_dotenv is not None:
        load_dotenv(_repo_root() / ".env", override=False)
    key = (os.environ.get("UTHANA_API_KEY") or "").strip()
    if not key:
        raise ProviderConfigError(
            "UTHANA_API_KEY is not set. Add UTHANA_API_KEY=... to the "
            "repo-root .env."
        )
    return key


def field_of(item: Any, key: str) -> Any:
    """Read ``key`` from an SDK result that may be a dict or an object."""
    if item is None:
        return None
    if isinstance(item, dict):
        return item.get(key)
    try:
        return item[key]  # type: ignore[index]
    except (KeyError, TypeError, IndexError):
        pass
    return getattr(item, key, None)


@dataclass
class AutoRigFlags:
    """The five auto-rig parameters, all on for VR characters."""

    auto_rig: bool = True
    front_facing: bool = True
    include_fingers: bool = True
    strip_namespace: bool = True
    feet_on_floor: bool = True

    def as_variables(self) -> dict[str, Any]:
        return {
            "auto_rig": self.auto_rig,
            "auto_rig_front_facing": self.front_facing,
            "rerig_target": None,
            "include_fingers": self.include_fingers,
            "auto_rig_strip_namespace": self.strip_namespace,
            "auto_rig_feet_on_floor": self.feet_on_floor,
        }


@dataclass
class CreatedCharacter:
    character_id: str
    name: str
    auto_rig_confidence: Optional[float]
    raw: dict[str, Any] = field(default_factory=dict)


@dataclass
class MotionRecord:
    motion_id: str
    name: str


class UthanaClient:
    """Synchronous facade over the Uthana API, used by the pipeline stages."""

    def __init__(
        self,
        api_key: Optional[str] = None,
        *,
        timeout_s: float = 360.0,
        sdk: Any = None,
        http: Optional[httpx.Client] = None,
    ) -> None:
        self.api_key = load_uthana_api_key(api_key)
        self.timeout_s = timeout_s
        self._sdk = sdk
        self._http = http

    # ------------------------------------------------------------------
    @property
    def sdk(self) -> Any:
        if self._sdk is None:
            try:
                from uthana import Uthana
            except ImportError as exc:  # pragma: no cover - real runs only
                raise ProviderConfigError(
                    "The 'uthana' package is not installed. Install "
                    "'uthana>=0.5.1' into the project venv."
                ) from exc
            self._sdk = Uthana(self.api_key)
        return self._sdk

    def _call(self, what: str, fn, *args, **kwargs) -> Any:
        try:
            return fn(*args, **kwargs)
        except UthanaClientError:
            raise
        except Exception as exc:  # noqa: BLE001 - normalised for the stages
            text = str(getattr(exc, "api_message", "") or exc)
            raise UthanaClientError(
                f"Uthana {what} failed: {text}", hint=_hint_for(text)
            ) from exc

    # ------------------------------------------------------------------
    # Characters
    # ------------------------------------------------------------------
    def create_character(
        self, path: Path, name: str, flags: Optional[AutoRigFlags] = None
    ) -> CreatedCharacter:
        """Upload a GLB and auto-rig it. Returns the character id.

        Uthana de-duplicates identical uploads and returns the FIRST
        character id for a repeated file, with that first upload's rig. The
        caller therefore never assumes the flags it sent took effect.
        """
        flags = flags or AutoRigFlags()
        path = Path(path)
        size = path.stat().st_size
        if size >= MAX_UPLOAD_BYTES:
            raise UthanaClientError(
                f"{path.name} is {size / 1e6:.1f} MB",
                hint=_hint_for("must be less than"),
            )
        variables = {"name": name, "file": None, **flags.as_variables()}
        operations = json.dumps({"query": _CREATE_CHARACTER, "variables": variables})
        map_data = json.dumps({"0": ["variables.file"]})

        client = self._http or httpx.Client(timeout=self.timeout_s)
        try:
            with path.open("rb") as handle:
                response = client.post(
                    GRAPHQL_URL,
                    auth=(self.api_key, ""),
                    data={"operations": operations, "map": map_data},
                    files={"0": (path.name, handle, "application/octet-stream")},
                )
        except httpx.HTTPError as exc:
            raise UthanaClientError(f"Uthana upload failed: {exc}") from exc
        finally:
            if self._http is None:
                client.close()

        try:
            payload = response.json()
        except ValueError:
            payload = {"raw": response.text[:500]}
        errors = payload.get("errors") if isinstance(payload, dict) else None
        if response.status_code >= 400 or errors:
            text = json.dumps(errors or payload)[:1500]
            raise UthanaClientError(
                f"Uthana create_character failed (HTTP {response.status_code}): {text}",
                hint=_hint_for(text),
            )
        data = (payload.get("data") or {}).get("create_character") or {}
        character = data.get("character") or {}
        character_id = character.get("id")
        if not character_id:
            raise UthanaClientError(f"No character id in response: {payload}")
        confidence = data.get("auto_rig_confidence")
        return CreatedCharacter(
            character_id=str(character_id),
            name=str(character.get("name") or name),
            auto_rig_confidence=(
                float(confidence) if isinstance(confidence, (int, float)) else None
            ),
            raw=data,
        )

    def download_character(self, character_id: str) -> bytes:
        return self._call(
            "character download",
            self.sdk.characters.download_sync,
            character_id,
            output_format="glb",
        )

    # ------------------------------------------------------------------
    # Motions
    # ------------------------------------------------------------------
    def list_locomotion_styles(self) -> list[str]:
        styles = self._call(
            "locomotion style query", self.sdk.motions.list_locomotion_styles_sync
        )
        return [str(s) for s in (styles or [])]

    def list_motions(self, limit: Optional[int] = 500) -> list[MotionRecord]:
        """Motions in the organisation, newest data straight from the API.

        ``limit`` mirrors the documented ``motions[:N]`` slice; ``None`` keeps
        everything.
        """
        motions = self._call("motion list", self.sdk.motions.list_sync) or []
        subset = motions if limit is None else motions[:limit]
        out: list[MotionRecord] = []
        for m in subset:
            mid = field_of(m, "id")
            if not mid:
                continue
            out.append(MotionRecord(motion_id=str(mid), name=str(field_of(m, "name") or "")))
        return out

    def count_motions(self) -> int:
        return len(self._call("motion list", self.sdk.motions.list_sync) or [])

    def get_motion(self, motion_id: str) -> Optional[MotionRecord]:
        motion = self._call("motion lookup", self.sdk.motions.get_sync, motion_id)
        if not motion:
            return None
        mid = field_of(motion, "id") or motion_id
        return MotionRecord(motion_id=str(mid), name=str(field_of(motion, "name") or ""))

    def create_locomotion(
        self,
        character_id: str,
        *,
        strides: int,
        move_speed: float,
        style_id: str,
        travel_angle: float,
    ) -> str:
        """Generate one locomotion clip. Returns its motion id."""
        result = self._call(
            "create_locomotion",
            self.sdk.motions.create_locomotion_sync,
            character_id,
            strides=strides,
            move_speed=move_speed,
            style_id=style_id,
            travel_angle=travel_angle,
        )
        motion_id = field_of(result, "motion_id")
        if not motion_id:
            raise UthanaClientError(f"No motion_id in create_locomotion response: {result!r}")
        return str(motion_id)

    def rename_motion(self, motion_id: str, new_name: str) -> None:
        """Rename via the documented ``update_motion`` mutation.

        Posted directly rather than through ``motions.rename_sync``: the
        published SDK (0.5.1) sends ``update_motion(id: ...)`` and asks for
        fields the schema does not have, so every rename fails with HTTP 400
        (https://uthana.com/docs/api/capabilities/asset-management#rename-a-motion).
        """
        data = self._graphql(
            "motion rename",
            "mutation UpdateMotion($motion_id: String!, $name: String) "
            "{ update_motion(motion_id: $motion_id, name: $name) { motion { id name } } }",
            {"motion_id": motion_id, "name": new_name},
        )
        motion = ((data.get("update_motion") or {}).get("motion") or {})
        if motion.get("name") != new_name:
            raise UthanaClientError(
                f"Uthana rename of {motion_id} returned {motion!r}, expected name {new_name!r}"
            )

    def _graphql(self, what: str, query: str, variables: dict[str, Any]) -> dict[str, Any]:
        client = self._http or httpx.Client(timeout=self.timeout_s)
        try:
            response = client.post(
                GRAPHQL_URL,
                auth=(self.api_key, ""),
                json={"query": query, "variables": variables},
            )
        except httpx.HTTPError as exc:
            raise UthanaClientError(f"Uthana {what} failed: {exc}") from exc
        finally:
            if self._http is None:
                client.close()
        try:
            payload = response.json()
        except ValueError:
            payload = {"raw": response.text[:500]}
        errors = payload.get("errors") if isinstance(payload, dict) else None
        if response.status_code >= 400 or errors:
            text = json.dumps(errors or payload)[:1500]
            raise UthanaClientError(
                f"Uthana {what} failed (HTTP {response.status_code}): {text}",
                hint=_hint_for(text),
            )
        return payload.get("data") or {}

    def is_download_allowed(self, character_id: str, motion_id: str) -> bool:
        """True unless the account's download quota says otherwise.

        Pay-as-you-go accounts have unlimited downloads; the check matters for
        legacy subscription plans. An unreadable answer is treated as allowed
        so a transient query failure does not block a free download.
        """
        try:
            answer = self.sdk.motions.download_allowed_sync(motion_id, character_id)
        except Exception:  # noqa: BLE001
            return True
        allowed = field_of(answer, "allowed")
        if allowed is None:
            allowed = field_of(answer, "is_allowed")
        return True if allowed is None else bool(allowed)

    def download_motion(
        self,
        character_id: str,
        motion_id: str,
        *,
        no_mesh: bool = True,
        in_place: bool = True,
        fps: int = 30,
    ) -> bytes:
        """Download ``motion_id`` retargeted onto ``character_id`` as GLB.

        Always ``no_mesh`` (animation only; the mesh comes from the rigged
        character) and ``in_place`` (the engine moves the body itself).
        """
        return self._call(
            "motion download",
            self.sdk.motions.download_sync,
            character_id,
            motion_id,
            output_format="glb",
            fps=fps,
            no_mesh=no_mesh,
            in_place=in_place,
        )
