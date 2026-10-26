"""quest_runtime_verify: device choice, telemetry parsing, the checks each
kind of game gets, and the skip paths (never a failure without a headset)."""

from __future__ import annotations

import json
import threading
from pathlib import Path

from ptv_schemas import CheckOutcome, NpcPlan, TargetDevice, VerificationStatus

from ptv_orchestrator.events import EventBus, PipelineEvent
from ptv_orchestrator.pipeline import stage_quest_verify as qv
from ptv_orchestrator.pipeline.context import PipelineContext
from ptv_orchestrator.pipeline.orchestrator import _stages_for
from ptv_orchestrator.settings import Settings
from ptv_orchestrator.storage import ProjectStore


class _Bus(EventBus):
    def __init__(self) -> None:
        super().__init__()
        self.events: list[PipelineEvent] = []
        self._l = threading.Lock()

    def publish(self, event: PipelineEvent) -> None:
        with self._l:
            self.events.append(event)


def _ctx(tmp_path: Path, device: TargetDevice = TargetDevice.quest,
         apk: bool = True) -> PipelineContext:
    settings = Settings(projects_dir=tmp_path / "projects", engine_dir=tmp_path / "engine",
                        samples_dir=tmp_path / "samples", repo_root=tmp_path)
    store = ProjectStore(settings.projects_dir)
    state = store.create("Demo", "prompt")
    state.target_device = device
    if apk:
        p = tmp_path / "app-debug.apk"
        p.write_bytes(b"apk")
        state.latest_build_apk = str(p)
    store.save_state(state)
    return PipelineContext(project_id=state.project_id, settings=settings,
                           store=store, bus=_Bus())


def _line(t: str, body: str) -> str:
    return f"10-25 12:00:{t} I/ptv     ( 1): [info] {body}"


BASE_LOG = "\n".join([
    _line("00.000", "Loaded scene 'arena' with 10 entities, 1 lights"),
    _line("01.000", "npc_manifest_summary scene=arena npcs=0 templates=0 unresolved=- player=3 verify=1"),
])


# --- the stage is Quest-only -----------------------------------------------

def test_verify_is_in_the_quest_tail_only() -> None:
    quest = [s.value for s, _ in _stages_for(TargetDevice.quest)]
    pcvr = [s.value for s, _ in _stages_for(TargetDevice.pcvr)]
    assert quest.index("quest_runtime_verify") == quest.index("quest_build") + 1
    assert "quest_runtime_verify" not in pcvr


# --- device choice -----------------------------------------------------------

def test_choose_device_cases() -> None:
    assert qv.choose_device("List of devices attached\n\n", None) == (
        None, "no Quest headset connected")
    serial, why = qv.choose_device("List of devices attached\nABC\tunauthorized\n", None)
    assert serial is None and "unauthorized" in why
    serial, why = qv.choose_device("List\nA\tdevice\nB\tdevice\n", None)
    assert serial is None and "several" in why
    assert qv.choose_device("List\nA\tdevice\nB\tdevice\n", "B") == ("B", "")
    assert qv.choose_device("List\nQ3\tdevice\n", None) == ("Q3", "")


# --- skip paths: never a failure --------------------------------------------

def test_no_headset_is_a_skip_not_a_failure(tmp_path: Path) -> None:
    ctx = _ctx(tmp_path)
    res = qv.run_with_adb(ctx, runner=lambda a, t: (0, "List of devices attached\n\n"))
    doc = json.loads((ctx.project_dir / qv.ARTIFACT).read_text(encoding="utf-8"))
    assert doc["status"] == "skipped" and "no Quest headset" in doc["reason"]
    assert res.message.startswith("skipped")


def test_pcvr_project_skips_itself(tmp_path: Path) -> None:
    ctx = _ctx(tmp_path, device=TargetDevice.pcvr)
    qv.run_with_adb(ctx, runner=lambda a, t: (0, "List\nQ3\tdevice\n"))
    doc = json.loads((ctx.project_dir / qv.ARTIFACT).read_text(encoding="utf-8"))
    assert doc["status"] == "skipped" and "Quest-only" in doc["reason"]


def test_headset_disconnecting_mid_run_is_a_skip(tmp_path: Path) -> None:
    ctx = _ctx(tmp_path)
    calls = {"devices": 0}

    def runner(args: list[str], _t: float) -> tuple[int, str]:
        if args == ["devices"]:
            calls["devices"] += 1
            return 0, "List\nQ3\tdevice\n" if calls["devices"] == 1 else "List\n"
        return 0, ""

    qv.run_with_adb(ctx, runner=runner, sleep=lambda _s: None)
    doc = json.loads((ctx.project_dir / qv.ARTIFACT).read_text(encoding="utf-8"))
    assert doc["status"] == "skipped" and "disconnected" in doc["reason"]


# --- what each kind of game is checked for ----------------------------------

