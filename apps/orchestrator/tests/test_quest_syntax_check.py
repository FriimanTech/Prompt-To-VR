"""Quest-only compile errors are caught during code generation.

MSVC accepted a ``requires``-probe of a function signature that does not
exist; the NDK's clang rejects it, so the Humans vs Aliens build passed
code generation and then failed in quest_build.
"""

from __future__ import annotations

from pathlib import Path
from types import SimpleNamespace

from ptv_orchestrator.pipeline import stage_code_generation as cg
from ptv_schemas import TargetDevice


def _fake_ndk(root: Path, version: str) -> Path:
    ndk = root / "ndk" / version
    exe = ndk / "toolchains" / "llvm" / "prebuilt" / "windows-x86_64" / "bin" / "clang++.exe"
    exe.parent.mkdir(parents=True)
    exe.write_text("")
    return ndk


def test_ndk_resolution_order(tmp_path, monkeypatch):
    monkeypatch.delenv("ANDROID_NDK_HOME", raising=False)
    monkeypatch.delenv("ANDROID_HOME", raising=False)
    monkeypatch.delenv("ANDROID_SDK_ROOT", raising=False)
    monkeypatch.setenv("LOCALAPPDATA", str(tmp_path / "nowhere"))
    settings = SimpleNamespace(android_ndk_home=None, android_sdk_root=None,
                               android_ndk_version="30.0.1")
    assert cg.resolve_ndk(settings) is None

    sdk = tmp_path / "sdk"
    ndk = _fake_ndk(sdk, "30.0.1")
    monkeypatch.setenv("ANDROID_HOME", str(sdk))
    assert cg.resolve_ndk(settings) == ndk

    explicit = _fake_ndk(tmp_path / "explicit", "x")
    settings.android_ndk_home = str(explicit)
    assert cg.resolve_ndk(settings) == explicit


def _ctx(device: TargetDevice):
    state = SimpleNamespace(target_device=device)
    emitted: list[tuple] = []
    return SimpleNamespace(
        project_id="p",
        settings=SimpleNamespace(codegen_quest_syntax_check=True),
        store=SimpleNamespace(load_state=lambda _pid: state),
        emit=lambda *a: emitted.append(a),
    )


def test_quest_projects_fail_verify_when_clang_fails(monkeypatch):
    monkeypatch.setattr(cg, "_default_verify_compile", lambda ctx: (True, "msvc ok"))
    clang_err = ("C:/e/generated/mechanics/p/pistol_fire_shared.h:115:75: error: "
                 "too few arguments to function call, expected 2, have 1")
    monkeypatch.setattr(cg, "quest_syntax_check", lambda ctx: (False, clang_err))

    ok, log = cg._verify_all_targets(_ctx(TargetDevice.quest))
    assert not ok and "msvc ok" in log and "too few arguments" in log

    ok, log = cg._verify_all_targets(_ctx(TargetDevice.pcvr))
    assert ok and "too few" not in log


def test_clang_skipped_when_msvc_fails_or_no_ndk(monkeypatch):
    calls: list[str] = []
    monkeypatch.setattr(cg, "_default_verify_compile", lambda ctx: (False, "LNK2005"))
    monkeypatch.setattr(cg, "quest_syntax_check", lambda ctx: calls.append("clang"))
    ok, _ = cg._verify_all_targets(_ctx(TargetDevice.quest))
    assert not ok and calls == []

    monkeypatch.setattr(cg, "_default_verify_compile", lambda ctx: (True, "ok"))
    monkeypatch.setattr(cg, "quest_syntax_check", lambda ctx: None)  # no NDK
    ok, _ = cg._verify_all_targets(_ctx(TargetDevice.quest))
    assert ok


def test_clang_error_lines_map_to_owning_task():
    board = SimpleNamespace(task=lambda tid: {
        "pistol_fire": {"files_written": [
            "engine/generated/mechanics/p/pistol_fire_shared.h",
            "engine/generated/mechanics/p/pistol_fire_profile.cpp"]},
        "survival_run_flow": {"files_written": [
            "engine/generated/mechanics/p/survival_run_flow_simulation.cpp"]},
    }.get(tid))
    runtime = SimpleNamespace(tasks_by_id={"pistol_fire": 1, "survival_run_flow": 1},
                              board=board)
    log = "\n".join([
        r"C:\r\engine\generated\mechanics\p/pistol_fire_shared.h:115:75: error: too few arguments",
        r"C:\r\engine\generated\mechanics\p\survival_run_flow_simulation.cpp:69:55: error: too few",
    ])
    got = cg._StageRuntime._failing_tasks_from_log(runtime, log)
    assert set(got) == {"pistol_fire", "survival_run_flow"}
