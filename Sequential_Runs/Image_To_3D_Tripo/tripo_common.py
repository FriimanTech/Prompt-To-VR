"""Tiny shared helpers for the Sequential_Runs Tripo V3 scripts.

Each script still has its own USER CONFIGURATION block. This module only
covers API-key loading, upload, task create/poll, and download so those
pieces are not copied four times.
"""

from __future__ import annotations

import os
import random
import sys
import time
from pathlib import Path
from typing import Any, NoReturn
from urllib.parse import urlsplit

import httpx

DEFAULT_BASE_URL = "https://openapi.tripo3d.ai/v3"
POLL_INTERVAL_S = 2.0
POLL_TIMEOUT_S = 600.0
MAX_RETRIES = 4
RETRYABLE_CODES = frozenset({1007, 2000})
TERMINAL_STATUSES = frozenset({"success", "failed", "cancelled"})

_THIS_DIR = Path(__file__).resolve().parent


def _find_repo_root() -> Path:
    """Walk up from this file to the Prompt-To-VR repo (the folder with .env)."""
    for candidate in (_THIS_DIR, *_THIS_DIR.parents):
        if (candidate / "AGENTS.md").is_file() and (candidate / "engine").is_dir():
            return candidate
        if (candidate / "Sequential_Runs").is_dir() and (candidate / ".env").is_file():
            return candidate
    # Sequential_Runs/Image_To_3D_Tripo -> Sequential_Runs -> repo
    return _THIS_DIR.parent.parent


_REPO_ROOT = _find_repo_root()


class TripoError(RuntimeError):
    """Raised when a Tripo request or task fails."""


def _normalize_base_url(raw: str | None) -> str:
    base = (raw or DEFAULT_BASE_URL).strip().rstrip("/")
    if not base:
        base = DEFAULT_BASE_URL
    if not base.endswith("/v3"):
        base = f"{base}/v3"
    return base


def _parse_env_file(env_path: Path) -> dict[str, str]:
    """Read KEY=VALUE lines from a .env file without requiring python-dotenv."""
    values: dict[str, str] = {}
    for raw in env_path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        if line.lower().startswith("export "):
            line = line[7:].strip()
        name, _, value = line.partition("=")
        name = name.strip()
        value = value.strip().strip('"').strip("'")
        if name:
            values[name] = value
    return values


def _load_repo_dotenv() -> None:
    """Load repo-root .env into os.environ (does not override existing vars)."""
    env_path = _REPO_ROOT / ".env"
    if not env_path.is_file():
        return
    try:
        from dotenv import load_dotenv
    except ImportError:
        parsed = _parse_env_file(env_path)
        for name, value in parsed.items():
            os.environ.setdefault(name, value)
        return
    load_dotenv(env_path, override=False)
    # Some dotenv versions skip blank-looking values; still read TRIPO_API_KEY
    # directly if the environment did not pick it up.
    if not (os.environ.get("TRIPO_API_KEY") or "").strip():
        parsed = _parse_env_file(env_path)
        for name, value in parsed.items():
            os.environ.setdefault(name, value)


def load_api_key() -> str:
    """Return TRIPO_API_KEY from the environment or the repo-root .env."""
    key = (os.environ.get("TRIPO_API_KEY") or "").strip()
    if key:
        return key

    _load_repo_dotenv()
    key = (os.environ.get("TRIPO_API_KEY") or "").strip()
    if not key:
        env_path = _REPO_ROOT / ".env"
        raise TripoError(
            "TRIPO_API_KEY is not set. Put TRIPO_API_KEY=... in the repo-root "
            f".env ({env_path}) or export it in your shell."
        )
    os.environ.setdefault("TRIPO_API_KEY", key)
    return key


def base_url() -> str:
    _load_repo_dotenv()
    return _normalize_base_url(os.environ.get("TRIPO_BASE_URL"))


def _headers(api_key: str) -> dict[str, str]:
    return {"Authorization": f"Bearer {api_key}"}