def test_game_without_npcs_runs_only_base_checks_and_passes() -> None:
    tel = qv.parse_telemetry(BASE_LOG)
    checks, summary = qv.evaluate(tel, NpcPlan(prompt_id="p"), {}, process_alive=True)
    assert summary == "not applicable: this game has no NPCs"
    assert {c.category for c in checks} == {"base"}
    assert all(c.outcome == CheckOutcome.passed for c in checks)


def _plan(style: str, behaviours: list[str]) -> NpcPlan:
    return NpcPlan.model_validate({"prompt_id": "p", "npcs": [{
        "asset_id": "guard", "role": "neutral", "persistent_count": 1,
        "movement_style": style, "behaviors": behaviours,
        "animation_intents": ["idle"]}]})


SCENE = {"entities": [{"id": "guard_1", "tags": ["npc"],
                       "mesh": {"asset_id": "guard", "animations": {"idle": "Idle"}}}],
         "navigation": {"regions": [{"id": "r", "min": [-20, 0, -20], "max": [20, 0, 20]}]}}


def _npc_log(points: list[tuple[float, float]], clip: str = "Idle") -> str:
    lines = [BASE_LOG,
             _line("01.100", "npc_manifest id=guard_1 e=7 asset=guard template=0 skeleton=1 "
                             "skinned=1 clips=idle:ok mechanics=- attached=- unresolved=- pos=0,0.9,0")]
    for i, (x, speed) in enumerate(points):
        lines.append(_line(f"{2 + i:02d}.000",
                           f"npc t={2 + i}.0 id=guard_1 e=7 asset=guard pos={x},0.90,0.00 "
                           f"feet_gap=0.02 speed={speed} intent=0 clip=\"{clip}\" alive=1 "
                           "dist_player=3.0"))
    return "\n".join(lines)


def test_stationary_npc_that_stays_put_passes_and_one_that_walks_fails() -> None:
    plan = _plan("stationary", ["idle", "guard_point"])
    still = qv.evaluate(qv.parse_telemetry(_npc_log([(0.0, 0.0)] * 5)), plan, SCENE,
                        process_alive=True)[0]
    assert all(c.outcome != CheckOutcome.failed for c in still)
    assert any(c.category == "stationary" and c.name == "stays where it was placed"
               for c in still)
    walked = qv.evaluate(qv.parse_telemetry(_npc_log([(float(i), 1.0) for i in range(5)])),
                         plan, SCENE, process_alive=True)[0]
    drift = next(c for c in walked if c.name == "stays where it was placed")
    assert drift.outcome == CheckOutcome.failed
    # A stationary NPC is never judged on follow / chase movement.
    assert not any(c.category in ("follower", "enemy") for c in walked)


def test_npc_launched_into_the_sky_fails_the_floor_check() -> None:
    log = _npc_log([(0.0, 0.0)] * 3).replace("feet_gap=0.02", "feet_gap=250.00")
    checks = qv.evaluate(qv.parse_telemetry(log), _plan("stationary", ["idle"]), SCENE,
                         process_alive=True)[0]
    floor = next(c for c in checks if c.name == "stands on the floor")
    assert floor.outcome == CheckOutcome.failed


def test_floating_by_half_its_height_fails_even_with_a_perfect_contract() -> None:
    # origin_gap 0 (the capsule sits right) but the DRAWN feet are 0.9 m up:
    # the Tripo floating bug. Only the drawn-feet measurement can see it.
    log = _npc_log([(0.0, 0.0)] * 3).replace(
        "feet_gap=0.02", "feet_gap=0.90 origin_gap=0.00 motor=grounded")
    checks = qv.evaluate(qv.parse_telemetry(log), _plan("stationary", ["idle"]), SCENE,
                         process_alive=True)[0]
    floor = next(c for c in checks if c.name == "stands on the floor")
    assert floor.outcome == CheckOutcome.failed


def test_hovering_npc_is_judged_against_its_hover_height() -> None:
    log = _npc_log([(0.0, 0.0)] * 3).replace("feet_gap=0.02", "feet_gap=2.02 motor=hover:2.00")
    checks = qv.evaluate(qv.parse_telemetry(log), _plan("flying", ["idle"]), SCENE,
                         process_alive=True)[0]
    hover = next(c for c in checks if c.name == "holds its hover height")
    assert hover.outcome == CheckOutcome.passed
    assert not any(c.name == "stands on the floor" for c in checks)


def test_npc_walking_sideways_fails_the_facing_check() -> None:
    plan = _plan("animated_locomotion", ["patrol"])
    sideways = _npc_log([(float(i), 1.0) for i in range(5)]).replace(
        "feet_gap=0.02", "feet_gap=0.02 facing_err_deg=90")
    checks = qv.evaluate(qv.parse_telemetry(sideways), plan, SCENE, process_alive=True)[0]
    facing = next(c for c in checks if c.name == "faces where it is going")
    assert facing.outcome == CheckOutcome.failed
    forward = _npc_log([(float(i), 1.0) for i in range(5)]).replace(
        "feet_gap=0.02", "feet_gap=0.02 facing_err_deg=4")
    checks = qv.evaluate(qv.parse_telemetry(forward), plan, SCENE, process_alive=True)[0]
    facing = next(c for c in checks if c.name == "faces where it is going")
    assert facing.outcome == CheckOutcome.passed


