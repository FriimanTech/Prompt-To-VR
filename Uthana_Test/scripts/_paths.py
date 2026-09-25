"""Shared paths / .env loading for Uthana_Test scripts."""

from __future__ import annotations

import os
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
UTHANA_TEST_ROOT = REPO_ROOT / "Uthana_Test"
PROJECTS_DIR = UTHANA_TEST_ROOT / "projects"
PROJECT_ID = "uthana-avatar-rig-test"
PROJECT_DIR = PROJECTS_DIR / PROJECT_ID

SOURCE_AVATAR_GLB = (
    REPO_ROOT
    / "projects"
    / "20260907-144543-shooting-range-game"
    / "assets"
    / "player_range_avatar"
    / "model.glb"
)
SOURCE_PISTOL_GLB = (
    REPO_ROOT
    / "projects"
    / "20260907-144543-shooting-range-game"
    / "assets"
    / "range_pistol"
    / "model.glb"
)

AVATAR_ASSET_DIR = PROJECT_DIR / "assets" / "player_range_avatar"
PISTOL_ASSET_DIR = PROJECT_DIR / "assets" / "range_pistol"
RIGGED_AVATAR_GLB = AVATAR_ASSET_DIR / "model.glb"
UTHANA_RIG_META = AVATAR_ASSET_DIR / "uthana_rig.json"


def load_repo_env() -> None:
    """Load repo-root ``.env`` into ``os.environ`` without overriding existing vars."""
    env_path = REPO_ROOT / ".env"
    if not env_path.is_file():
        return
    try:
        from dotenv import load_dotenv
    except ImportError:
        # Minimal fallback: KEY=VALUE lines only.
        for line in env_path.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, _, value = line.partition("=")
            key = key.strip()
            value = value.strip().strip('"').strip("'")
            if key and key not in os.environ:
                os.environ[key] = value
        return
    load_dotenv(env_path, override=False)


def require_uthana_api_key() -> str:
    load_repo_env()
    key = (os.environ.get("UTHANA_API_KEY") or "").strip()
    if not key:
        raise SystemExit(
            "UTHANA_API_KEY missing. Set it in the repo-root .env "
            f"({REPO_ROOT / '.env'})."
        )
    return key