def _mime_type(path: Path) -> str:
    return {
        ".png": "image/png",
        ".jpg": "image/jpeg",
        ".jpeg": "image/jpeg",
        ".webp": "image/webp",
        ".bmp": "image/bmp",
        ".tif": "image/tiff",
        ".tiff": "image/tiff",
        ".glb": "model/gltf-binary",
        ".gltf": "model/gltf+json",
        ".fbx": "application/octet-stream",
        ".obj": "text/plain",
        ".stl": "application/octet-stream",
    }.get(path.suffix.lower(), "application/octet-stream")


def _sleep_backoff(attempt: int, response: httpx.Response | None = None) -> None:
    if response is not None:
        retry_after = response.headers.get("Retry-After")
        if retry_after:
            try:
                time.sleep(min(60.0, float(retry_after)))
                return
            except ValueError:
                pass
    time.sleep(min(30.0, (2.0**attempt) + random.uniform(0.0, 0.5)))


def _decode(response: httpx.Response) -> dict[str, Any]:
    try:
        payload = response.json()
    except ValueError as exc:
        raise TripoError(
            f"Tripo returned a non-JSON response (HTTP {response.status_code}): "
            f"{response.text[:200]}"
        ) from exc
    if not isinstance(payload, dict):
        raise TripoError(f"Tripo returned an unexpected response shape: {payload!r}")
    return payload


def request(
    method: str,
    path: str,
    *,
    json_body: dict[str, Any] | None = None,
    files: dict[str, Any] | None = None,
) -> dict[str, Any]:
    """Send one V3 request and return the unwrapped ``data`` object."""
    api_key = load_api_key()
    url = f"{base_url()}{path if path.startswith('/') else '/' + path}"
    last_error = ""

    with httpx.Client(timeout=httpx.Timeout(60.0, connect=15.0)) as client:
        for attempt in range(MAX_RETRIES):
            try:
                response = client.request(
                    method,
                    url,
                    headers=_headers(api_key),
                    json=json_body,
                    files=files,
                )
            except httpx.HTTPError as exc:
                last_error = f"{type(exc).__name__}: {exc}"
                if attempt + 1 >= MAX_RETRIES:
                    break
                _sleep_backoff(attempt)
                continue

            if response.status_code == 401:
                raise TripoError(
                    "Tripo rejected the API key (HTTP 401). Check TRIPO_API_KEY "
                    f"and region (client is pointed at {base_url()})."
                )
            if response.status_code == 429 or response.status_code >= 500:
                last_error = f"HTTP {response.status_code}: {response.text[:200]}"
                if attempt + 1 >= MAX_RETRIES:
                    break
                _sleep_backoff(attempt, response)
                continue

            payload = _decode(response)
            code = payload.get("code")
            if code == 0:
                data = payload.get("data")
                return data if isinstance(data, dict) else {}

            message = payload.get("message") or response.text[:200]
            detail = f"Tripo error code {code}: {message}"
            suggestion = payload.get("suggestion")
            if suggestion:
                detail += f" ({suggestion})"
            if code in RETRYABLE_CODES and attempt + 1 < MAX_RETRIES:
                last_error = detail
                _sleep_backoff(attempt, response)
                continue
            raise TripoError(detail)

    raise TripoError(
        f"Tripo request {method} {path} failed after {MAX_RETRIES} attempts. "
        f"Last error: {last_error}"
    )


def require_file(path: Path, *, kind: str) -> Path:
    resolved = path.expanduser()
    if not resolved.is_file():
        raise TripoError(f"{kind} not found: {resolved}")
    return resolved


def upload_file(path: Path, *, label: str = "file") -> str:
    """POST /files and return ``file_token``."""
    src = require_file(path, kind="Upload source")
    print(f"Uploading {label}...")
    print(f"  {src}")
    with src.open("rb") as handle:
        data = request(
            "POST",
            "/files",
            files={"file": (src.name, handle, _mime_type(src))},
        )
    token = data.get("file_token")
    if not token:
        raise TripoError(f"Upload of {src.name} returned no file_token: {data!r}")
    print("Upload complete.")
    print(f"  file_token: {token}")
    return str(token)


