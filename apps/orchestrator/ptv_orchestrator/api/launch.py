"""Local launcher: starts the built executable.

The built runtime is a generic VR host that loads whatever ``scene.json``
is handed to it. For orchestrator-built projects the exe is named after the
project's title (e.g. ``GREEN LANTERN WILLPOWER VR.exe``) via ``-DPTV_APP_NAME``
so it's identifiable in Windows / Quest Link; the hand-built starter-room
sample keeps the ``ptv_starter_room.exe`` name. CMake POST_BUILD copies
the starter-room's own ``scene.json`` next to the exe as a convenience
for double-clicking, which means that if we launch the exe with no
arguments we always get the starter room -- *even when we set the
PTV_SCENE_JSON env var*, because the runtime's ``main.cpp`` assigns
the sidecar path into ``Config::scene_json_path`` first.

To guarantee "Launch latest build" opens the *project's* scene, we
pass the scene path as ``argv[1]`` (the app's top-priority source)
and also set ``PTV_SCENE_JSON`` as a belt-and-braces fallback for any
future custom entry points that don't parse argv.
"""

from __future__ import annotations

import os
import shutil
import subprocess
from pathlib import Path


def launch_exe(exe: Path, *, scene_json: Path | None = None) -> int:
    if not exe.exists():
        raise FileNotFoundError(f"Executable not found: {exe}")
    env = {**os.environ}
    argv: list[str] = [str(exe)]
    if scene_json is not None:
        scene_str = str(scene_json)
        env["PTV_SCENE_JSON"] = scene_str
        argv.append(scene_str)
    # DETACHED: we do not wait; the GUI runs in its own window.
    subprocess.Popen(argv, cwd=str(exe.parent), env=env)
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


def _adb(adb_path: str | None) -> str:
    """Resolve the adb executable (settings override -> PATH)."""
    if adb_path:
        return adb_path
    return shutil.which("adb") or "adb"


def install_and_launch_apk(
    apk: Path,
    *,
    package: str,
    adb_path: str | None = None,
) -> int:
    """Install (``adb install -r``) then launch (``am start`` the
    NativeActivity) a Quest APK on the connected headset.

    The headset must be in Developer Mode, connected over USB, and authorized
    (``adb devices`` shows it). Returns 0 on success; raises on adb failure.
    """
    if not apk.exists():
        raise FileNotFoundError(f"APK not found: {apk}")
    adb = _adb(adb_path)

    install = subprocess.run(
        [adb, "install", "-r", str(apk)],
        capture_output=True,
        text=True,
    )
    if install.returncode != 0:
        raise RuntimeError(
            f"adb install failed (exit={install.returncode}): "
            f"{(install.stderr or install.stdout)[:500]}"
        )

    # NativeActivity is the launch component (matches AndroidManifest.xml).
    component = f"{package}/android.app.NativeActivity"
    start = subprocess.run(
        [adb, "shell", "am", "start", "-n", component],
        capture_output=True,
        text=True,
    )
    if start.returncode != 0:
        raise RuntimeError(
            f"adb am start failed (exit={start.returncode}): "
            f"{(start.stderr or start.stdout)[:500]}"
        )
    return 0
