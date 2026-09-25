#!/usr/bin/env python3
"""Seed the Uthana_Test project tree and run quest_build (no LLM stages).

Uses a ProjectStore rooted at Uthana_Test/projects so the main repo
``projects/`` directory and global PROJECTS_DIR are untouched.
"""

from __future__ import annotations

import shutil
import sys
from pathlib import Path

_SCRIPTS = Path(__file__).resolve().parent
_REPO = _SCRIPTS.parents[1]
if str(_SCRIPTS) not in sys.path:
    sys.path.insert(0, str(_SCRIPTS))

# Editable / src layouts for orchestrator + schemas.
for extra in (
    _REPO / "apps" / "orchestrator",
    _REPO / "packages" / "schemas",
):
    s = str(extra)
    if s not in sys.path:
        sys.path.insert(0, s)

from _paths import (  # noqa: E402
    PISTOL_ASSET_DIR,
    PROJECT_DIR,
    PROJECT_ID,
    PROJECTS_DIR,
    RIGGED_AVATAR_GLB,
    SOURCE_PISTOL_GLB,
)

from ptv_orchestrator.events import EventBus  # noqa: E402
from ptv_orchestrator.pipeline.context import PipelineContext  # noqa: E402
from ptv_orchestrator.pipeline import stage_quest_build  # noqa: E402
from ptv_orchestrator.settings import Settings, get_settings  # noqa: E402
from ptv_orchestrator.storage import ProjectStore  # noqa: E402
from ptv_schemas import (  # noqa: E402
    ProjectStage,
    ProjectState,
    ProjectStatus,
    TargetDevice,
)


def _ensure_assets() -> None:
    if not RIGGED_AVATAR_GLB.is_file():
        raise SystemExit(
            f"Missing rigged avatar GLB: {RIGGED_AVATAR_GLB}\n"
            "Run: python Uthana_Test/scripts/rig_avatar.py"
        )
    PISTOL_ASSET_DIR.mkdir(parents=True, exist_ok=True)
    dest = PISTOL_ASSET_DIR / "model.glb"
    if not dest.is_file():
        if not SOURCE_PISTOL_GLB.is_file():
            raise SystemExit(f"Source pistol missing: {SOURCE_PISTOL_GLB}")
        shutil.copy2(SOURCE_PISTOL_GLB, dest)
        print(f"Copied pistol -> {dest}")

    scene = PROJECT_DIR / "scene.json"
    if not scene.is_file():
        raise SystemExit(f"Missing hand-authored scene.json: {scene}")


def _ensure_state(store: ProjectStore) -> ProjectState:
    try:
        state = store.load_state(PROJECT_ID)
    except FileNotFoundError:
        state = ProjectState(
            project_id=PROJECT_ID,
            title="Uthana Avatar Rig Test",
            prompt=(
                "Hand-authored Uthana auto-rig first-person avatar Quest "
                "playtest. No LLM stages."
            ),
            target_device=TargetDevice.quest,
            current_stage=ProjectStage.scene_compile,
            status=ProjectStatus.idle,
            latest_scene_json="scene.json",
        )
    else:
        state.target_device = TargetDevice.quest
        state.latest_scene_json = "scene.json"
        state.status = ProjectStatus.idle
    store.save_state(state)
    (PROJECT_DIR / "prompt.txt").write_text(state.prompt, encoding="utf-8")
    for sub in ("images", "assets", "generated_code", "logs", "builds"):
        (PROJECT_DIR / sub).mkdir(parents=True, exist_ok=True)
    return state


class _PrintBus(EventBus):
    """Print stage events while still forwarding to any async subscribers."""

    def publish(self, event) -> None:  # noqa: ANN001
        print(f"[{event.stage}] {event.status}: {event.message}")
        super().publish(event)


def main() -> int:
    _ensure_assets()
    PROJECTS_DIR.mkdir(parents=True, exist_ok=True)

    # Keep Settings.repo_root / engine_dir at the real repo; only the store
    # points at Uthana_Test/projects.
    settings = get_settings()
    # Force a fresh Settings if PROJECTS_DIR was cached differently.
    if not isinstance(settings, Settings):
        settings = Settings()

    store = ProjectStore(PROJECTS_DIR)
    _ensure_state(store)

    bus = _PrintBus()

    ctx = PipelineContext(
        project_id=PROJECT_ID,
        settings=settings,
        store=store,
        bus=bus,
        dry_run=False,
    )

    print(f"Building Quest APK for {PROJECT_ID}")
    print(f"  project_dir = {ctx.project_dir}")
    print(f"  scene.json  = {ctx.project_dir / 'scene.json'}")
    print(f"  repo_root   = {settings.repo_root}")

    result = stage_quest_build.run(ctx)
    state = store.load_state(PROJECT_ID)
    apk = state.latest_build_apk
    print("---")
    print(f"quest_build artifacts: {result.artifacts}")
    if apk:
        from ptv_orchestrator.quest_ids import quest_package_name

        package = quest_package_name(
            settings.android_package_prefix, PROJECT_ID
        )
        print(f"APK: {apk}")
        print("Install / launch:")
        print(f'  adb install -r "{apk}"')
        print(
            f"  adb shell am start -n {package}/android.app.NativeActivity"
        )
    else:
        print("No APK path recorded — see logs/gradle.log")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
