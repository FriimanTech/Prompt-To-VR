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
4. With a game design, a second launch checks the game loop
   (``debug.ptv.verify_flow``); with player weapons in the scene, a third
   (``debug.ptv.verify_weapons``): a scripted hand holds and fires each
   weapon at a hostile, and ``evaluate_weapons`` checks the shot left the
   muzzle, hit, damaged through ctx.combat, kept the projectile pool inside
   its cap and held the frame rate.
5. Writes ``runtime_verification.json`` and ``logs/runtime/verify_<ts>.log``.

Report-only: a failed check fails the stage with the exact list of what did
not hold, and nothing is repaired automatically.
"""

from __future__ import annotations

import datetime as _dt
import json
import math
import os
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
from .quest_metrics import (low_memory_kills, parse_dumpsys_meminfo, parse_gpumeminfo,
                            parse_mem_lines)
from .texture_gallery import (judge, load_rgb, parse_gallery_lines, reference_stats,
                              screenshot_object_stats)

ARTIFACT = "runtime_verification.json"
#: Median distance between the DRAWN feet (lowest animated foot joint, less
#: its bind height above the soles) and the floor for a grounded NPC.
FEET_TOLERANCE_M = 0.05
#: A grounded NPC moving vertically faster than this for more than a couple
#: of samples is climbing or sinking, whatever its feet gap says.
VERTICAL_DRIFT_MPS = 1.0
#: A hovering NPC's median sole height may differ from its hover height by this.
HOVER_TOLERANCE_M = 0.15
#: Median heading error, while moving, against the travel direction or target.
FACING_TOLERANCE_DEG = 20.0
#: Moving samples needed before the facing check judges at all.
MIN_FACING_SAMPLES = 3
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
            for key in ("feet_gap", "origin_gap", "facing_err_deg", "vy"):
                try:
                    kv[key] = float(kv.get(key, "nan"))
                except ValueError:
                    kv[key] = math.nan
            kv["_tags"] = [x for x in str(kv.get("tags", "-")).split(",") if x and x != "-"]
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


#: Behaviours that move an NPC toward / around someone it has NOTICED; with
#: nobody within sight or hearing it rightly holds its post.
_ENGAGE_BEHAVIOURS = {"chase", "ranged_attack", "melee_attack", "contact_damage", "take_cover",
                      "reposition", "guard_point", "flee"}


def _moves_expected(npc: Any, behaviours: set[str], samples: list[dict]) -> tuple[bool, str]:
    """Should this moving NPC have moved during the capture? A patrol always
    should; an NPC that only engages what it notices should only when the
    player came within its sight / hearing (the scripted walk stays near
    the spawn, and an insurgent 50 m away in a hut has no one to engage)."""
    if "patrol" in behaviours or not behaviours & _ENGAGE_BEHAVIOURS:
        return True, ""
    perception = getattr(npc, "perception", None)
    notice = max(float(getattr(perception, "sight_range_m", 25.0) or 0.0),
                 float(getattr(perception, "hearing_range_m", 0.0) or 0.0))
    dists = [float(s["dist_player"]) for s in samples
             if isinstance(s.get("dist_player"), (int, float)) and not math.isnan(s["dist_player"])]
    if not dists:
        return True, ""
    closest = min(dists)
    if closest > notice + 1.0:
        return False, (f"the player never came within its {notice:.0f} m notice range "
                       f"(closest {closest:.0f} m); it had no one to engage")
    return True, ""


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
        if not samples:
            return
        motor = str(samples[-1].get("motor") or "grounded")
        # ``feet_gap`` is measured from the DRAWN feet (lowest animated
        # foot/toe joint), not from the declared foot offset, so a model that
        # floats or sinks relative to its capsule fails here even when the
        # contract looks right.
        gaps = [s["feet_gap"] for s in samples if not math.isnan(s["feet_gap"])]
        if motor == "flying":
            checks.append(_check("stands on the floor", category, None,
                                 "flies freely; no floor height applies", subject))
        elif not gaps:
            checks.append(_check("stands on the floor", category, False,
                                 "no floor found under it in any sample", subject))
        elif motor.startswith("hover:"):
            hover = float(motor.split(":", 1)[1])
            median = sorted(gaps)[len(gaps) // 2]
            ok = abs(median - hover) <= HOVER_TOLERANCE_M
            checks.append(_check(
                "holds its hover height", category, ok,
                f"feet {median:+.2f} m above the floor (median), asked {hover:.2f} m",
                subject))
        else:
            worst = max(gaps, key=abs)
            median = sorted(gaps)[len(gaps) // 2]
            ok = abs(median) <= FEET_TOLERANCE_M and abs(worst) <= 1.0
            checks.append(_check(
                "stands on the floor", category, ok,
                f"drawn feet {median:+.2f} m from the floor (median), "
                f"worst {worst:+.2f} m over {len(gaps)} samples", subject))
        # A grounded body climbing or sinking steadily is broken even when
        # its floor probe is fooled: a held rifle once read as "floor" and
        # the NPC rose 41 m/s while reporting a constant -0.63 m gap.
        if motor != "flying" and not motor.startswith("hover:"):
            vys = [s["vy"] for s in samples[2:] if not math.isnan(s.get("vy", math.nan))]
            if vys:
                drifting = [v for v in vys if abs(v) > VERTICAL_DRIFT_MPS]
                checks.append(_check(
                    "does not drift vertically", category, len(drifting) <= 2,
                    f"{len(drifting)}/{len(vys)} samples moving vertically faster than "
                    f"{VERTICAL_DRIFT_MPS:.1f} m/s (worst {max(vys, key=abs):+.2f} m/s)",
                    subject))
        faced = [s["facing_err_deg"] for s in samples
                 if s["speed"] > 0.3 and not math.isnan(s["facing_err_deg"])]
        if 0 < len(faced) < MIN_FACING_SAMPLES:
            # One sample a second: a single one is as likely mid-turn as not.
            checks.append(_check(
                "faces where it is going", category, None,
                f"only {len(faced)} moving sample(s); {MIN_FACING_SAMPLES} needed to judge",
                subject))
        elif faced:
            median = sorted(faced)[len(faced) // 2]
            checks.append(_check(
                "faces where it is going", category, median <= FACING_TOLERANCE_DEG,
                f"median {median:.0f} deg off its travel direction / target over "
                f"{len(faced)} moving samples (allowed {FACING_TOLERANCE_DEG:.0f})",
                subject))
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
                outcome, detail = _moves_expected(npc, behaviours, samples)
                checks.append(_check("moves", "persistent",
                                     bool(moved) if outcome else None,
                                     f"{len(moved)} samples faster than 0.3 m/s" if outcome
                                     else detail, eid))

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
            band = zone_band_check(npc.asset_id, mine, status_by_e, npc_plan, scene)
            if band is not None:
                checks.append(band)
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


#: How far outside its zone's radius a spawned NPC may first be seen.
ZONE_SLACK_M = 1.5


def zone_band_check(asset_id: str, spawns: list[dict], status_by_e: dict[str, list[dict]],
                    npc_plan: Any, scene: dict) -> Optional[VerificationCheck]:
    """An NPC type the plan put in a place ("front_band") is first seen in a
    zone carrying that tag; None when the plan does not place it."""
    enc = getattr(npc_plan, "encounters", None)
    tags = sorted({t for w in (enc.waves if enc else []) for g in w.groups
                   if g.asset_id == asset_id for t in g.zone_tags})
    if not tags:
        return None
    director = next((b for e in scene.get("entities") or [] for b in e.get("mechanic_bindings") or []
                     if isinstance(b, dict)
                     and (b.get("builtin_id") or b.get("id")) == "builtin.encounter_director"), None)
    zones = [z for z in ((director or {}).get("params") or {}).get("zones") or []
             if isinstance(z, dict) and ({z.get("id"), *(z.get("tags") or [])} & set(tags))]
    if not zones:
        return _check("spawns in its zone", "encounter", False,
                      f"no npc_spawn_zone carries {tags}", asset_id)
    inside = total = 0
    worst = ""
    for s in spawns:
        first = next((x["_pos"] for x in status_by_e.get(s["e"], []) if x.get("_pos")), None)
        if first is None:
            continue
        total += 1
        d = min(math.hypot(first[0] - float(z["position"][0]), first[2] - float(z["position"][2]))
                - float(z.get("radius") or 2.0) for z in zones)
        if d <= ZONE_SLACK_M:
            inside += 1
        else:
            worst = f"{s['e']} first seen {d:.1f} m outside its zone(s)"
    if total == 0:
        return _check("spawns in its zone", "encounter", None, "no position samples", asset_id)
    return _check("spawns in its zone", "encounter", inside == total,
                  f"{inside}/{total} instance(s) inside a zone tagged {tags}"
                  + (f"; {worst}" if worst else ""), asset_id)


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
# The game-loop pass
# ---------------------------------------------------------------------------

#: builtin.game_flow's scripted verification: each life is ended after ~12 s,
#: an end screen is clicked after ~3 s, the winning attempt runs timers 10x
#: and forces non-timer objectives after ~15 s, the last attempt pauses and
#: leaves after ~10 s. Plus launch and fades.
FLOW_PER_LIFE_S = 17.0
FLOW_FIXED_S = 75.0
MAX_FLOW_CAPTURE_S = 300.0


def flow_capture_seconds(design: Any) -> float:
    lives = design.lives.count if design.lives.count is not None else 1
    deaths = lives if design.can_die() else 0
    timed = max((o.duration_s or 0.0 for o in design.objectives
                 if o.kind.value == "survive_time"), default=0.0)
    return min(MAX_FLOW_CAPTURE_S, FLOW_FIXED_S + deaths * FLOW_PER_LIFE_S + timed / 10.0)


def parse_flow(text: str) -> list[dict[str, str]]:
    """Every ``flow:`` line, as key/value pairs, in order. A line without
    ``key=value`` pairs keeps its text under ``_text``."""
    out: list[dict[str, str]] = []
    for raw in text.splitlines():
        if "flow:" not in raw:
            continue
        body = raw.split("flow:", 1)[1].strip()
        kv = _kv(body)
        kv["_text"] = body
        out.append(kv)
    return out


def evaluate_flow(events: list[dict[str, str]], design: Any, *, exited: bool,
                  relaunched: bool) -> list[VerificationCheck]:
    """What this game's loop promises, checked against what it did.

    Only checks the design calls for: a game that cannot kill the player is
    checked for never dying; one with nothing to win is not asked to win."""
    cat = "game_flow"
    states = [e for e in events if "state" in e]
    buttons = [e.get("button") for e in events if "button" in e]
    starts = [e for e in states if e.get("state") == "playing" and e.get("why") == "start"]
    checks = [_check("game loop starts", cat, bool(starts),
                     "" if starts else "no 'flow: state=playing why=start' line",
                     evidence=[e["_text"] for e in states[:3]])]
    if not starts:
        return checks
    can_die = design.can_die()
    lost = [e for e in states if e.get("state") == "life_lost"]
    if not can_die:
        checks.append(_check("the player cannot die", cat, not lost,
                             f"{len(lost)} life lost event(s) in a game with no failure"))
    else:
        checks.append(_check("health reaching zero costs a life", cat, bool(lost),
                             f"{len(lost)} life lost event(s)",
                             evidence=[e["_text"] for e in lost[:3]]))
        lives = design.lives.count
        if lives is None or lives > 1:
            respawned = [e for e in states if e.get("state") == "respawning"]
            checks.append(_check("the player respawns", cat, bool(respawned),
                                 f"{len(respawned)} respawn(s)",
                                 evidence=[e["_text"] for e in events
                                           if e["_text"].startswith("respawned")][:3]))
        if lives is not None:
            over = [e for e in states if e.get("state") == "game_over"]
            ok = bool(over) and over[0].get("lives") == "0"
            checks.append(_check("Game Over follows the last life", cat, ok,
                                 f"game over events: {[e['_text'] for e in over[:2]]}"))
            checks.append(_check("Try Again restarts the game", cat,
                                 "try_again" in buttons and len(starts) >= 2,
                                 f"buttons {buttons}; {len(starts)} start(s)"))
            if len(starts) >= 2:
                fresh = starts[1].get("lives") == str(lives)
                checks.append(_check("a restart resets lives", cat, fresh,
                                     f"attempt 2 started with lives={starts[1].get('lives')} "
                                     f"(want {lives})"))
    can_win = design.victory.kind.value != "none" and bool(design.objectives)
    if can_win:
        won = [e for e in states if e.get("state") in ("victory", "completed")]
        checks.append(_check("winning ends on the victory screen", cat, bool(won),
                             f"{len(won)} win(s)", evidence=[e["_text"] for e in won[:2]]))
        if won:
            checks.append(_check("Play Again restarts the game", cat,
                                 "play_again" in buttons and len(starts) >= (3 if can_die and design.lives.count is not None else 2),
                                 f"buttons {buttons}; {len(starts)} start(s)"))
    left = "leave_game" in buttons
    checks.append(_check("Leave Game ends the app", cat, left and exited,
                         ("" if left else "Leave Game was never chosen; ")
                         + ("the process exited" if exited else "the process was still running")))
    if exited:
        checks.append(_check("the app launches again after leaving", cat, relaunched,
                             "" if relaunched else "the second launch did not start: the "
                             "activity was not finished cleanly"))
    return checks


# ---------------------------------------------------------------------------
# The player's hands (Hands Only games: the engine's default rigged hands)
# ---------------------------------------------------------------------------

#: A drawn default hand's joints span this much (0.27 m hand: ~0.26 m).
HAND_SPAN_RANGE_M = (0.08, 0.45)
#: A placed hand is within this of the head (held, or scripted in verify).
HAND_REACH_M = 1.2


def evaluate_hands(text: str, scene: dict) -> list[VerificationCheck]:
    """Hands Only: both default hands drawn, finite, hand-sized and near the
    head (``hands:`` telemetry). They once loaded 20 m long -- a room the
    player stood inside -- and every other check still passed."""
    source = (((scene.get("player") or {}).get("hands") or {}).get("source")
              or scene.get("hands_source") or "default")
    if source != "default":
        return []
    lines = [l.split("hands:", 1)[1] for l in text.splitlines() if "hands:" in l]
    good = []
    for body in lines:
        sides = [s for s in body.split("|")]
        ok = len(sides) == 2
        for s in sides:
            kv = _kv(s.replace("L:", "").replace("R:", ""))
            span = _f(kv.get("joint_span_m"))
            ok = ok and kv.get("drawn") == "1" and kv.get("finite") == "1" \
                and HAND_SPAN_RANGE_M[0] <= span <= HAND_SPAN_RANGE_M[1] \
                and _f(kv.get("head_m"), 99.0) <= HAND_REACH_M
        if ok:
            good.append(body.strip())
    return [_check("the player's default hands are drawn", "base", bool(good),
                   f"{len(good)} of {len(lines)} 'hands:' sample(s) had both hands drawn, "
                   f"finite, {HAND_SPAN_RANGE_M[0]}-{HAND_SPAN_RANGE_M[1]} m and within "
                   f"{HAND_REACH_M} m of the head"
                   + ("" if lines else " (no 'hands:' telemetry at all)"),
                   evidence=(good or [l.strip() for l in lines])[:3])]


def evaluate_body_draws(text: str, scene: dict) -> list[VerificationCheck]:
    """Nothing but the player's own hands / body / held items draws on their
    hands and feet, and a generated game draws no placeholder boxes
    (``render_body:`` telemetry). The player's collider proxies once drew as
    white 0.3 m cubes over every finger, in both Hands Only and Full Player
    Avatar games, and every other check passed."""
    lines = [_kv(l.split("render_body:", 1)[1]) for l in text.splitlines()
             if "render_body:" in l]
    if not lines:
        return [_check("nothing draws over the player's hands / body", "base", None,
                       "no 'render_body:' telemetry (engine built before it existed)")]
    worst = max(lines, key=lambda kv: int(_f(kv.get("player_draws"))))
    over = int(_f(worst.get("player_draws")))
    checks = [_check(
        "nothing draws over the player's hands / body", "base", over == 0,
        f"{over} foreign draw(s) at a hand or foot in the worst second"
        + (f" ({worst.get('names')})" if over else "")
        + f" over {len(lines)} sample(s)")]
    if scene.get("prompt_id") or scene.get("environment_builder_id"):
        boxes = max(int(_f(kv.get("placeholders"))) for kv in lines)
        checks.append(_check(
            "no placeholder boxes in a generated game", "base", boxes == 0,
            f"{boxes} meshless entit(ies) drawn as placeholder boxes in the worst second "
            "(the log names each one: 'render: ... drawn as a placeholder box')"))
    return checks


#: TextureGallery.h timing (settle, per asset) plus launch slack.
GALLERY_SETTLE_S = 6.0
GALLERY_SLOT_S = 4.0
GALLERY_SLACK_S = 30.0


def texture_gallery_wanted(scene: dict) -> bool:
    """Run the gallery pass when the scene places textured meshes."""
    perf = scene.get("performance") or {}
    return int((perf.get("textures") or {}).get("textured_mesh_entities") or 0) > 0


def gallery_capture_seconds(scene: dict) -> float:
    meshes = {((e.get("mesh") or {}).get("lod_paths") or [None])[0]
              or (e.get("mesh") or {}).get("glb_path")
              for e in scene.get("entities") or [] if isinstance(e.get("mesh"), dict)}
    meshes.discard(None)
    # Engine hands are textured too and are part of the gallery.
    return GALLERY_SETTLE_S + GALLERY_SLOT_S * (len(meshes) + 2) * 1.6 + GALLERY_SLACK_S


def evaluate_texture_gallery(ctx: PipelineContext, shots: list[dict[str, Any]],
                             scene: dict) -> list[VerificationCheck]:
    """One check per shown asset: visible, no magenta, still colourful, not
    turned white -- against an offline render of the same LOD."""
    import subprocess
    import tempfile

    cat = "textures"
    bg = next((s for s in shots if s.get("name") == "background"), None)
    assets = [s for s in shots if s.get("entry")]
    if not assets:
        return [_check("texture gallery screenshots", cat, False,
                       "no asset was shown or captured (is the Quest screenshot service "
                       "available, and did the engine log 'texgallery: show'?)")]
    bg_img = load_rgb(bg["file"]) if bg and bg.get("file") else None
    if bg_img is None:
        return [_check("texture gallery screenshots", cat, False,
                       "no background screenshot to separate the assets from")]
    # The second background can land after the first asset appeared (it did:
    # the blaster read "0.09% of the view" against a background showing the
    # blaster). Judge each asset against whichever background it differs from.
    backgrounds = [bg_img] + [img for s in shots if s.get("name") == "background_first"
                              and s.get("file") for img in [load_rgb(s["file"])]
                              if img is not None]
    views_tool = None
    root = ctx.settings.engine_dir / "build"
    exe = "ptv_glb_views.exe" if os.name == "nt" else "ptv_glb_views"
    found = sorted(root.rglob(exe), key=lambda p: p.stat().st_mtime) if root.exists() else []
    if found:
        views_tool = found[-1]
    checks: list[VerificationCheck] = []
    for shot in assets:
        entry = shot["entry"]
        name = Path(entry.get("path", "?")).stem
        img = load_rgb(shot["file"]) if shot.get("file") else None
        if img is None:
            checks.append(_check("texture shows on the headset", cat, False,
                                 "screenshot missing", subject=name))
            continue
        stats = max((screenshot_object_stats(img, b) for b in backgrounds),
                    key=lambda s: s.get("share", 0.0))
        ref: dict[str, float] = {}
        glb = ctx.settings.repo_root / entry.get("path", "")
        if views_tool is not None and glb.is_file():
            with tempfile.TemporaryDirectory() as tmp:
                proc = subprocess.run([str(views_tool), "--input", str(glb), "--out-dir", tmp,
                                       "--size", "256"], capture_output=True, text=True)
                if proc.returncode == 0:
                    ref = reference_stats(Path(tmp) / "view_6.png", Path(tmp) / "view_6.ids", 256)
        ok, notes = judge(stats, ref, int(entry.get("textured") or 0),
                          int(entry.get("missing") or 0))
        detail = (f"object {stats.get('share', 0):.1%} of view, chroma {stats.get('chroma', 0):.1f}"
                  f" (reference {ref.get('chroma', 0):.1f}), white {stats.get('white', 0):.0%}"
                  f" (reference {ref.get('white', 0):.0%}), magenta {stats.get('magenta', 0):.1%}"
                  f"; textured submeshes {entry.get('textured', 0)}"
                  + (f"; {'; '.join(notes)}" if notes else ""))
        rel = str(shot["file"].relative_to(ctx.settings.repo_root)).replace("\\", "/")
        checks.append(_check("texture shows on the headset", cat, ok, detail, subject=name,
                             evidence=[rel]))
    return checks


#: Leak and soak limits (MB).
LEAK_MB_PER_CYCLE = 50.0
SOAK_MB_PER_MIN = 5.0
SOAK_WARMUP_S = 60.0


def evaluate_memory(*, samples: list[dict[str, Any]], gpu: dict[str, float], base_text: str,
                    flow_text: str, soak: list[tuple[float, float]], pressure_text: str,
                    pressure_alive: Optional[bool], kills: list[str], scene: dict,
                    compromises: Optional[list[str]] = None):
    """Quest 3 memory checks against the engine targets (4.5 GiB steady,
    5.0 GiB peak) and Meta's 5.75 GiB limit, from measured data only."""
    from ptv_schemas import MemoryReport, MemoryStatus

    perf = scene.get("performance") or {}
    target = float(perf.get("memory_target_mb") or 4608.0)
    ceiling = float(perf.get("memory_ceiling_mb") or 5120.0)
    limit = float(perf.get("memory_limit_mb") or 5888.0)
    est = perf.get("memory_estimate") or {}
    cat = "memory"
    checks: list[VerificationCheck] = []
    dumpsys = [s["total_pss_kb"] / 1024.0 for s in samples if s.get("total_pss_kb")]
    engine = parse_mem_lines(base_text) + parse_mem_lines(flow_text)
    measured_all = dumpsys + [float(x) for x in engine] + [p for _, p in soak]
    report = MemoryReport(status=MemoryStatus.failed_on_device, limit_mb=limit,
                          ceiling_mb=ceiling, target_mb=target,
                          estimated_steady_mb=est.get("steady_mb"),
                          estimated_peak_mb=est.get("peak_mb"),
                          compromises=list(compromises or []),
                          low_memory_kills=kills[:10])
    if not measured_all:
        checks.append(_check("memory measured on the headset", cat, False,
                             "no dumpsys meminfo sample and no engine 'mem:' line"))
        report.reasons.append("no memory measurement")
        return checks, report
    peak = max(measured_all)
    tail = dumpsys[len(dumpsys) // 2:] or [float(x) for x in engine[len(engine) // 2:]]
    steady = sorted(tail)[len(tail) // 2] if tail else peak
    report.measured_peak_mb = round(peak, 1)
    report.measured_steady_mb = round(steady, 1)
    if samples:
        last = samples[-1]
        for k, label in (("native_heap_kb", "native_heap"), ("graphics_kb", "graphics"),
                         ("gl_mtrack_kb", "gl_mtrack"), ("total_pss_kb", "total_pss")):
            if last.get(k):
                report.measured[label] = round(last[k] / 1024.0, 1)
    for k, v in gpu.items():
        report.measured[f"gpu_{k}"] = v
    checks.append(_check("peak PSS under the 5.0 GiB ceiling", cat, peak <= ceiling,
                         f"peak {peak:.0f} MB (ceiling {ceiling:.0f}, Meta limit {limit:.0f})"))
    checks.append(_check("steady PSS under the 4.5 GiB target", cat, steady <= target,
                         f"steady {steady:.0f} MB (target {target:.0f})"
                         + (f"; estimated {est.get('steady_mb'):.0f} MB" if est.get("steady_mb")
                            else "")))
    checks.append(_check("no low-memory kill", cat, not kills,
                         f"{len(kills)} lowmemorykiller line(s) name the game" if kills else
                         "no lowmemorykiller / lmkd line names the game",
                         evidence=kills[:5]))
    # Leaks across Try Again / Play Again: PSS a few seconds into each start.
    starts: list[float] = []
    pss_now: Optional[float] = None
    pending = False
    for line in flow_text.splitlines():
        m = re.search(r"mem:\s*pss\s*(\d+)", line)
        if m:
            pss_now = float(m.group(1))
            if pending:
                starts.append(pss_now)
                pending = False
        elif "flow:" in line and "state=playing" in line:
            pending = True
    if len(starts) >= 2:
        per_cycle = (starts[-1] - starts[0]) / (len(starts) - 1)
        report.leak_mb_per_cycle = round(per_cycle, 1)
        checks.append(_check("no leak across restarts", cat, per_cycle <= LEAK_MB_PER_CYCLE,
                             f"{per_cycle:+.1f} MB per cycle over {len(starts) - 1} restart(s) "
                             f"(limit {LEAK_MB_PER_CYCLE:.0f})"))
    else:
        checks.append(_check("no leak across restarts", cat, None,
                             "fewer than two game starts in the game-loop pass"))
    if soak:
        warm = [(t, p) for t, p in soak if t >= SOAK_WARMUP_S]
        if len(warm) >= 3:
            n = len(warm)
            mt = sum(t for t, _ in warm) / n
            mp = sum(p for _, p in warm) / n
            den = sum((t - mt) ** 2 for t, _ in warm) or 1.0
            slope = sum((t - mt) * (p - mp) for t, p in warm) / den * 60.0
            checks.append(_check("no growth during the soak", cat, slope <= SOAK_MB_PER_MIN,
                                 f"{slope:+.2f} MB/min after warm-up over "
                                 f"{warm[-1][0] - warm[0][0]:.0f} s (limit {SOAK_MB_PER_MIN:.0f})"))
    if pressure_text:
        reached_red = bool(re.search(r"mem: level (red|critical)", pressure_text))
        recovered = bool(re.search(r"mem: level \w+ -> (green|yellow)", pressure_text))
        ok = reached_red and bool(pressure_alive)
        checks.append(_check("memory pressure handled", cat, ok,
                             f"reached red: {reached_red}; still running: {pressure_alive}; "
                             f"stepped back down: {recovered}"))
    failed = [c for c in checks if c.outcome == CheckOutcome.failed]
    if failed:
        report.status = MemoryStatus.failed_on_device
        report.reasons = [c.name + ": " + c.detail for c in failed]
    else:
        report.status = MemoryStatus.verified_safe
        report.reasons = [f"measured on a Quest 3: peak {peak:.0f} MB, steady {steady:.0f} MB"]
    return checks, report


def _estimated_memory(ctx: PipelineContext):
    """No headset ran the game: the build-time estimate, never 'verified'."""
    from ptv_schemas import MemoryReport, MemoryStatus

    try:
        scene = json.loads((ctx.project_dir / "scene.json").read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    perf = scene.get("performance") or {}
    est = perf.get("memory_estimate") or {}
    if not est:
        return None
    over = est.get("outcome") == "over"
    return MemoryReport(
        status=MemoryStatus.at_risk if over else MemoryStatus.estimated_safe_not_device_tested,
        reasons=[("estimate over the target after corrections; " if over else "")
                 + f"not tested on a Quest 3 (estimate {float(est.get('steady_mb') or 0) / 1024:.1f}"
                 f" of {float(perf.get('memory_target_mb') or 4608) / 1024:.1f} GiB)"],
        target_mb=float(perf.get("memory_target_mb") or 4608.0),
        ceiling_mb=float(perf.get("memory_ceiling_mb") or 5120.0),
        limit_mb=float(perf.get("memory_limit_mb") or 5888.0),
        estimated_steady_mb=est.get("steady_mb"), estimated_peak_mb=est.get("peak_mb"))


def _record_memory(ctx: PipelineContext, memory) -> None:
    """Device-measured memory status for the project page, calibration from
    estimate versus measurement, and the feedback a correction round reads."""
    from . import memory_model as mm

    status_path = ctx.project_dir / "memory_status.json"
    status_path.write_text(json.dumps({
        "status": memory.status.value, "source": "device", "reasons": memory.reasons,
        "measured_peak_mb": memory.measured_peak_mb,
        "measured_steady_mb": memory.measured_steady_mb,
        "estimated_peak_mb": memory.estimated_peak_mb,
        "compromises": memory.compromises,
    }, indent=2), encoding="utf-8")
    if memory.estimated_peak_mb and memory.measured_peak_mb:
        mm.update_calibration(ctx.settings.projects_dir / "memory_calibration.json",
                              ctx.project_id, float(memory.estimated_peak_mb),
                              float(memory.measured_peak_mb))
    fb = ctx.project_dir / "memory_feedback.json"
    # Only failures smaller budgets can fix start a correction round; a leak
    # or a pressure-handling failure is reported, not rebuilt around.
    correctable = any(r.startswith(("peak PSS", "steady PSS", "no low-memory kill"))
                      for r in memory.reasons)
    if memory.status.value == "failed_on_device" and correctable:
        prev = {}
        if fb.exists():
            try:
                prev = json.loads(fb.read_text(encoding="utf-8"))
            except (OSError, ValueError):
                prev = {}
        ratio = None
        if memory.estimated_peak_mb and memory.measured_peak_mb:
            ratio = round(float(memory.measured_peak_mb) / float(memory.estimated_peak_mb), 3)
        fb.write_text(json.dumps({
            "round": int(prev.get("round") or 0) + 1, "reasons": memory.reasons,
            "measured": memory.measured, "measured_peak_mb": memory.measured_peak_mb,
            "estimated_peak_mb": memory.estimated_peak_mb, "ratio": ratio,
        }, indent=2), encoding="utf-8")
    elif fb.exists():
        fb.unlink()


def evaluate_render_health(text: str) -> list[VerificationCheck]:
    """The optimized render path works as built: FFR's density map reached the
    render pass (otherwise nothing draws), merged static batches stayed intact
    (a moved member dissolves its batch -- correct, but the compiler misjudged
    it), and no GPU allocation had to be recovered."""
    cat = "render"
    out: list[VerificationCheck] = []
    fdm = [l.strip() for l in text.splitlines() if "no fragment density map image" in l]
    out.append(_check("FFR density map reaches the render pass", cat, not fdm,
                      f"{len(fdm)} frame(s) had no density map" if fdm else "",
                      evidence=fdm[:3]))
    dissolved = [l.strip() for l in text.splitlines() if "static batch" in l and "dissolved" in l]
    if dissolved:
        out.append(VerificationCheck(
            name="static batches stayed intact", category=cat, outcome=CheckOutcome.warning,
            detail="a member the compiler marked static moved; its batch now draws "
                   "per entity", evidence=dissolved[:3]))
    oom = [l.strip() for l in text.splitlines() if "GPU allocation failed" in l]
    if oom:
        out.append(VerificationCheck(
            name="no GPU allocation failure", category=cat, outcome=CheckOutcome.warning,
            detail=f"{len(oom)} allocation(s) needed out-of-memory recovery",
            evidence=oom[:3]))
    return out


def evaluate_locks(text: str, scene: dict) -> list[VerificationCheck]:
    """The user's locked optimization settings held on the headset.

    From every ``perf:`` line: "FFR off" means ffr 0 throughout, "fixed"
    exactly that level, "max" never above it; resolution "off"/"high" means
    the render scale never left 1.0. A frame-rate miss while the governor was
    blocked by the user's own locks is a WARNING, never a failure."""
    from .quest_metrics import parse_perf_lines

    locks = (scene.get("performance") or {}).get("locks") or {}
    active = {k: v for k, v in locks.items()
              if isinstance(v, dict) and v.get("value", "auto") != "auto"}
    if not active:
        return []
    rows = parse_perf_lines(text)
    cat = "optimization"
    out: list[VerificationCheck] = []
    ffr_vals = [int(r["ffr"]) for r in rows if "ffr" in r]
    scale_vals = [r["scale"] for r in rows if "scale" in r]
    ffr = active.get("ffr")
    if ffr:
        v, lvl = ffr.get("value"), ffr.get("level")
        if v == "off":
            ok = bool(ffr_vals) and all(x == 0 for x in ffr_vals)
            want = "level 0 throughout"
        elif v == "fixed":
            ok = bool(ffr_vals) and all(x == int(lvl or 2) for x in ffr_vals)
            want = f"level {int(lvl or 2)} throughout"
        else:
            ok = bool(ffr_vals) and all(x <= int(lvl if lvl is not None else 1) for x in ffr_vals)
            want = f"never above level {int(lvl if lvl is not None else 1)}"
        out.append(_check(f"user lock held: FFR {v}", cat, ok,
                          f"wanted {want}; observed levels {sorted(set(ffr_vals)) or 'none'}",
                          evidence=[f"quote: {ffr.get('quote', '')}"]))
    res = active.get("resolution")
    if res:
        ok = bool(scale_vals) and all(abs(s - 1.0) < 1e-3 for s in scale_vals)
        out.append(_check(f"user lock held: resolution {res.get('value')}", cat, ok,
                          f"render scale observed {sorted(set(round(s, 2) for s in scale_vals)) or 'none'}",
                          evidence=[f"quote: {res.get('quote', '')}"]))
    blocked = [l.strip() for l in text.splitlines() if "blocked by user settings" in l]
    if blocked:
        out.append(VerificationCheck(
            name="frame rate below target because of user-chosen settings", category=cat,
            outcome=CheckOutcome.warning,
            detail="the governor had no unlocked quality step left; the user's locked "
                   "settings are kept as requested",
            evidence=blocked[:3]))
    return out


def evaluate_textures(text: str, scene: dict) -> list[VerificationCheck]:
    """Every base-colour texture the game references loaded on the headset.

    From the engine's 1 Hz ``tex:`` line (resident / missing counts, how many
    came from ASTC KTX2 versus a PNG/JPEG decoded on device) and its
    ``textures: MISSING`` errors, which name the asset, material and file. A
    scene whose placed meshes reference textures must end with at least one
    resident and none missing."""
    perf = scene.get("performance") or {}
    textured = int((perf.get("textures") or {}).get("textured_mesh_entities") or 0)
    rows = []
    for line in text.splitlines():
        if "tex:" not in line:
            continue
        body = line.split("tex:", 1)[1]
        kv: dict[str, str] = {}
        for part in body.split("|"):
            bits = part.strip().split()
            if len(bits) >= 2:
                kv[bits[0]] = bits[1]
        rows.append(kv)
    missing_lines = [l.strip() for l in text.splitlines() if "textures: MISSING" in l]
    if not rows:
        return [_check("textures load on the headset", "base", textured == 0,
                       "no 'tex:' telemetry" + ("" if textured == 0 else
                                                f" although {textured} placed mesh(es) "
                                                "reference textures"))]
    last = rows[-1]
    missing = int(_f(last.get("missing")))
    resident = int(_f(last.get("resident")))
    ktx2 = int(_f(last.get("ktx2")))
    decoded = int(_f(last.get("decoded")))
    ok = missing == 0 and not missing_lines and (textured == 0 or resident > 0)
    detail = (f"{resident} texture(s) resident ({ktx2} ASTC KTX2, {decoded} decoded on "
              f"device), {missing} missing; {textured} placed mesh(es) are textured")
    checks = [_check("textures load on the headset", "base", ok, detail,
                     evidence=(missing_lines or [str(last)])[:4])]
    if textured > 0:
        checks.append(_check("textures use the headset's ASTC format", "base",
                             decoded == 0 or ktx2 > 0,
                             f"{ktx2} KTX2, {decoded} PNG/JPEG decoded on the headset"))
    return checks


# ---------------------------------------------------------------------------
# Weapons pass (debug.ptv.verify_weapons): a scripted hand holds and fires
# every player weapon -- see engine/runtime/src/core/WeaponVerifier.h.
# ---------------------------------------------------------------------------

#: Engine kSettleS plus launch slack, then one budget per weapon (hold 1.2 s,
#: fire up to 5.5 s, release 0.8 s, plus margin).
WEAPONS_FIXED_S = 25.0
WEAPON_PASS_S = 9.0
MAX_WEAPONS_CAPTURE_S = 240.0
#: A shot must leave within this of the muzzle socket.
SHOT_ORIGIN_TOLERANCE_CM = 2.0
#: A beam held for 2 s must register at least this many damage ticks.
BEAM_MIN_HITS = 10
#: Headroom on the p95 frame time for vsync timing jitter, and the share of
#: frames that may miss a vsync during sustained fire.
FRAME_JITTER = 0.15
MAX_DROPPED_FRAMES = 0.02


def scene_weapons(scene: dict) -> list[dict]:
    """The weapons the player wields: entities bound to builtin.weapon /
    builtin.bow that are not templates, NPC gear or NPCs themselves."""
    out = []
    for e in scene.get("entities") or []:
        if "template" in (e.get("tags") or []) or e.get("attached_to") or e.get("faction"):
            continue
        ids = [b.get("builtin_id") or b.get("id") for b in e.get("mechanic_bindings") or []]
        kind = "bow" if "builtin.bow" in ids else "gun" if "builtin.weapon" in ids else None
        if kind is not None:
            out.append({"id": e.get("id"), "kind": kind})
    return out


def weapons_capture_seconds(count: int) -> float:
    return min(MAX_WEAPONS_CAPTURE_S, WEAPONS_FIXED_S + WEAPON_PASS_S * count)


def parse_weapons(text: str) -> list[dict[str, str]]:
    out: list[dict[str, str]] = []
    for raw in text.splitlines():
        if "weapons:" not in raw:
            continue
        body = raw.split("weapons:", 1)[1].strip()
        kv = _kv(body)
        kv["_event"] = body.split(" ", 1)[0]
        kv["_text"] = body
        out.append(kv)
    return out


def _f(v: Optional[str], default: float = 0.0) -> float:
    try:
        return float(v) if v is not None else default
    except ValueError:
        return default


def evaluate_weapons(events: list[dict[str, str]], scene: dict) -> list[VerificationCheck]:
    """Each player weapon: held, fires from its muzzle, hits and damages a
    target through ctx.combat, keeps the projectile pool inside its cap and
    the frame rate up under sustained fire."""
    cat = "weapons"
    weapons = scene_weapons(scene)
    if not weapons:
        return []
    begun = [e for e in events if e["_event"] == "begin"]
    done = [e for e in events if e["_event"] == "done"]
    checks = [_check("weapons pass ran to the end", cat, bool(begun and done),
                     "" if done else "no 'weapons: done' line (crash, or the capture ended early)",
                     evidence=[e["_text"] for e in (begun + done)[:2]])]
    if not begun:
        return checks
    perf = scene.get("performance") or {}
    fps = float(perf.get("target_fps") or 72)
    frame_budget_ms = 1000.0 / fps
    cap = int(perf.get("max_live_projectiles") or 64)
    for w in weapons:
        name = w["id"]
        mine = [e for e in events if e.get("weapon") == name]
        hold = next((e for e in mine if e["_event"] == "hold"), None)
        finished = next((e for e in mine if e["_event"] == "weapon_done"), None)
        shots = [e for e in mine if e["_event"] == "shot"]
        # Only hits on something: a beam playing over the wall behind a
        # drone "hits" every tick and damages nothing.
        hits = [e for e in mine if e["_event"] in ("hit", "projectile_hit")
                and e.get("target", "world") != "world"]
        if hold is None:
            checks.append(_check("weapon was tested", cat, False,
                                 "the pass never reached it", subject=name))
            continue
        checks.append(_check("held by the scripted hand", cat, hold.get("held") == "1",
                             hold["_text"], subject=name))
        delivery = (finished or {}).get("delivery", "")
        n_shots = int(_f((finished or {}).get("shots"), len(shots)))
        # Five single pulls -- or the whole magazine when it holds fewer (an
        # RPG has one rocket; the pass never presses reload).
        magazine = int(_f((finished or {}).get("magazine"), 0))
        need = 1 if delivery in ("beam", "drawn") else (min(5, magazine) if magazine > 0 else 5)
        checks.append(_check("fires", cat, n_shots >= need,
                             f"{n_shots} shot(s), want >= {need}", subject=name,
                             evidence=[e["_text"] for e in shots[:3]]))
        errs = [_f(e.get("origin_err_cm")) for e in shots if "origin_err_cm" in e]
        if errs:
            worst = max(errs)
            checks.append(_check("shots leave the muzzle", cat,
                                 worst <= SHOT_ORIGIN_TOLERANCE_CM,
                                 f"worst origin error {worst:.2f} cm "
                                 f"(tolerance {SHOT_ORIGIN_TOLERANCE_CM} cm)", subject=name))
        has_target = hold.get("target", "none") != "none"
        # Hits on the TARGET: a prop standing in the line of fire takes every
        # shot and is not what the weapon was aimed at. A beam's continuity
        # counts any damageable entity it plays over (drones cross the line).
        entity_hits = hits
        if has_target:
            hits = [e for e in hits if e.get("target") == hold.get("target")]
        if not has_target:
            checks.append(_check("hits a target", cat, None,
                                 "no hostile or NPC template to aim at", subject=name))
        else:
            checks.append(_check("hits a target", cat, bool(hits),
                                 f"{len(hits)} hit(s) on target {hold.get('target')}",
                                 subject=name, evidence=[e["_text"] for e in hits[:3]]))
            health = (finished or {}).get("target_health", "")
            before, _, after = health.partition("->")
            dealt = sum(_f(e.get("damage")) for e in hits)
            killed = any(e.get("killed") == "1" for e in hits)
            ok = killed or dealt > 0.0 or (_f(after, _f(before)) < _f(before))
            checks.append(_check("damage lands through ctx.combat", cat, ok,
                                 f"target health {health or '?'}, {dealt:.0f} damage reported"
                                 + (", killed" if killed else ""), subject=name))
            if delivery == "beam":
                ticks = [e for e in entity_hits if _f(e.get("damage")) > 0.0]
                checks.append(_check("the beam damages continuously", cat,
                                     len(ticks) >= BEAM_MIN_HITS,
                                     f"{len(ticks)} damaging tick(s) in 2 s "
                                     f"(want >= {BEAM_MIN_HITS})", subject=name))
        if finished is not None:
            live = int(_f(finished.get("max_live")))
            checks.append(_check("projectile pool stays inside its cap", cat, live <= cap,
                                 f"max {live} live, cap {cap}", subject=name))
            # Frame-to-frame timing jitters around the display period even at
            # a steady 72 Hz, so the p95 gets headroom; what must not happen
            # is a missed vsync (a frame over 1.5 periods).
            p95 = _f(finished.get("p95_ms"))
            frames = max(1, int(_f(finished.get("frames"), 1)))
            dropped = int(_f(finished.get("dropped"), 0))
            median = _f(finished.get("median_ms"), frame_budget_ms)
            ok: Optional[bool] = (p95 <= frame_budget_ms * (1.0 + FRAME_JITTER)
                                  and dropped <= max(1, int(frames * MAX_DROPPED_FRAMES)))
            detail = (f"p95 {p95:.2f} ms (budget {frame_budget_ms:.1f} ms at "
                      f"{fps:.0f} Hz), {dropped} dropped of {frames} frames")
            if median < 0.5 * frame_budget_ms:
                # Frames far shorter than a display period: the compositor was
                # not pacing the app (headset asleep / not presenting).
                ok, detail = None, f"not presenting (median frame {median:.2f} ms): " + detail
            checks.append(_check("frame rate holds under sustained fire", cat, ok, detail,
                                 subject=name))
    return checks


# ---------------------------------------------------------------------------
# Interactions pass (debug.ptv.verify_interactions): a scripted player settles,
# walks into, grabs, presses and throws everything -- see
# engine/runtime/src/core/InteractionVerifier.h.
# ---------------------------------------------------------------------------

#: A loose prop left alone for three seconds moves less than this.
SETTLE_TOLERANCE_CM = 3.0
#: Walking into a prop the body only passes through moves it less than this.
WALK_TOLERANCE_CM = 1.0
#: An open hand reaching for an item knocks it less than this.
REACH_TOLERANCE_CM = 3.0
#: Props resting this high are out of the legs' way; lower ones meet the
#: shins, which may push them as a real leg would.
WALK_BODY_HEIGHT_M = 0.55
INTERACT_FIXED_S = 30.0


def _bound_ids(e: dict) -> list[str]:
    return [str(b.get("builtin_id") or b.get("id")) for b in e.get("mechanic_bindings") or []
            if isinstance(b, dict)]


def scene_interactives(scene: dict) -> dict[str, list[dict]]:
    """What the pass exercises, the way the engine collects it."""
    out: dict[str, list[dict]] = {"props": [], "buttons": [], "challenges": [],
                                  "explosives": []}
    for e in scene.get("entities") or []:
        if not isinstance(e, dict) or "template" in (e.get("tags") or []) or e.get("attached_to"):
            continue
        ids = _bound_ids(e)
        kind = (e.get("physics") or {}).get("kind")
        if "builtin.grab" in ids and kind == "dynamic" and len(out["props"]) < 16:
            out["props"].append(e)
        if "builtin.press_button" in ids:
            out["buttons"].append(e)
        if "builtin.target_challenge" in ids:
            out["challenges"].append(e)
        if "builtin.explosive" in ids and len(out["explosives"]) < 2:
            out["explosives"].append(e)
    return out


def interactions_wanted(scene: dict) -> bool:
    """Anything to interact with or look at -- and every game with player
    hands, whose ``self_view`` screenshot photographs them."""
    found = scene_interactives(scene)
    return (any(found.values()) or bool(scene.get("verify_viewpoints"))
            or bool((scene.get("player") or {}).get("hands")))


def interactions_capture_seconds(scene: dict) -> float:
    f = scene_interactives(scene)
    views = min(10, len(scene.get("verify_viewpoints") or [])) + 1  # + self_view
    return min(420.0, INTERACT_FIXED_S + 4.6 * len(f["props"]) + 2.8 * len(f["buttons"])
               + 9.5 * len(f["challenges"]) + 9.0 * len(f["explosives"]) + 4.2 * views)


def parse_interactions(text: str) -> list[dict[str, str]]:
    out: list[dict[str, str]] = []
    for raw in text.splitlines():
        if "interact:" not in raw:
            continue
        body = raw.split("interact:", 1)[1].strip()
        kv = _kv(body)
        kv["_event"] = body.split(" ", 1)[0]
        kv["_text"] = body
        out.append(kv)
    return out


def evaluate_interactions(events: list[dict[str, str]], scene: dict) -> list[VerificationCheck]:
    """Every loose prop settles and is never shoved by the body; every
    grabbable can be reached for without knocking it and then held; every
    button presses; a button that starts a challenge starts it; every
    explosive goes off."""
    cat = "interactions"
    found = scene_interactives(scene)
    if not any(found.values()) and not scene.get("verify_viewpoints"):
        return []
    begun = [e for e in events if e["_event"] == "begin"]
    done = [e for e in events if e["_event"] == "done"]
    checks = [_check("interactions pass ran to the end", cat, bool(begun and done),
                     "" if done else "no 'interact: done' line (crash, or the capture ended early)",
                     evidence=[e["_text"] for e in (begun + done)[:2]])]
    if not begun:
        return checks

    def by(event: str, name: str) -> Optional[dict]:
        return next((e for e in events if e["_event"] == event and e.get("e") == name), None)

    for p in found["props"]:
        name = str(p.get("id"))
        settle = by("settle", name)
        if settle is not None:
            moved = _f(settle.get("moved_cm"))
            checks.append(_check("stays put when left alone", cat, moved <= SETTLE_TOLERANCE_CM,
                                 f"moved {moved:.2f} cm in 3 s (tolerance "
                                 f"{SETTLE_TOLERANCE_CM} cm)", subject=name))
        walk = by("walk", name)
        if walk is not None:
            moved = _f(walk.get("moved_cm"))
            high = _f(walk.get("height_m")) >= WALK_BODY_HEIGHT_M
            checks.append(_check(
                "not pushed by the player's body", cat,
                (moved <= WALK_TOLERANCE_CM) if high else None,
                f"moved {moved:.2f} cm when the player walked into it"
                + ("" if high else f" (rests below {WALK_BODY_HEIGHT_M} m: legs may push it)"),
                subject=name))
        grab = by("grab", name)
        if grab is None:
            checks.append(_check("can be picked up", cat, False, "the pass never reached it",
                                 subject=name))
            continue
        pushed = _f(grab.get("pushed_cm"))
        checks.append(_check("not knocked away by a reaching hand", cat,
                             pushed <= REACH_TOLERANCE_CM,
                             f"moved {pushed:.2f} cm while the open hand approached",
                             subject=name))
        checks.append(_check("can be picked up", cat, grab.get("held") == "1",
                             grab["_text"], subject=name))
    pressed: set[str] = set()
    for b in found["buttons"]:
        name = str(b.get("id"))
        ev = by("press", name)
        how = (ev or {}).get("by", "none")
        if how != "none":
            pressed.add(str((ev or {}).get("signal", "")))
        checks.append(_check("presses", cat, ev is not None and how != "none",
                             (ev or {}).get("_text", "the pass never reached it"), subject=name))
        if ev is not None and how == "trigger":
            checks.append(_check("presses with a fingertip poke", cat, None,
                                 "pressed by the trigger fallback; the scripted poke did not "
                                 "register (fingertip not posed on this headset run)",
                                 subject=name))
    for c in found["challenges"]:
        params = next((b.get("params") or {} for b in c.get("mechanic_bindings") or []
                       if (b.get("builtin_id") or b.get("id")) == "builtin.target_challenge"), {})
        cid = str(params.get("id") or c.get("id"))
        ev = next((e for e in events if e["_event"] == "challenge" and e.get("id") == cid), None)
        if ev is None:
            checks.append(_check("challenge starts", cat, False, "the pass never reached it",
                                 subject=cid))
            continue
        if ev.get("pressed") != "1":
            checks.append(_check("challenge starts", cat, None,
                                 f"its start signal was not pressed: {ev['_text']}", subject=cid))
            continue
        running = ev.get("state") in ("running", "countdown", "finished")
        checks.append(_check("challenge starts", cat, running, ev["_text"], subject=cid))
        if running:
            checks.append(_check("challenge spawns targets", cat, int(_f(ev.get("active"))) > 0,
                                 f"{ev.get('active', '0')} live target(s)", subject=cid))
            checks.append(_check("challenge clock runs", cat, _f(ev.get("time_left_drop")) > 0.5,
                                 f"time_left dropped {ev.get('time_left_drop', '0')} s in 1.5 s",
                                 subject=cid))
    for x in found["explosives"]:
        name = str(x.get("id"))
        ev = by("explosive", name)
        if ev is None:
            checks.append(_check("explodes", cat, False, "the pass never reached it",
                                 subject=name))
            continue
        checks.append(_check("can be picked up", cat, ev.get("held") == "1", ev["_text"],
                             subject=name))
        checks.append(_check("explodes when armed and thrown", cat, ev.get("detonated") == "1",
                             ev["_text"], subject=name))
    return checks


def interaction_requirement_results(events: list[dict[str, str]], scene: dict, ledger: Any,
                                    shots: list[dict[str, Any]]) -> dict[str, tuple[str, list[str]]]:
    """requirement id -> (outcome, evidence) for interaction / mechanic
    requirements whose entities this pass exercised, and the viewpoint
    screenshots taken for the rest."""
    from .stage_scene_review import entities_for

    out: dict[str, tuple[str, list[str]]] = {}
    if ledger is None:
        return out
    ok_by_entity: dict[str, tuple[bool, str]] = {}
    for e in events:
        name = e.get("e")
        if not name:
            continue
        ev = e["_event"]
        if ev == "grab":
            ok_by_entity[name] = (e.get("held") == "1", e["_text"])
        elif ev == "press":
            ok_by_entity[name] = (e.get("by", "none") != "none", e["_text"])
        elif ev == "explosive":
            ok_by_entity[name] = (e.get("detonated") == "1", e["_text"])
    for r in ledger.requirements:
        if r.kind.value not in ("interaction", "mechanic"):
            continue
        ents = [str(x.get("id")) for x in entities_for(r, scene, ledger)]
        seen = [ok_by_entity[i] for i in ents if i in ok_by_entity]
        if not seen:
            continue
        outcome = "pass" if any(ok for ok, _ in seen) else "fail"
        out[r.id] = (outcome, [f"headset: {t}" for _, t in seen][:6])
    for s in shots:
        rid = str(s.get("viewpoint", "")).split("__", 1)[0]
        if rid and ledger.get(rid) is not None and rid not in out and s.get("file"):
            out[rid] = ("unverified", [f"headset screenshot: {s['file']}"])
    return out


# ---------------------------------------------------------------------------
# Stage
# ---------------------------------------------------------------------------

def _record_requirements(ctx: PipelineContext, events: list[dict[str, str]], scene: dict,
                         shots: list[dict[str, Any]], checks: list[VerificationCheck]) -> None:
    """Fold what the headset showed into the requirements ledger and write
    requirements_report.json (pass / fail / unverified per requirement)."""
    from ptv_schemas.requirements import RequirementOutcome

    from . import requirements_ledger as rl

    ledger = rl.load(ctx.project_dir)
    if ledger is None:
        return
    rl.clear_results(ledger, "quest_runtime_verify")
    for rid, (outcome, evidence) in interaction_requirement_results(
            events, scene, ledger, shots).items():
        rl.record_result(ledger, rid, RequirementOutcome(outcome), evidence,
                         "quest_runtime_verify")
    rl.save(ctx.project_dir, ledger)
    rl.write_report(ctx.project_dir, ledger)


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
        if "memory" not in extra:
            extra["memory"] = _estimated_memory(ctx)
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
    design = ctx.game_design or ctx.store.load_game_design(ctx.project_id)
    flow_log_file = log_file.with_name(log_file.stem + "_flow.log")
    flow_text = ""
    flow_exited = False
    flow_relaunched = False
    player_weapons = scene_weapons(scene)
    weapons_log_file = log_file.with_name(log_file.stem + "_weapons.log")
    weapons_text = ""
    textures_log_file = log_file.with_name(log_file.stem + "_textures.log")
    textures_text = ""
    gallery_shots: list[dict[str, Any]] = []
    interact_log_file = log_file.with_name(log_file.stem + "_interactions.log")
    interact_text = ""
    view_shots: list[dict[str, Any]] = []
    # Memory pass (Quest 3 PSS).
    mem_samples: list[dict[str, Any]] = []
    gpu_mem: dict[str, float] = {}
    lmk_hits: list[str] = []
    soak_samples: list[tuple[float, float]] = []
    soak_log_file = log_file.with_name(log_file.stem + "_soak.log")
    pressure_log_file = log_file.with_name(log_file.stem + "_pressure.log")
    pressure_text = ""
    pressure_alive: Optional[bool] = None
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
        def running() -> bool:
            rc_p, pid_p = adb_s(["shell", "pidof", package])
            return rc_p == 0 and bool(pid_p.strip())

        def exited() -> bool:
            # One failed ``pidof`` is not an exit: a busy host makes adb
            # time out, and reading that as "the game left" cut the game-loop
            # pass off a second after its pause screen opened.
            if running():
                return False
            sleep(2.0)
            return not running()

        def sample_memory(into: Optional[list] = None) -> None:
            rc_m, out_m = adb_s(["shell", "dumpsys", "meminfo", package], 30)
            row = parse_dumpsys_meminfo(out_m) if rc_m == 0 else {}
            if row:
                (mem_samples if into is None else into).append(row)

        def scan_kills() -> None:
            _, all_log = adb_s(["logcat", "-d", "-b", "all"], 90)
            for hit in low_memory_kills(all_log, package):
                if hit not in lmk_hits:
                    lmk_hits.append(hit)

        def start_game() -> bool:
            sleep(LAUNCH_SETTLE_S)
            for attempt in range(LAUNCH_ATTEMPTS):
                if attempt:
                    ctx.emit(stage, "info", f"launch attempt {attempt + 1}: the game "
                             "did not start (Guardian interstitial?); retrying")
                    adb_s(["shell", "am", "broadcast", "-a",
                           "com.oculus.vrpowermanager.prox_close"])
                    adb_s(["shell", "input", "keyevent", "KEYCODE_WAKEUP"])
                    adb_s(["shell", "input", "keyevent", "KEYCODE_BACK"])
                    sleep(LAUNCH_SETTLE_S)
                rc_s, out_s = adb_s(["shell", "am", "start", "-n",
                                     f"{package}/android.app.NativeActivity"])
                if rc_s != 0 or "Error" in out_s:
                    raise RuntimeError(f"could not launch {package}: {out_s.strip()[:300]}")
                for _ in range(LAUNCH_POLL_S):
                    sleep(1.0)
                    if running():
                        return True
            return False

        launched = start_game()
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
            sample_memory()
            if not gpu_mem and waited >= capture / 2:
                rc_pid, pid = adb_s(["shell", "pidof", package])
                if rc_pid == 0 and pid.strip():
                    gpu_mem.update(parse_gpumeminfo(adb_s(
                        ["shell", "gpumeminfo", "-p", pid.strip().split()[0], "-m", "-o"],
                        30)[1]))
        alive = running()
        scan_kills()
        if stop_stream is not None:
            log_text = stop_stream()
            stop_stream = None
        else:
            _, log_text = adb_s(["logcat", "-d", "-v", "time", "-s", "ptv:*"], 60)
        _, crash_text = adb_s(["logcat", "-b", "crash", "-d"], 30)

        # --- Second pass: the game loop -------------------------------
        # The player can die, the engine clicks the end-screen buttons, and
        # the game is expected to END ITSELF through Leave Game -- then it
        # must launch again (a Quest app that returned from android_main
        # without finishing its activity does not).
        if design is not None:
            adb_s(["shell", "am", "force-stop", package])
            adb_s(["shell", "setprop", "debug.ptv.verify_flow", "1"])
            adb_s(["logcat", "-c"])
            if streamer is not None:
                stop_stream = streamer(["-s", serial, "logcat", "-v", "time", "-s", "ptv:*"],
                                       flow_log_file)
            flow_budget = flow_capture_seconds(design)
            ctx.emit(stage, "info", f"game-loop pass: up to {flow_budget:.0f} s")
            if start_game():
                waited = 0.0
                while waited < flow_budget:
                    ctx.raise_if_cancelled()
                    sleep(5.0)
                    waited += 5.0
                    if not still_there():
                        raise DeviceGone("the headset disconnected during the game-loop pass")
                    if exited():
                        flow_exited = True
                        break
            if stop_stream is not None:
                flow_text = stop_stream()
                stop_stream = None
            else:
                _, flow_text = adb_s(["logcat", "-d", "-v", "time", "-s", "ptv:*"], 60)
            scan_kills()
            adb_s(["shell", "setprop", "debug.ptv.verify_flow", "0"])
            if flow_exited:
                adb_s(["shell", "setprop", "debug.ptv.verify", "0"])
                flow_relaunched = start_game()
                adb_s(["shell", "am", "force-stop", package])

        # --- Third pass: the weapons -----------------------------------
        # A scripted hand holds each player weapon, aims at a hostile and
        # fires; the engine logs where every shot left and what it hit.
        if player_weapons:
            adb_s(["shell", "am", "force-stop", package])
            adb_s(["shell", "setprop", "debug.ptv.verify", "1"])
            adb_s(["shell", "setprop", "debug.ptv.verify_flow", "0"])
            adb_s(["shell", "setprop", "debug.ptv.verify_weapons", "1"])
            adb_s(["logcat", "-c"])
            if streamer is not None:
                stop_stream = streamer(["-s", serial, "logcat", "-v", "time", "-s", "ptv:*"],
                                       weapons_log_file)
            budget = weapons_capture_seconds(len(player_weapons))
            ctx.emit(stage, "info", f"weapons pass: {len(player_weapons)} weapon(s), "
                     f"up to {budget:.0f} s")
            if start_game():
                waited = 0.0
                while waited < budget:
                    ctx.raise_if_cancelled()
                    sleep(5.0)
                    waited += 5.0
                    if not still_there():
                        raise DeviceGone("the headset disconnected during the weapons pass")
                    if exited():
                        break
                    try:
                        if "weapons: done" in weapons_log_file.read_text(
                                encoding="utf-8", errors="replace"):
                            break
                    except OSError:
                        pass
            if stop_stream is not None:
                weapons_text = stop_stream()
                stop_stream = None
            else:
                _, weapons_text = adb_s(["logcat", "-d", "-v", "time", "-s", "ptv:*"], 60)
            scan_kills()
            adb_s(["shell", "setprop", "debug.ptv.verify_weapons", "0"])
            adb_s(["shell", "am", "force-stop", package])

        # --- Fourth pass: the texture gallery -------------------------
        # The engine shows each textured asset in front of the player in
        # turn; a Quest screenshot of each (plus one of the empty background)
        # is compared with an offline render of the shipped LOD.
        if texture_gallery_wanted(scene):
            shot_dir = log_dir / f"textures_{log_file.stem}"
            shot_dir.mkdir(parents=True, exist_ok=True)
            adb_s(["shell", "am", "force-stop", package])
            adb_s(["shell", "setprop", "debug.ptv.verify", "1"])
            adb_s(["shell", "setprop", "debug.ptv.verify_flow", "0"])
            adb_s(["shell", "setprop", "debug.ptv.verify_weapons", "0"])
            adb_s(["shell", "setprop", "debug.ptv.verify_textures", "1"])
            adb_s(["logcat", "-c"])
            if streamer is not None:
                stop_stream = streamer(["-s", serial, "logcat", "-v", "time", "-s", "ptv:*"],
                                       textures_log_file)

            def take_shot(name: str) -> Optional[Path]:
                before = set(adb_s(["shell", "ls", "/sdcard/Oculus/Screenshots/"])[1].split())
                adb_s(["shell", "am", "startservice", "-n",
                       "com.oculus.metacam/.capture.CaptureService", "-a", "TAKE_SCREENSHOT"])
                for _ in range(12):
                    sleep(0.75)
                    after = adb_s(["shell", "ls", "/sdcard/Oculus/Screenshots/"])[1].split()
                    fresh = [f for f in after if f not in before and package in f]
                    if fresh:
                        local = shot_dir / f"{name}.jpg"
                        adb_s(["pull", f"/sdcard/Oculus/Screenshots/{sorted(fresh)[-1]}",
                               str(local)], 60)
                        return local if local.exists() else None
                return None

            if start_game():
                budget = gallery_capture_seconds(scene)
                ctx.emit(stage, "info", f"texture gallery pass: up to {budget:.0f} s")
                waited, bg_taken, seen = 0.0, False, set()
                while waited < budget:
                    ctx.raise_if_cancelled()
                    sleep(1.0)
                    waited += 1.0
                    try:
                        text = textures_log_file.read_text(encoding="utf-8", errors="replace")
                    except OSError:
                        text = ""
                    if not bg_taken and "texgallery: begin" in text:
                        sleep(2.0)
                        waited += 2.0
                        # The system shows an indicator once a screenshot has
                        # been taken; the second background includes it, so
                        # it is not mistaken for part of an asset.
                        first = take_shot("background_first")
                        sleep(2.0)  # back-to-back captures are dropped
                        waited += 2.0
                        bg = take_shot("background") or first
                        gallery_shots.append({"name": "background", "file": bg})
                        if first and first != bg:
                            gallery_shots.append({"name": "background_first", "file": first})
                        bg_taken = True
                    for entry in parse_gallery_lines(text):
                        if entry["index"] in seen:
                            continue
                        seen.add(entry["index"])
                        sleep(1.0)
                        waited += 1.0
                        f = take_shot(f"asset_{entry['index']:02d}")
                        gallery_shots.append({"name": f"asset_{entry['index']:02d}",
                                              "file": f, "entry": entry})
                    if "texgallery: done" in text:
                        break
                    if not still_there():
                        raise DeviceGone("the headset disconnected during the texture pass")
            if stop_stream is not None:
                textures_text = stop_stream()
                stop_stream = None
            else:
                _, textures_text = adb_s(["logcat", "-d", "-v", "time", "-s", "ptv:*"], 60)
            scan_kills()
            adb_s(["shell", "setprop", "debug.ptv.verify_textures", "0"])
            adb_s(["shell", "am", "force-stop", package])

        # --- Fifth pass: interactions + requirement viewpoints -----------
        # A scripted player settles, walks into, grabs, presses and throws
        # every interactive thing; at each scene_review viewpoint a Quest
        # screenshot is taken.
        if interactions_wanted(scene):
            view_dir = log_dir / f"views_{log_file.stem}"
            view_dir.mkdir(parents=True, exist_ok=True)
            adb_s(["shell", "am", "force-stop", package])
            adb_s(["shell", "setprop", "debug.ptv.verify", "1"])
            adb_s(["shell", "setprop", "debug.ptv.verify_flow", "0"])
            adb_s(["shell", "setprop", "debug.ptv.verify_weapons", "0"])
            adb_s(["shell", "setprop", "debug.ptv.verify_textures", "0"])
            adb_s(["shell", "setprop", "debug.ptv.verify_interactions", "1"])
            adb_s(["logcat", "-c"])
            if streamer is not None:
                stop_stream = streamer(["-s", serial, "logcat", "-v", "time", "-s", "ptv:*"],
                                       interact_log_file)

            def view_shot(name: str) -> Optional[Path]:
                before = set(adb_s(["shell", "ls", "/sdcard/Oculus/Screenshots/"])[1].split())
                adb_s(["shell", "am", "startservice", "-n",
                       "com.oculus.metacam/.capture.CaptureService", "-a", "TAKE_SCREENSHOT"])
                for _ in range(12):
                    sleep(0.75)
                    after = adb_s(["shell", "ls", "/sdcard/Oculus/Screenshots/"])[1].split()
                    fresh = [f for f in after if f not in before and package in f]
                    if fresh:
                        local = view_dir / f"{name}.jpg"
                        adb_s(["pull", f"/sdcard/Oculus/Screenshots/{sorted(fresh)[-1]}",
                               str(local)], 60)
                        return local if local.exists() else None
                return None

            if start_game():
                budget = interactions_capture_seconds(scene)
                ctx.emit(stage, "info", f"interactions pass: up to {budget:.0f} s")
                waited, shot_ids = 0.0, set()
                while waited < budget:
                    ctx.raise_if_cancelled()
                    sleep(1.0)
                    waited += 1.0
                    try:
                        text = interact_log_file.read_text(encoding="utf-8", errors="replace")
                    except OSError:
                        text = ""
                    for ev in parse_interactions(text):
                        if ev["_event"] != "viewpoint" or ev.get("id") in shot_ids:
                            continue
                        shot_ids.add(ev.get("id"))
                        f = view_shot("self_view" if ev.get("id") == "self_view"
                                      else f"view_{len(shot_ids):02d}")
                        view_shots.append({"viewpoint": ev.get("id"), "file": str(f) if f else ""})
                    if "interact: done" in text:
                        break
                    if not still_there():
                        raise DeviceGone("the headset disconnected during the interactions pass")
                    if exited():
                        break
            if stop_stream is not None:
                interact_text = stop_stream()
                stop_stream = None
            else:
                _, interact_text = adb_s(["logcat", "-d", "-v", "time", "-s", "ptv:*"], 60)
            scan_kills()
            adb_s(["shell", "setprop", "debug.ptv.verify_interactions", "0"])
            adb_s(["shell", "am", "force-stop", package])

        # --- Memory soak: the scripted walk for minutes, PSS every 15 s --
        soak_s = float(getattr(ctx.settings, "quest_memory_soak_seconds", 0.0) or 0.0)
        if soak_s > 0:
            adb_s(["shell", "am", "force-stop", package])
            adb_s(["shell", "setprop", "debug.ptv.verify", "1"])
            adb_s(["logcat", "-c"])
            if streamer is not None:
                stop_stream = streamer(["-s", serial, "logcat", "-v", "time", "-s", "ptv:*"],
                                       soak_log_file)
            ctx.emit(stage, "info", f"memory soak: {soak_s:.0f} s")
            if start_game():
                waited = 0.0
                while waited < soak_s:
                    ctx.raise_if_cancelled()
                    sleep(15.0)
                    waited += 15.0
                    rows: list[dict[str, Any]] = []
                    sample_memory(rows)
                    if rows and rows[0].get("total_pss_kb"):
                        soak_samples.append((waited, rows[0]["total_pss_kb"] / 1024.0))
                    if not still_there():
                        raise DeviceGone("the headset disconnected during the memory soak")
                    if not running():
                        break
            if stop_stream is not None:
                stop_stream()
                stop_stream = None
            scan_kills()
            adb_s(["shell", "am", "force-stop", package])

        # --- Memory pressure: MemoryGuard's thresholds lowered mid-game ---
        if getattr(ctx.settings, "quest_memory_pressure_test", True):
            adb_s(["shell", "am", "force-stop", package])
            adb_s(["shell", "setprop", "debug.ptv.verify", "1"])
            adb_s(["shell", "setprop", "debug.ptv.simulate_memory_pressure", "1"])
            adb_s(["logcat", "-c"])
            if streamer is not None:
                stop_stream = streamer(["-s", serial, "logcat", "-v", "time", "-s", "ptv:*"],
                                       pressure_log_file)
            # Thresholds scaled so red sits at half of what this game measured,
            # whatever its size (a fixed 5% left a 200 MB game green).
            pss_now = max([s.get("total_pss_kb", 0) / 1024.0 for s in mem_samples] or [200.0])
            scale = min(0.5, max(0.001, 0.5 * pss_now / 4608.0))
            ctx.emit(stage, "info", f"memory pressure pass: thresholds to {scale:.1%} "
                                    f"(red at ~{4608 * scale:.0f} MB) for 20 s")
            if start_game():
                sleep(12.0)
                adb_s(["shell", "setprop", "debug.ptv.simulate_memory_pressure", f"{scale:.4f}"])
                sleep(20.0)
                adb_s(["shell", "setprop", "debug.ptv.simulate_memory_pressure", "1"])
                sleep(20.0)
                pressure_alive = running()
            if stop_stream is not None:
                pressure_text = stop_stream()
                stop_stream = None
            else:
                _, pressure_text = adb_s(["logcat", "-d", "-v", "time", "-s", "ptv:*"], 60)
            adb_s(["shell", "setprop", "debug.ptv.simulate_memory_pressure", "1"])
            scan_kills()
            adb_s(["shell", "am", "force-stop", package])
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
                     ["shell", "setprop", "debug.ptv.verify_flow", "0"],
                     ["shell", "setprop", "debug.ptv.verify_weapons", "0"],
                     ["shell", "setprop", "debug.ptv.verify_textures", "0"],
                     ["shell", "setprop", "debug.ptv.verify_interactions", "0"],
                     ["shell", "setprop", "debug.ptv.simulate_memory_pressure", "1"],
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
    checks.extend(evaluate_hands(log_text, scene))
    checks.extend(evaluate_body_draws(log_text, scene))
    checks.extend(evaluate_textures(log_text, scene))
    checks.extend(evaluate_locks(log_text, scene))
    if "Renderer ready" in log_text:
        checks.extend(evaluate_render_health(log_text))
    if design is not None:
        if flow_text and not (flow_log_file.exists() and flow_log_file.stat().st_size):
            flow_log_file.write_text(flow_text, encoding="utf-8")
        checks.extend(evaluate_flow(parse_flow(flow_text), design,
                                    exited=flow_exited, relaunched=flow_relaunched))
    if player_weapons:
        if weapons_text and not (weapons_log_file.exists() and weapons_log_file.stat().st_size):
            weapons_log_file.write_text(weapons_text, encoding="utf-8")
        checks.extend(evaluate_weapons(parse_weapons(weapons_text), scene))
    if texture_gallery_wanted(scene):
        if textures_text and not (textures_log_file.exists() and textures_log_file.stat().st_size):
            textures_log_file.write_text(textures_text, encoding="utf-8")
        checks.extend(evaluate_texture_gallery(ctx, gallery_shots, scene))
    interact_events: list[dict[str, str]] = []
    if interactions_wanted(scene):
        if interact_text and not (interact_log_file.exists() and interact_log_file.stat().st_size):
            interact_log_file.write_text(interact_text, encoding="utf-8")
        interact_events = parse_interactions(interact_text)
        checks.extend(evaluate_interactions(interact_events, scene))
        self_view = next((s for s in view_shots if s.get("viewpoint") == "self_view"), None)
        req_views = [s for s in view_shots if s.get("viewpoint") != "self_view"]
        if self_view is not None:
            checks.append(_check(
                "the player's own hands photographed (self_view)", "interactions",
                bool(self_view.get("file")) or None,
                "both scripted hands 35 cm in front of the eyes"
                + ("" if self_view.get("file") else "; the screenshot was not captured"),
                evidence=[self_view["file"]] if self_view.get("file") else []))
        if req_views:
            checks.append(_check(
                "requirement viewpoint screenshots", "interactions",
                any(s.get("file") for s in req_views),
                f"{sum(1 for s in req_views if s.get('file'))} of {len(req_views)} captured",
                evidence=[s["file"] for s in req_views if s.get("file")][:10]))
    _record_requirements(ctx, interact_events, scene, view_shots, checks)
    if pressure_text and not (pressure_log_file.exists() and pressure_log_file.stat().st_size):
        pressure_log_file.write_text(pressure_text, encoding="utf-8")
    compromises = []
    try:
        nrep = json.loads((ctx.project_dir / "asset_normalization.json").read_text(encoding="utf-8"))
        compromises = list((nrep.get("__memory__") or {}).get("compromises") or [])
    except (OSError, ValueError):
        pass
    mem_checks, memory = evaluate_memory(
        samples=mem_samples, gpu=gpu_mem, base_text=log_text, flow_text=flow_text,
        soak=soak_samples, pressure_text=pressure_text, pressure_alive=pressure_alive,
        kills=lmk_hits, scene=scene, compromises=compromises)
    checks.extend(mem_checks)
    _record_memory(ctx, memory)
    failed = [c for c in checks if c.outcome == CheckOutcome.failed]
    # Memory first: a memory failure is the reason the run failed.
    failed.sort(key=lambda c: 0 if c.category == "memory" else 1)
    status = VerificationStatus.failed if failed else VerificationStatus.passed
    reason = (f"{len(failed)} of {len(checks)} check(s) failed: "
              + "; ".join(f"{c.subject + ': ' if c.subject else ''}{c.name}"
                          for c in failed)[:1500]
              if failed else f"all {sum(1 for c in checks if c.outcome == CheckOutcome.passed)} applicable checks passed")
    from .quest_metrics import parse_perf_lines, summarize_perf

    result = RuntimeVerification(
        project_id=ctx.project_id, status=status, reason=reason,
        device_serial=serial, package=package, capture_seconds=capture,
        npc_summary=npc_summary, checks=checks, log_path=log_rel,
        started_at=started, finished_at=_now(),
        performance=summarize_perf(parse_perf_lines(log_text)), memory=memory)
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
