"""Quest only: run the built APK on the headset and check that the game does
what it asked for.

Compiling and packaging prove nothing about runtime behaviour. Every NPC bug
this stage exists for -- characters launched into the sky by their own
ground probe, generated AI that was never instantiated, an ally following a
static rig at the world origin -- built, packaged and installed cleanly.

What it does (``target_device == quest`` only; it is not in the PC VR stage
list, and it skips itself if it ever runs on a PC VR project):

1. Finds the headset with ``adb devices``. No adb, no device, an
   ``unauthorized`` / ``offline`` device, or several devices with none named
   in Settings: the stage is SKIPPED, never failed.
2. Keeps the headset awake unworn (``vrpowermanager.prox_close``), turns on
   the engine's verification mode (``setprop debug.ptv.verify 1``: the player
   walks a scripted square and cannot die), installs and launches the APK,
   captures ``adb logcat -s ptv`` for the encounter's start delay plus
   ``quest_verify_capture_seconds``, then force-stops it and restores the
   proximity sensor and the property -- always, even when a step failed.
3. Parses the engine's NPC telemetry (``npc_manifest``, ``npc``,
   ``npc_summary``, ``spawn:``, ``combat:`` lines) and runs ONLY the checks
   this game calls for, derived from its NPC plan and compiled scene: base
   checks for every game (including one with no NPCs), presence and floor
   checks for placed NPCs, a no-drift check for stationary ones, a band check
   for followers, spawn checks for encounters, and close-and-hit checks for
   enemies hostile to the player.
4. Writes ``runtime_verification.json`` and ``logs/runtime/verify_<ts>.log``.

Report-only: a failed check fails the stage with the exact list of what did
not hold, and nothing is repaired automatically.
"""

from __future__ import annotations

import datetime as _dt
import json
import math
import re
import shutil
import subprocess
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, Optional

from ptv_schemas import (
    CheckOutcome,
    NpcPlan,
    ProjectStage,
    ProjectStatus,
    RuntimeVerification,
    TargetDevice,
    VerificationCheck,
    VerificationStatus,
)

from ..quest_ids import quest_package_name
from .context import PipelineContext, StageResult

ARTIFACT = "runtime_verification.json"
#: A capsule standing on the floor has feet within this of it.
FEET_TOLERANCE_M = 0.3
#: A stationary NPC may sway (idle clip, turning to face) but not walk.
STATIONARY_DRIFT_M = 0.25
#: The scripted walk starts this long after launch (engine kSettleS).
AUTOPILOT_SETTLE_S = 3.0
#: Launch handling (see the launch loop in ``run_with_adb``).
#: Horizon OS opens its Guardian boundary-setup flow on a headset sitting
#: unworn, and a launch issued under it is dropped; the flow closes on its
#: own within a couple of minutes. So: settle, try, dismiss, retry.
LAUNCH_SETTLE_S = 8.0
LAUNCH_ATTEMPTS = 6
LAUNCH_POLL_S = 15

#: ``(argv, timeout_s) -> (returncode, stdout+stderr)``. Injected by tests.
AdbRunner = Callable[[list[str], float], "tuple[int, str]"]
#: ``(argv, out_path) -> stop()``; ``stop()`` ends the stream and returns
#: everything it captured. The headset's log ring buffer holds well under a
#: minute of NPC telemetry, so reading it once at the end (``logcat -d``)
#: loses the start-up lines -- the log is streamed to disk for the whole run.
LogStreamer = Callable[[list[str], Path], Callable[[], str]]


class DeviceGone(RuntimeError):
    """The headset disappeared mid-run: a skip, not a failure."""


# ---------------------------------------------------------------------------
# adb
# ---------------------------------------------------------------------------

def _default_runner(adb: str) -> AdbRunner:
    def run(args: list[str], timeout: float) -> tuple[int, str]:
        try:
            p = subprocess.run([adb, *args], capture_output=True, text=True,
                               timeout=timeout, encoding="utf-8", errors="replace")
        except FileNotFoundError:
            return 127, "adb not found"
        except subprocess.TimeoutExpired:
            return 124, f"adb {' '.join(args[:3])} timed out"
        return p.returncode, (p.stdout or "") + (p.stderr or "")
    return run