def create_task(path: str, body: dict[str, Any]) -> str:
    print("Creating Tripo task...")
    data = request("POST", path, json_body=body)
    task_id = data.get("task_id")
    if not task_id:
        raise TripoError(f"Tripo {path} returned no task_id: {data!r}")
    print(f"Task ID: {task_id}")
    return str(task_id)


def poll_task(task_id: str) -> dict[str, Any]:
    """Poll GET /tasks/{id} until success, failed, or cancelled."""
    deadline = time.monotonic() + POLL_TIMEOUT_S
    last_shown = None
    while True:
        task = request("GET", f"/tasks/{task_id}")
        status = str(task.get("status") or "").lower()
        progress = task.get("progress")
        shown = (status, progress)
        if shown != last_shown:
            if isinstance(progress, (int, float)):
                print(f"Processing: {int(progress)}%  ({status})")
            else:
                print(f"Processing: {status}")
            last_shown = shown

        if status in TERMINAL_STATUSES or status not in {"queued", "running"}:
            if status != "success":
                code = task.get("error_code")
                message = task.get("error_message") or "no error_message returned"
                raise TripoError(
                    f"Tripo task {task_id} finished with status {status!r} "
                    f"(error_code={code}): {message}"
                )
            return task

        if time.monotonic() >= deadline:
            raise TripoError(
                f"Tripo task {task_id} did not finish within "
                f"{POLL_TIMEOUT_S:.0f}s (last status: {status})."
            )
        time.sleep(POLL_INTERVAL_S)


def model_urls_from_output(output: dict[str, Any]) -> list[str]:
    urls: list[str] = []
    single = output.get("model_url")
    if isinstance(single, str) and single:
        urls.append(single)
    many = output.get("model_urls")
    if isinstance(many, list):
        for item in many:
            if isinstance(item, str) and item and item not in urls:
                urls.append(item)
            elif isinstance(item, dict):
                url = item.get("url") or item.get("model_url")
                if isinstance(url, str) and url and url not in urls:
                    urls.append(url)
    return urls


def download_url(url: str, destination: Path) -> Path:
    print("Downloading model...")
    destination.parent.mkdir(parents=True, exist_ok=True)
    suffix = Path(urlsplit(url).path).suffix.lower()
    if suffix and destination.suffix.lower() != suffix:
        destination = destination.with_suffix(suffix)

    try:
        with httpx.Client(timeout=httpx.Timeout(300.0, connect=15.0)) as client:
            with client.stream("GET", url) as response:
                response.raise_for_status()
                with destination.open("wb") as handle:
                    for chunk in response.iter_bytes():
                        handle.write(chunk)
    except httpx.HTTPError as exc:
        raise TripoError(
            f"Failed to download {url[:120]}: {type(exc).__name__}: {exc}"
        ) from exc
    return destination


def download_task_models(
    task: dict[str, Any],
    output_directory: Path,
    *,
    filename_stem: str,
) -> list[Path]:
    output = task.get("output")
    output = output if isinstance(output, dict) else {}
    urls = model_urls_from_output(output)
    if not urls:
        raise TripoError(
            f"Task succeeded but returned no output.model_url / model_urls: {output!r}"
        )
    output_directory.mkdir(parents=True, exist_ok=True)
    saved: list[Path] = []
    for index, url in enumerate(urls):
        stem = filename_stem if len(urls) == 1 else f"{filename_stem}_{index + 1}"
        path = download_url(url, output_directory / f"{stem}.glb")
        saved.append(path)
        print("Saved to:")
        print(path)
    return saved


def fail(message: str, code: int = 1) -> NoReturn:
    print(f"ERROR: {message}", file=sys.stderr)
    raise SystemExit(code)
