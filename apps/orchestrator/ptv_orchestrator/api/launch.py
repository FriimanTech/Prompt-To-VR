"""Local launcher: starts the built executable."""

from __future__ import annotations

import os
import shutil
import subprocess
from pathlib import Path


def launch_exe(exe: Path, *, scene_json: Path | None = None) -> int:
    if not exe.exists():
        raise FileNotFoundError(f"Executable not found: {exe}")
    env = {**os.environ}
    if scene_json is not None:
        env["PTV_SCENE_JSON"] = str(scene_json)
    # DETACHED: we do not wait; the GUI runs in its own window.
    subprocess.Popen([str(exe)], cwd=str(exe.parent), env=env)
    return 0


def find_starter_exe(repo_root: Path, config: str = "Release") -> Path | None:
    """Find the starter-room exe under any build folder."""
    for root in (
        repo_root / "engine" / "build",
        repo_root / "engine" / "out",
    ):
        if not root.exists():
            continue
        for p in root.rglob("ptv_starter_room.exe"):
            return p
    return None