def _default_streamer(adb: str) -> LogStreamer:
    def start(args: list[str], out_path: Path) -> Callable[[], str]:
        out_path.parent.mkdir(parents=True, exist_ok=True)
        fh = out_path.open("w", encoding="utf-8", errors="replace")
        proc = subprocess.Popen([adb, *args], stdout=fh, stderr=subprocess.STDOUT,
                                text=True, encoding="utf-8", errors="replace")

        def stop() -> str:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
            fh.close()
            return out_path.read_text(encoding="utf-8", errors="replace")
        return stop
    return start


def choose_device(devices_output: str, preferred: Optional[str]) -> tuple[Optional[str], str]:
    """Pick the headset from ``adb devices`` output. Returns ``(serial,
    reason)``; ``serial`` is None with the reason to skip."""
    ready: list[str] = []
    other: list[str] = []
    for line in devices_output.splitlines():
        line = line.strip()
        if not line or line.startswith("List of devices") or line.startswith("*"):
            continue
        parts = line.split()
        if len(parts) < 2:
            continue
        serial, state = parts[0], parts[1]
        (ready if state == "device" else other).append(f"{serial} ({state})")
    ready_serials = [r.split(" ")[0] for r in ready]
    if preferred:
        if preferred in ready_serials:
            return preferred, ""
        return None, (f"the configured headset {preferred} is not connected "
                      f"and authorized (seen: {ready + other or 'nothing'})")
    if not ready:
        if other:
            return None, ("a device is connected but not usable: "
                          + ", ".join(other)
                          + " (accept the USB-debugging prompt in the headset)")
        return None, "no Quest headset connected"
    if len(ready) > 1:
        return None, ("several devices are connected (" + ", ".join(ready_serials)
                      + "); set quest_verify_device_serial to choose one")
    return ready_serials[0], ""


# ---------------------------------------------------------------------------
# Telemetry parsing
# ---------------------------------------------------------------------------

_KV = re.compile(r'(\w+)=("[^"]*"|\S+)')
_TS = re.compile(r"^(\d\d)-(\d\d) (\d\d):(\d\d):(\d\d)\.(\d{3})")
_SPAWN = re.compile(r"spawn: (\S+) -> entity (\d+) at (\S+?),(\S+?),(\S+)")
_HIT = re.compile(r"combat: (\d+) hit (\d+)( \(player\))? for ([\d.]+)")


def _kv(text: str) -> dict[str, str]:
    return {k: v.strip('"') for k, v in _KV.findall(text)}


def _vec(text: str) -> Optional[tuple[float, float, float]]:
    try:
        x, y, z = (float(v) for v in text.split(",")[:3])
        return x, y, z
    except (ValueError, TypeError):
        return None


def _seconds(line: str) -> Optional[float]:
    m = _TS.match(line)
    if not m:
        return None
    _mo, _d, h, mi, s, ms = (int(g) for g in m.groups())
    return h * 3600 + mi * 60 + s + ms / 1000.0


@dataclass
class Telemetry:
    lines: list[str] = field(default_factory=list)
    loaded: bool = False
    load_time: Optional[float] = None
    manifest: list[dict[str, str]] = field(default_factory=list)
    summary_manifest: dict[str, str] = field(default_factory=dict)
    status: list[dict[str, Any]] = field(default_factory=list)
    summaries: list[dict[str, str]] = field(default_factory=list)
    spawns: list[dict[str, Any]] = field(default_factory=list)
    hits: list[dict[str, Any]] = field(default_factory=list)
    wave_started: list[float] = field(default_factory=list)
    unresolved_lines: list[str] = field(default_factory=list)
    load_failures: list[str] = field(default_factory=list)
    errors: list[str] = field(default_factory=list)