def _goblin_design():
    from ptv_schemas import GameDesign

    return GameDesign.model_validate({
        "prompt_id": "p", "experience": "combat",
        "player_vitality": {"has_health": True}, "lives": {"count": 3},
        "objectives": [{"id": "survive", "kind": "survive_time", "description": "Survive",
                        "duration_s": 180}],
        "victory": {"kind": "all_objectives", "objective_ids": ["survive"]},
        "failure": [{"kind": "lives_exhausted"}],
        "end_screens": [
            {"kind": "game_over", "title": "Game Over", "buttons": ["try_again", "leave_game"]},
            {"kind": "victory", "title": "You Win!", "buttons": ["play_again", "leave_game"]}]})


FLOW_LOG = "\n".join(_line("00.000", f"flow: {b}") for b in [
    "state=playing from=inactive why=start attempt=1 lives=3",
    "state=life_lost from=playing why=health_zero attempt=1 lives=2",
    "respawned at 0.00,0.00,0.00 (spawn) lives=2",
    "state=respawning from=life_lost why=respawn attempt=1 lives=2",
    "state=life_lost from=playing why=health_zero attempt=1 lives=1",
    "state=respawning from=life_lost why=respawn attempt=1 lives=1",
    "state=life_lost from=playing why=health_zero attempt=1 lives=0",
    "state=game_over from=life_lost why=lives_exhausted attempt=1 lives=0",
    "button=try_again state=game_over",
    "state=playing from=inactive why=start attempt=2 lives=3",
    "state=victory from=playing why=objectives_met attempt=2 lives=3",
    "button=play_again state=victory",
    "state=playing from=inactive why=start attempt=3 lives=3",
    "state=paused from=playing why=verify_pause attempt=3 lives=3",
    "button=leave_game state=paused",
    "state=exiting from=paused why=leave_game attempt=3 lives=3",
])


def test_goblin_shooter_loop_passes_every_flow_check() -> None:
    checks = qv.evaluate_flow(qv.parse_flow(FLOW_LOG), _goblin_design(),
                              exited=True, relaunched=True)
    names = {c.name for c in checks}
    assert {"health reaching zero costs a life", "the player respawns",
            "Game Over follows the last life", "Try Again restarts the game",
            "a restart resets lives", "winning ends on the victory screen",
            "Play Again restarts the game", "Leave Game ends the app",
            "the app launches again after leaving"} <= names
    assert all(c.outcome == CheckOutcome.passed for c in checks), [
        (c.name, c.detail) for c in checks if c.outcome != CheckOutcome.passed]


def test_frozen_at_zero_health_fails_the_flow() -> None:
    # The humans-vs-aliens bug: health hit zero and nothing happened.
    log = "\n".join(_line("00.000", f"flow: {b}") for b in [
        "state=playing from=inactive why=start attempt=1 lives=3"])
    checks = qv.evaluate_flow(qv.parse_flow(log), _goblin_design(),
                              exited=False, relaunched=False)
    failed = {c.name for c in checks if c.outcome == CheckOutcome.failed}
    assert "health reaching zero costs a life" in failed
    assert "Leave Game ends the app" in failed


def test_a_reader_is_only_checked_for_not_dying_and_leaving() -> None:
    from ptv_schemas import GameDesign

    reader = GameDesign.model_validate({"prompt_id": "p", "experience": "media",
                                        "free_play": True})
    log = "\n".join(_line("00.000", f"flow: {b}") for b in [
        "state=playing from=inactive why=start attempt=1 lives=-1",
        "state=paused from=playing why=verify_pause attempt=1 lives=-1",
        "button=leave_game state=paused"])
    checks = qv.evaluate_flow(qv.parse_flow(log), reader, exited=True, relaunched=True)
    assert {c.name for c in checks} == {
        "game loop starts", "the player cannot die", "Leave Game ends the app",
        "the app launches again after leaving"}
    assert all(c.outcome == CheckOutcome.passed for c in checks)
    assert qv.flow_capture_seconds(reader) < qv.flow_capture_seconds(_goblin_design())


def test_crash_fails_the_base_check() -> None:
    checks, _ = qv.evaluate(qv.parse_telemetry(BASE_LOG), None, {}, process_alive=False,
                            crash_text="F libc: Fatal signal 11 (SIGSEGV)")
    crash = next(c for c in checks if c.name == "no crash")
    assert crash.outcome == CheckOutcome.failed


def test_status_enum_values() -> None:
    assert {s.value for s in VerificationStatus} == {"passed", "failed", "skipped"}
