"""The web UI's stage list must match the pipeline's.

The UI keeps its own copy of the stage order in ``apps/web/src/lib/stages.ts``
because it has to render a timeline before it has spoken to the backend. That
copy silently went stale: ``world_generation`` moved ahead of
``code_generation`` in the orchestrator and the UI kept the old order, so the
timeline showed the pipeline running its stages backwards, and the resume menu
offered them in an order the backend would not honour.

Neither symptom fails anything. Both look like a rendering quirk rather than a
wrong list, which is exactly why this needs a test rather than care.
"""

from __future__ import annotations

import re
from pathlib import Path

from ptv_orchestrator.pipeline.orchestrator import (
    _PCVR_TAIL,
    _QUEST_TAIL,
    _SHARED_STAGES,
)
from ptv_schemas import ProjectStage

_STAGES_TS = (
    Path(__file__).resolve().parents[3] / "apps" / "web" / "src" / "lib" / "stages.ts"
)


def _ts_source() -> str:
    assert _STAGES_TS.exists(), f"missing {_STAGES_TS}"
    return _STAGES_TS.read_text(encoding="utf-8")


def _ui_stages() -> list[tuple[str, int, str | None]]:
    """(id, step, device) for each entry in the UI's STAGES array, in order."""
    src = _ts_source()
    start = src.index("const STAGES: StageMeta[] = [")
    body = src[start:]
    out: list[tuple[str, int, str | None]] = []
    # Each entry is an object literal opening with id then step.
    for match in re.finditer(
        r'\{\s*id:\s*"([a-z_]+)",\s*step:\s*(\d+),(.*?)\n  \},',
        body,
        re.DOTALL,
    ):
        stage_id, step, rest = match.group(1), int(match.group(2)), match.group(3)
        device_match = re.search(r'device:\s*"(pcvr|quest)"', rest)
        out.append((stage_id, step, device_match.group(1) if device_match else None))
    return out


def _backend_stages(device: str) -> list[str]:
    tail = _QUEST_TAIL if device == "quest" else _PCVR_TAIL
    return [stage.value for stage, _fn in [*_SHARED_STAGES, *tail]]


def test_the_ui_lists_every_stage_the_pipeline_runs():
    ui_ids = {sid for sid, _step, _dev in _ui_stages()}
    for device in ("pcvr", "quest"):
        for stage in _backend_stages(device):
            assert stage in ui_ids, (
                f"the pipeline runs '{stage}' but the UI has no entry for it, "
                "so it will be invisible in the timeline and unavailable in "
                "the resume menu"
            )


def test_the_ui_invents_no_stages():
    real = set(_backend_stages("pcvr")) | set(_backend_stages("quest"))
    for stage_id, _step, _dev in _ui_stages():
        assert stage_id in real, (
            f"the UI offers '{stage_id}', which the pipeline does not run; "
            "picking it would be rejected as an unknown stage"
        )
        # Also has to be a real enum member, or the API rejects it outright.
        ProjectStage(stage_id)


def test_the_ui_order_matches_the_run_order_per_device():
    entries = _ui_stages()
    for device in ("pcvr", "quest"):
        ui = [
            sid
            for sid, _step, dev in entries
            if dev is None or dev == device
        ]
        assert ui == _backend_stages(device), (
            f"UI stage order for {device} disagrees with the pipeline.\n"
            f"  UI:      {ui}\n"
            f"  pipeline: {_backend_stages(device)}"
        )


def test_step_numbers_are_sequential_per_device():
    # The step number is what the operator reads as "where am I". Two stages
    # sharing a number, or a gap, makes the timeline unreadable.
    entries = _ui_stages()
    for device in ("pcvr", "quest"):
        steps = [
            step for _sid, step, dev in entries if dev is None or dev == device
        ]
        assert steps == list(range(1, len(steps) + 1)), (
            f"step numbers for {device} are not 1..n: {steps}"
        )


def test_world_generation_is_numbered_before_code_generation():
    # The specific regression this file exists for. Gameplay code addresses
    # ids the world architects invented, so it cannot be written first.
    steps = {sid: step for sid, step, _dev in _ui_stages()}
    assert steps["world_generation"] < steps["code_generation"]


def test_credit_spending_stages_are_marked_as_such():
    # These bill a third party per asset or per clip. Re-running them by
    # accident from the resume menu costs real money, so the badge is the
    # warning and it has to be present.
    src = _ts_source()
    for stage in (
        "reference_images",
        "tripo_models",
        "asset_rigging",
        "asset_animation",
    ):
        block = re.search(
            r'\{\s*id:\s*"' + stage + r'",.*?\n  \},', src, re.DOTALL
        )
        assert block, f"no UI entry for {stage}"
        assert 'cost: "credits"' in block.group(0), (
            f"{stage} bills third-party credits but is not marked 'credits' "
            "in the UI, so nothing warns before a re-run"
        )


def test_deterministic_stages_are_not_marked_as_billable():
    # The inverse mistake is just as bad: marking free stages as expensive
    # trains the operator to ignore the badge.
    src = _ts_source()
    for stage in (
        "asset_normalization",
        "asset_contract",
        "asset_optimization",
        "scene_compile",
        "engine_promotion",
    ):
        block = re.search(
            r'\{\s*id:\s*"' + stage + r'",.*?\n  \},', src, re.DOTALL
        )
        assert block, f"no UI entry for {stage}"
        assert 'cost: "free"' in block.group(0), (
            f"{stage} is deterministic local work but is not marked 'free'"
        )