def parse_telemetry(text: str) -> Telemetry:
    t = Telemetry()
    for raw in text.splitlines():
        if "ptv" not in raw:
            continue
        t.lines.append(raw)
        ts = _seconds(raw)
        body = raw.split("] ", 1)[1] if "] " in raw else raw
        if "Loaded scene" in body:
            t.loaded = True
            if t.load_time is None:
                t.load_time = ts
        elif body.startswith("npc_manifest_summary"):
            t.summary_manifest = _kv(body)
        elif body.startswith("npc_manifest "):
            t.manifest.append(_kv(body))
        elif body.startswith("npc_summary "):
            t.summaries.append(_kv(body))
        elif body.startswith("npc "):
            kv: dict[str, Any] = _kv(body)
            kv["_pos"] = _vec(str(kv.get("pos", "")))
            for key in ("t", "speed", "intent", "dist_player"):
                try:
                    kv[key] = float(kv.get(key, "nan"))
                except ValueError:
                    kv[key] = math.nan
            try:
                kv["feet_gap"] = float(kv.get("feet_gap", "nan"))
            except ValueError:
                kv["feet_gap"] = math.nan
            t.status.append(kv)
        elif body.startswith("spawn:"):
            m = _SPAWN.search(body)
            if m:
                t.spawns.append({"template": m.group(1), "e": m.group(2),
                                 "time": ts})
        elif body.startswith("combat:"):
            m = _HIT.search(body)
            if m:
                t.hits.append({"by": m.group(1), "target": m.group(2),
                               "player": bool(m.group(3)),
                               "amount": float(m.group(4)), "time": ts})
        elif "encounter: wave" in body and "started" in body:
            if ts is not None:
                t.wave_started.append(ts)
        if "Unresolved mechanic bindings" in body or "No mechanic registered" in body:
            t.unresolved_lines.append(body)
        if "failed to load" in body:
            t.load_failures.append(body)
        if "E/ptv" in raw or "[error]" in body:
            t.errors.append(body)
    return t


# ---------------------------------------------------------------------------
# Expectations
# ---------------------------------------------------------------------------

def _check(name: str, category: str, ok: Optional[bool], detail: str = "",
           subject: str = "", evidence: Optional[list[str]] = None) -> VerificationCheck:
    outcome = (CheckOutcome.not_applicable if ok is None
               else CheckOutcome.passed if ok else CheckOutcome.failed)
    return VerificationCheck(name=name, category=category, subject=subject,
                             outcome=outcome, detail=detail[:2000],
                             evidence=[e[:300] for e in (evidence or [])][:20])


def _inside(regions: list[dict], pos: tuple[float, float, float]) -> bool:
    return any(float(r["min"][0]) - 0.5 <= pos[0] <= float(r["max"][0]) + 0.5
               and float(r["min"][2]) - 0.5 <= pos[2] <= float(r["max"][2]) + 0.5
               for r in regions)


def _clip_names(entity: dict, intents: tuple[str, ...]) -> set[str]:
    anims = ((entity.get("mesh") or {}).get("animations")) or {}
    out = {str(anims[i]) for i in intents if i in anims}
    # Gait variants ("walk.holding_torch_a") are walking too.
    out |= {str(v) for k, v in anims.items()
            if any(k.startswith(i + ".") for i in intents)}
    return out


