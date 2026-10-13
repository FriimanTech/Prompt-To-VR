"""Shared helpers for the Sequential_Runs Uthana R&A scripts.

Each script keeps its own USER CONFIGURATION block. This module only covers
repo-root ``.env`` loading, API-key checks, and client construction.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path
from typing import NoReturn

_THIS_DIR = Path(__file__).resolve().parent


def _find_repo_root() -> Path:
    """Walk up from this file to the Prompt-To-VR repo (the folder with .env)."""
    for candidate in (_THIS_DIR, *_THIS_DIR.parents):
        if (candidate / "AGENTS.md").is_file() and (candidate / "engine").is_dir():
            return candidate
        if (candidate / "Sequential_Runs").is_dir() and (candidate / ".env").is_file():
            return candidate
    # Sequential_Runs/Uthana_R&A -> Sequential_Runs -> repo
    return _THIS_DIR.parent.parent


REPO_ROOT = _find_repo_root()


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


def load_repo_dotenv() -> None:
    """Load repo-root .env into os.environ (does not override existing vars)."""
    env_path = REPO_ROOT / ".env"
    if not env_path.is_file():
        return
    try:
        from dotenv import load_dotenv
    except ImportError:
        for name, value in _parse_env_file(env_path).items():
            os.environ.setdefault(name, value)
        return
    load_dotenv(env_path, override=False)
    if not (os.environ.get("UTHANA_API_KEY") or "").strip():
        for name, value in _parse_env_file(env_path).items():
            os.environ.setdefault(name, value)


def require_uthana_api_key() -> str:
    """Return UTHANA_API_KEY from the environment or the repo-root .env."""
    key = (os.environ.get("UTHANA_API_KEY") or "").strip()
    if key:
        return key

    load_repo_dotenv()
    key = (os.environ.get("UTHANA_API_KEY") or "").strip()
    if not key:
        env_path = REPO_ROOT / ".env"
        raise SystemExit(
            "UTHANA_API_KEY is not set. Put UTHANA_API_KEY=... in the "
            f"repo-root .env ({env_path}) or export it in your shell."
        )
    os.environ.setdefault("UTHANA_API_KEY", key)
    return key


_MIN_UTHANA_VERSION = (0, 5, 1)


def _parse_version(raw: str) -> tuple[int, ...]:
    parts: list[int] = []
    for piece in raw.split("."):
        digits = "".join(ch for ch in piece if ch.isdigit())
        if not digits:
            break
        parts.append(int(digits))
    return tuple(parts) if parts else (0,)


def make_client():
    """Build an authenticated ``Uthana`` client from the repo-root .env."""
    try:
        from uthana import Uthana
    except ImportError as exc:
        raise SystemExit(
            "The 'uthana' package is not installed for this Python.\n"
            "Activate the project venv and install into it (not base Python):\n"
            "  .\\vrvenv\\Scripts\\Activate.ps1\n"
            "  uv pip install -e .\\packages\\schemas "
            "-e .\\packages\\provider-clients "
            '-e ".\\apps\\orchestrator[dev]"\n'
            '  uv pip install "uthana>=0.5.1"\n'
            f"({exc})"
        ) from exc

    try:
        from importlib.metadata import version as pkg_version
    except ImportError:  # pragma: no cover
        from importlib_metadata import version as pkg_version  # type: ignore

    installed = pkg_version("uthana")
    if _parse_version(installed) < _MIN_UTHANA_VERSION:
        raise SystemExit(
            f"uthana {installed} is too old (need >=0.5.1 for in_place / "
            "no_mesh download options).\n"
            "Upgrade inside vrvenv:\n"
            '  uv pip install "uthana>=0.5.1"'
        )

    return Uthana(require_uthana_api_key())


def require_file(path: Path, *, label: str = "file") -> Path:
    """Resolve ``path`` and exit if it is missing."""
    resolved = path.expanduser().resolve()
    if not resolved.is_file():
        raise SystemExit(f"{label} not found: {resolved}")
    return resolved


def fail(message: str) -> NoReturn:
    print(message, file=sys.stderr)
    raise SystemExit(1)