def evaluate(tel: Telemetry, npc_plan: Optional[NpcPlan], scene: dict,
             *, process_alive: bool, crash_text: str = "") -> tuple[list[VerificationCheck], str]:
    """Every check THIS game calls for. Returns ``(checks, npc_summary)``."""

    checks: list[VerificationCheck] = []
    # --- Base: every Quest game, with or without NPCs ---------------------
    checks.append(_check("app launched and loaded its scene", "base", tel.loaded,
                         "" if tel.loaded else "no 'Loaded scene' line in the log"))
    frames = bool(tel.summary_manifest)
    checks.append(_check(
        "frame loop ran", "base", frames,
        "" if frames else "the engine never reached its first frame (XR session "
        "never focused -- headset asleep, or a crash at start-up)"))
    crashed = ("Fatal signal" in crash_text or "FATAL EXCEPTION" in crash_text
               or not process_alive)
    checks.append(_check("no crash", "base", not crashed,
                         "" if not crashed else "the app was not running at the end "
                         "of the capture or the crash buffer has a fatal signal",
                         evidence=[l for l in crash_text.splitlines()
                                   if "Fatal" in l or "signal" in l][:5]))
    unresolved = (tel.summary_manifest.get("unresolved", "-") not in ("", "-")
                  or bool(tel.unresolved_lines))
    checks.append(_check("every mechanic binding resolves", "base", not unresolved,
                         "bindings that do nothing at runtime" if unresolved else "",
                         evidence=tel.unresolved_lines[:5]))
    checks.append(_check("every GLB loaded", "base", not tel.load_failures,
                         evidence=tel.load_failures[:5]))

    if npc_plan is None or not npc_plan.npcs:
        return checks, "not applicable: this game has no NPCs"

    entities = [e for e in scene.get("entities") or [] if isinstance(e, dict)]
    regions = [r for r in ((scene.get("navigation") or {}).get("regions") or [])
               if isinstance(r, dict)]
    manifest_by_id = {m.get("id"): m for m in tel.manifest}
    status_by_id: dict[str, list[dict]] = {}
    status_by_e: dict[str, list[dict]] = {}
    for s in tel.status:
        status_by_id.setdefault(str(s.get("id")), []).append(s)
        status_by_e.setdefault(str(s.get("e")), []).append(s)
    autopilot_from = AUTOPILOT_SETTLE_S + 5.0

    def floor_and_area(subject: str, samples: list[dict], category: str) -> None:
        gaps = [s["feet_gap"] for s in samples if not math.isnan(s["feet_gap"])]
        if not samples:
            return
        if not gaps:
            checks.append(_check("stands on the floor", category, False,
                                 "no floor found under it in any sample", subject))
        else:
            worst = max(gaps, key=abs)
            ok = abs(sorted(gaps)[len(gaps) // 2]) <= FEET_TOLERANCE_M and abs(worst) <= 1.0
            checks.append(_check(
                "stands on the floor", category, ok,
                f"feet {sorted(gaps)[len(gaps) // 2]:+.2f} m from the floor (median), "
                f"worst {worst:+.2f} m over {len(gaps)} samples", subject))
        if regions:
            outside = [s for s in samples if s["_pos"] and not _inside(regions, s["_pos"])]
            checks.append(_check(
                "stays inside the walkable area", category, not outside,
                f"{len(outside)}/{len(samples)} samples outside every nav region",
                subject, [str(s.get("pos")) for s in outside[:3]]))

    # --- Placed at load ---------------------------------------------------
    for npc in npc_plan.npcs:
        if not npc.persistent_count:
            continue
        ents = [e for e in entities
                if (e.get("mesh") or {}).get("asset_id") == npc.asset_id
                and "template" not in (e.get("tags") or [])]
        if not ents:
            checks.append(_check("is in the scene", "persistent", False,
                                 "the compiled scene has no entity for it", npc.asset_id))
            continue
        animated = (npc.movement_style.value != "slide_no_animation"
                    and bool(npc.animation_intents))
        for ent in ents:
            eid = str(ent.get("id"))
            man = manifest_by_id.get(eid)
            samples = status_by_id.get(eid, [])
            present = man is not None and bool(samples)
            checks.append(_check("is present and reported", "persistent", present,
                                 "" if present else "no manifest/status lines for it",
                                 eid))
            if man is None:
                continue
            if animated:
                checks.append(_check("skeleton loaded", "persistent",
                                     man.get("skeleton") == "1", f"manifest: {man.get('skeleton')}", eid))
                missing = [c for c in str(man.get("clips", "")).split(",") if c.endswith(":missing")]
                checks.append(_check("animation clips resolved", "persistent", not missing,
                                     ", ".join(missing), eid))
            floor_and_area(eid, samples, "persistent")
            behaviours = {b.value for b in npc.behaviors}
            if npc.movement_style.value == "stationary":
                first = next((s["_pos"] for s in samples if s["_pos"]), None)
                drift = max((math.hypot(s["_pos"][0] - first[0], s["_pos"][2] - first[2])
                             for s in samples if s["_pos"]), default=0.0) if first else 0.0
                checks.append(_check(
                    "stays where it was placed", "stationary", drift <= STATIONARY_DRIFT_M,
                    f"moved {drift:.2f} m (allowed {STATIONARY_DRIFT_M} m)", eid))
                idle = _clip_names(ent, ("idle",))
                if idle and animated:
                    clips = {s.get("clip") for s in samples}
                    checks.append(_check("plays idle", "stationary", bool(clips & idle),
                                         f"clips seen: {sorted(c for c in clips if c)}", eid))
                if behaviours & {"ranged_attack", "melee_attack", "contact_damage"}:
                    e_num = str(man.get("e"))
                    hits = [h for h in tel.hits if h["by"] == e_num and h["player"]]
                    checks.append(_check("attacks the player from its spot", "stationary",
                                         bool(hits), f"{len(hits)} hit(s) on the player", eid))
            elif "follow_leader" in behaviours:
                walking = [s for s in samples if s["t"] >= autopilot_from]
                follow = next((b.get("params") or {} for b in ent.get("mechanic_bindings") or []
                               if isinstance(b, dict) and "npc.follow" in str(
                                   b.get("builtin_id") or b.get("id"))), {})
                # The trailing band plus slack for the moment it takes to
                # start walking after the player does.
                band = float(follow.get("max_distance_m", 2.8)) + 3.5
                near = [s for s in walking if s["dist_player"] <= band]
                moved = [s for s in walking if s["speed"] > 0.3]
                ratio = len(near) / len(walking) if walking else 0.0
                checks.append(_check(
                    "keeps up with the player", "follower", bool(walking) and ratio >= 0.8,
                    f"within {band:.0f} m of the player in {len(near)}/{len(walking)} samples "
                    "while the player walked", eid))
                checks.append(_check("walks", "follower", bool(moved),
                                     f"{len(moved)} samples faster than 0.3 m/s", eid))
                if animated:
                    gait = _clip_names(ent, ("walk", "run"))
                    moving_clips = [s.get("clip") for s in moved]
                    in_gait = sum(1 for c in moving_clips if c in gait)
                    checks.append(_check(
                        "plays a walk/run clip while moving", "follower",
                        bool(moving_clips) and in_gait >= 0.6 * len(moving_clips),
                        f"{in_gait}/{len(moving_clips)} moving samples in a gait clip", eid))
            elif npc.movement_style.value in ("animated_locomotion", "slide_no_animation"):
                moved = [s for s in samples if s["speed"] > 0.3]
                checks.append(_check("moves", "persistent", bool(moved),
                                     f"{len(moved)} samples faster than 0.3 m/s", eid))

    # --- Encounters -------------------------------------------------------
    enc = npc_plan.encounters
    if enc is None:
        checks.append(_check("waves start", "encounter", None,
                             "the game has no encounter plan"))
    else:
        started = bool(tel.wave_started)
        checks.append(_check("waves start", "encounter", started,
                             "" if started else "no 'encounter: wave N started' line"))
        first_wave = sum(g.count for g in enc.waves[0].groups) if enc.waves else 0
        want = max(1, min(enc.max_concurrent, first_wave))
        deadline = (tel.load_time or 0.0) + float(enc.start_delay_s) + 15.0
        early = [s for s in tel.spawns
                 if s["time"] is None or tel.load_time is None or s["time"] <= deadline]
        checks.append(_check(
            "enemies spawn in time", "encounter", len(early) >= want,
            f"{len(early)} spawned within start delay + 15 s (want {want}); "
            f"{len(tel.spawns)} over the whole capture", evidence=[
                f"{s['template']} -> {s['e']}" for s in tel.spawns[:6]]))

        # Spawned enemies hostile to the player, per type.
        templates = {e.get("id"): (e.get("mesh") or {}).get("asset_id") for e in entities
                     if "template" in (e.get("tags") or [])}
        tmpl_by_asset = {a: tid for tid, a in templates.items()}
        for npc in npc_plan.npcs:
            if npc.asset_id not in npc_plan.spawned_asset_ids():
                continue
            behaviours = {b.value for b in npc.behaviors}
            subject = npc.asset_id
            mine = [s for s in tel.spawns if templates.get(s["template"]) == npc.asset_id]
            if not mine:
                checks.append(_check("spawns", "enemy", False,
                                     "no instance of this type was spawned", subject))
                continue
            samples = [x for s in mine for x in status_by_e.get(s["e"], [])]
            floor_and_area(subject, samples, "enemy")
            tmpl_ent = next((e for e in entities if e.get("id") == tmpl_by_asset.get(npc.asset_id)), {})
            if npc.movement_style.value == "stationary":
                continue
            if "player" in npc.hostile_to and behaviours & {"chase", "melee_attack"}:
                closed = 0
                for s in mine:
                    series = [x["dist_player"] for x in status_by_e.get(s["e"], [])
                              if not math.isnan(x["dist_player"])]
                    if len(series) >= 2 and (min(series) <= 0.7 * series[0]
                                             or min(series) <= npc.combat.attack_range_m + 1.0):
                        closed += 1
                checks.append(_check("moves toward the player", "enemy", closed > 0,
                                     f"{closed}/{len(mine)} instances closed the distance", subject))
                gait = _clip_names(tmpl_ent, ("walk", "run"))
                moving = [x.get("clip") for x in samples if x["speed"] > 0.3]
                if gait and npc.animation_intents:
                    in_gait = sum(1 for c in moving if c in gait)
                    checks.append(_check(
                        "plays a walk/run clip while moving", "enemy",
                        bool(moving) and in_gait >= 0.6 * len(moving),
                        f"{in_gait}/{len(moving)} moving samples in a gait clip", subject))
                ids = {s["e"] for s in mine}
                hits = [h for h in tel.hits if h["by"] in ids and h["player"]]
                checks.append(_check("attacks the player", "enemy", bool(hits),
                                     f"{len(hits)} hit(s) landed on the player", subject))

    placed = sum(n.persistent_count for n in npc_plan.npcs)
    summary = (f"{len(npc_plan.npcs)} NPC type(s): {placed} placed at load, "
               f"{len(npc_plan.spawned_asset_ids())} spawned in waves")
    return checks, summary


#: Extra watching per wave before the last NPC type first appears. In
#: verification mode the engine defeats an enemy shortly after it lands a hit,
#: so waves advance on their own -- but each still takes the enemies time to
#: arrive from their spawn zones.
SECONDS_PER_EXTRA_WAVE = 35.0
MAX_CAPTURE_S = 240.0


def capture_seconds(npc_plan: Optional[NpcPlan], base: float) -> float:
    """How long to watch: the start delay, the base window, and enough waves
    that every spawned type has had its first chance to appear."""
    if npc_plan is None or npc_plan.encounters is None:
        return base
    enc = npc_plan.encounters
    first_wave: dict[str, int] = {}
    for i, w in enumerate(enc.waves):
        for g in w.groups:
            first_wave.setdefault(g.asset_id, i)
    extra = max(first_wave.values(), default=0) * SECONDS_PER_EXTRA_WAVE
    return min(MAX_CAPTURE_S, float(enc.start_delay_s) + base + extra)


# ---------------------------------------------------------------------------
# Stage
# ---------------------------------------------------------------------------

def _write(ctx: PipelineContext, result: RuntimeVerification) -> str:
    path = ctx.project_dir / ARTIFACT
    path.write_text(result.model_dump_json(indent=2), encoding="utf-8")
    return str(path.relative_to(ctx.settings.repo_root)).replace("\\", "/")


def _now() -> str:
    return _dt.datetime.now(tz=_dt.timezone.utc).isoformat()


def run(ctx: PipelineContext) -> StageResult:
    return run_with_adb(ctx)


def run_with_adb(ctx: PipelineContext, runner: Optional[AdbRunner] = None,
                 sleep: Callable[[float], None] = time.sleep,
                 streamer: Optional[LogStreamer] = None) -> StageResult:
    stage = ProjectStage.quest_runtime_verify
    ctx.emit(stage, "running", "Verifying the game on the headset")
    state = ctx.store.load_state(ctx.project_id)
    ctx.store.record_stage(state, stage, ProjectStatus.running)
    started = _now()

    def finish_skipped(reason: str, **extra: Any) -> StageResult:
        result = RuntimeVerification(project_id=ctx.project_id,
                                     status=VerificationStatus.skipped,
                                     reason=reason, started_at=started,
                                     finished_at=_now(), **extra)
        rel = _write(ctx, result)
        st = ctx.store.load_state(ctx.project_id)
        ctx.store.record_stage(st, stage, ProjectStatus.succeeded,
                               message=f"skipped: {reason}", artifacts=[rel])
        ctx.emit(stage, "skipped", reason)
        return StageResult(stage=stage, artifacts=[rel], message=f"skipped: {reason}")

    if getattr(state, "target_device", TargetDevice.pcvr) != TargetDevice.quest:
        return finish_skipped("not a Quest project; on-headset verification is Quest-only")
    if not getattr(ctx.settings, "quest_runtime_verify", True):
        return finish_skipped("disabled in Settings (quest_runtime_verify)")
    apk = Path(state.latest_build_apk) if state.latest_build_apk else None
    if apk is None or not apk.exists():
        return finish_skipped("no APK to install; run quest_build first")

    adb = ctx.settings.adb_path or shutil.which("adb")
    if runner is None:
        if not adb:
            return finish_skipped("adb is not installed or not on PATH")
        runner = _default_runner(adb)
        streamer = streamer or _default_streamer(adb)
    rc, out = runner(["devices"], 20)
    if rc != 0:
        return finish_skipped(f"adb is not usable ({out.strip()[:200]})")
    serial, why = choose_device(out, ctx.settings.quest_verify_device_serial)
    if serial is None:
        return finish_skipped(why)

    package = quest_package_name(ctx.settings.android_package_prefix, ctx.project_id)

    def adb_s(args: list[str], timeout: float = 30) -> tuple[int, str]:
        return runner(["-s", serial, *args], timeout)

    def still_there() -> bool:
        rc_, out_ = runner(["devices"], 20)
        return rc_ == 0 and any(l.split()[:2] == [serial, "device"]
                                for l in out_.splitlines() if l.strip())

    try:
        npc_plan: Optional[NpcPlan] = ctx.npc_plan or ctx.store.load_npc_plan(ctx.project_id)
    except Exception:  # noqa: BLE001 - a game may have no NPC plan at all
        npc_plan = None
    try:
        scene = json.loads((ctx.project_dir / "scene.json").read_text(encoding="utf-8"))
    except (OSError, ValueError):
        scene = {}
    has_npcs = npc_plan is not None and bool(npc_plan.npcs)
    capture = (capture_seconds(npc_plan, float(ctx.settings.quest_verify_capture_seconds))
               if has_npcs else 20.0)

    log_dir = ctx.project_dir / "logs" / "runtime"
    log_dir.mkdir(parents=True, exist_ok=True)
    log_file = log_dir / f"verify_{_dt.datetime.now().strftime('%Y%m%d-%H%M%S')}.log"
    log_rel = str(log_file.relative_to(ctx.settings.repo_root)).replace("\\", "/")

    log_text = ""
    crash_text = ""
    alive = False
    stop_stream: Optional[Callable[[], str]] = None
    pin = getattr(ctx.settings, "quest_verify_store_pin", None)

    def scriptable(enable: bool) -> None:
        # Meta Quest Scriptable Testing: suppress Guardian / dialogs /
        # auto-sleep for the run. Needs the account's Store PIN.
        if not pin:
            return
        flag = "true" if enable else "false"
        adb_s(["shell", "content", "call", "--uri", "content://com.oculus.rc",
               "--method", "SET_PROPERTY",
               "--extra", f"disable_guardian:b:{flag}",
               "--extra", f"disable_dialogs:b:{flag}",
               "--extra", f"disable_autosleep:b:{flag}",
               "--extra", f"PIN:s:{pin}"])

    # With the proximity sensor overridden, a headset lying on a desk looks
    # WORN but has no boundary, and Horizon OS opens its Guardian setup flow,
    # under which every launch times out in window placement. Pause Guardian
    # for the run (developer property) and put back whatever was there.
    _, prev_guardian = adb_s(["shell", "getprop", "debug.oculus.guardian_pause"])
    prev_guardian = prev_guardian.strip() or "0"

    try:
        scriptable(True)
        adb_s(["shell", "setprop", "debug.oculus.guardian_pause", "1"])
        adb_s(["shell", "am", "broadcast", "-a", "com.oculus.vrpowermanager.prox_close"])
        adb_s(["shell", "setprop", "debug.ptv.verify", "1"])
        ctx.emit(stage, "info", f"installing {apk.name} on {serial}")
        rc, out = adb_s(["install", "-r", str(apk)], 600)
        if rc != 0:
            if not still_there():
                raise DeviceGone("the headset disconnected during install")
            raise RuntimeError(f"adb install failed: {out.strip()[:300]}")
        adb_s(["shell", "am", "force-stop", package])
        adb_s(["logcat", "-c"])
        adb_s(["logcat", "-b", "crash", "-c"])
        if streamer is not None:
            stop_stream = streamer(["-s", serial, "logcat", "-v", "time", "-s", "ptv:*"],
                                   log_file)
        # Plain ``am start``: ``-W`` waits for a "launch complete" a VR
        # NativeActivity on an unworn headset never reports. Confirm with
        # pidof instead. A launch issued while Horizon OS is still showing its
        # Guardian "headset taken off" interstitial times out in window
        # placement and is silently dropped, so let the interstitial clear
        # first and retry with a wake-up between attempts.
        sleep(LAUNCH_SETTLE_S)
        launched = False
        for attempt in range(LAUNCH_ATTEMPTS):
            if attempt:
                ctx.emit(stage, "info", f"launch attempt {attempt + 1}: the game "
                         "did not start (Guardian interstitial?); retrying")
                adb_s(["shell", "am", "broadcast", "-a",
                       "com.oculus.vrpowermanager.prox_close"])
                adb_s(["shell", "input", "keyevent", "KEYCODE_WAKEUP"])
                adb_s(["shell", "input", "keyevent", "KEYCODE_BACK"])
                sleep(LAUNCH_SETTLE_S)
            rc, out = adb_s(["shell", "am", "start", "-n",
                             f"{package}/android.app.NativeActivity"])
            if rc != 0 or "Error" in out:
                raise RuntimeError(f"could not launch {package}: {out.strip()[:300]}")
            for _ in range(LAUNCH_POLL_S):
                sleep(1.0)
                rc_p, pid_p = adb_s(["shell", "pidof", package])
                if rc_p == 0 and pid_p.strip():
                    launched = True
                    break
            if launched:
                break
        if not launched:
            if not still_there():
                raise DeviceGone("the headset disconnected while launching the game")
            raise RuntimeError(
                f"{package} did not start after {LAUNCH_ATTEMPTS} launches. The "
                "headset is probably showing a Guardian / boundary prompt: put it "
                "on once to clear it, or set quest_verify_store_pin so the stage "
                "can disable Guardian for the run")
        ctx.emit(stage, "info", f"watching the game for {capture:.0f} s")
        waited = 0.0
        while waited < capture:
            ctx.raise_if_cancelled()
            step = min(5.0, capture - waited)
            sleep(step)
            waited += step
            if not still_there():
                raise DeviceGone("the headset disconnected while the game was running")
        rc, pid = adb_s(["shell", "pidof", package])
        alive = rc == 0 and pid.strip() != ""
        if stop_stream is not None:
            log_text = stop_stream()
            stop_stream = None
        else:
            _, log_text = adb_s(["logcat", "-d", "-v", "time", "-s", "ptv:*"], 60)
        _, crash_text = adb_s(["logcat", "-b", "crash", "-d"], 30)
    except DeviceGone as exc:
        return finish_skipped(str(exc), device_serial=serial, package=package)
    finally:
        if stop_stream is not None:
            try:
                log_text = stop_stream()
            except Exception:  # noqa: BLE001 - best effort
                pass
        # Always leave the headset as it was found.
        for args in (["shell", "am", "force-stop", package],
                     ["shell", "setprop", "debug.ptv.verify", "0"],
                     ["shell", "setprop", "debug.oculus.guardian_pause", prev_guardian],
                     ["shell", "am", "broadcast", "-a",
                      "com.oculus.vrpowermanager.automation_disable"]):
            try:
                adb_s(args)
            except Exception:  # noqa: BLE001 - best effort cleanup
                pass
        try:
            scriptable(False)
        except Exception:  # noqa: BLE001 - best effort cleanup
            pass

    if not log_file.exists() or not log_file.read_text(encoding="utf-8", errors="replace"):
        log_file.write_text(log_text, encoding="utf-8")

    tel = parse_telemetry(log_text)
    checks, npc_summary = evaluate(tel, npc_plan, scene, process_alive=alive,
                                   crash_text=crash_text)
    failed = [c for c in checks if c.outcome == CheckOutcome.failed]
    status = VerificationStatus.failed if failed else VerificationStatus.passed
    reason = (f"{len(failed)} of {len(checks)} check(s) failed: "
              + "; ".join(f"{c.subject + ': ' if c.subject else ''}{c.name}"
                          for c in failed)[:1500]
              if failed else f"all {sum(1 for c in checks if c.outcome == CheckOutcome.passed)} applicable checks passed")
    result = RuntimeVerification(
        project_id=ctx.project_id, status=status, reason=reason,
        device_serial=serial, package=package, capture_seconds=capture,
        npc_summary=npc_summary, checks=checks, log_path=log_rel,
        started_at=started, finished_at=_now())
    rel = _write(ctx, result)
    for c in checks:
        if c.outcome != CheckOutcome.not_applicable:
            ctx.emit(stage, "info" if c.outcome == CheckOutcome.passed else "warning",
                     f"{c.outcome.value}: {c.category}"
                     f"{' ' + c.subject if c.subject else ''}: {c.name}"
                     f"{' -- ' + c.detail if c.detail else ''}"[:300])
    st = ctx.store.load_state(ctx.project_id)
    if failed:
        ctx.store.record_stage(st, stage, ProjectStatus.failed, message=reason,
                               artifacts=[rel, log_rel])
        ctx.emit(stage, "failed", reason)
        raise RuntimeError(f"runtime verification failed: {reason}")
    ctx.store.record_stage(st, stage, ProjectStatus.succeeded, message=reason,
                           artifacts=[rel, log_rel])
    ctx.emit(stage, "succeeded", reason)
    return StageResult(stage=stage, artifacts=[rel, log_rel], message=reason)
