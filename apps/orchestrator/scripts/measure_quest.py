"""Measure a built Quest APK on the connected headset: frame time and memory.

    python apps/orchestrator/scripts/measure_quest.py <project_id> [--label base]
        [--seconds 40] [--no-install]

Installs ``latest_build_apk``, launches it in verification mode (scripted
walk, player cannot die) with the headset held awake, and records:

* the engine's ``perf:`` lines (fps, CPU ms, GPU ms, draw calls, triangles),
* ``dumpsys meminfo`` PSS every 5 s and ``gpumeminfo`` once at steady state.

Writes ``projects/<id>/logs/runtime/measure_<label>_<timestamp>.json`` and
prints the summary. Used for before/after comparisons; the pipeline's own
on-device checks live in ``quest_runtime_verify``.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import shutil
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "apps" / "orchestrator"))

from ptv_orchestrator.pipeline.quest_metrics import (  # noqa: E402
    low_memory_kills, parse_dumpsys_meminfo, parse_gpumeminfo, parse_mem_lines,
    parse_perf_lines, summarize_perf)
from ptv_orchestrator.quest_ids import quest_package_name  # noqa: E402
from ptv_orchestrator.settings import get_settings  # noqa: E402
from ptv_orchestrator.storage import ProjectStore  # noqa: E402


def adb(args: list[str], timeout: float = 60) -> tuple[int, str]:
    exe = shutil.which("adb") or "adb"
    try:
        p = subprocess.run([exe, *args], capture_output=True, text=True,
                           timeout=timeout, encoding="utf-8", errors="replace")
        return p.returncode, (p.stdout or "") + (p.stderr or "")
    except subprocess.TimeoutExpired:
        return 1, "timeout"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("project_id")
    ap.add_argument("--label", default="measure")
    ap.add_argument("--seconds", type=float, default=40.0)
    ap.add_argument("--no-install", action="store_true")
    ap.add_argument("--prop", action="append", default=[],
                    help="extra debug property name=value set before launch")
    a = ap.parse_args()

    settings = get_settings()
    store = ProjectStore(settings.projects_dir)
    state = store.load_state(a.project_id)
    package = quest_package_name(settings.android_package_prefix, a.project_id)
    apk = Path(state.latest_build_apk) if state.latest_build_apk else None

    prev_guardian = adb(["shell", "getprop", "debug.oculus.guardian_pause"])[1].strip() or "0"
    adb(["shell", "setprop", "debug.oculus.guardian_pause", "1"])
    adb(["shell", "am", "broadcast", "-a", "com.oculus.vrpowermanager.prox_close"])
    adb(["shell", "setprop", "debug.ptv.verify", "1"])
    for kv in a.prop:
        k, _, v = kv.partition("=")
        adb(["shell", "setprop", k, v])
    try:
        if not a.no_install:
            if apk is None or not apk.exists():
                print("no APK; run quest_build first")
                return 2
            rc, out = adb(["install", "-r", str(apk)], 600)
            if rc != 0:
                print("install failed:", out[-400:])
                return 1
        adb(["shell", "am", "force-stop", package])
        adb(["logcat", "-c"])
        time.sleep(2.0)
        adb(["shell", "am", "start", "-n", f"{package}/android.app.NativeActivity"])
        mem_samples: list[dict] = []
        gpu: dict = {}
        t0 = time.time()
        pid = ""
        while time.time() - t0 < a.seconds:
            time.sleep(5.0)
            pid = adb(["shell", "pidof", package])[1].strip()
            if not pid:
                continue
            mi = parse_dumpsys_meminfo(adb(["shell", "dumpsys", "meminfo", package])[1])
            if mi:
                mi["t"] = round(time.time() - t0, 1)
                mem_samples.append(mi)
            if not gpu and time.time() - t0 > a.seconds * 0.6:
                gpu = parse_gpumeminfo(adb(["shell", "gpumeminfo", "-p", pid, "-m", "-o"], 30)[1])
        _, log = adb(["logcat", "-d", "-v", "time", "-s", "ptv:*"], 120)
        _, sys_log = adb(["logcat", "-d", "-v", "time"], 120)
        alive = bool(adb(["shell", "pidof", package])[1].strip())
    finally:
        adb(["shell", "am", "force-stop", package])
        adb(["shell", "setprop", "debug.ptv.verify", "0"])
        for kv in a.prop:
            adb(["shell", "setprop", kv.partition("=")[0], "0"])
        adb(["shell", "setprop", "debug.oculus.guardian_pause", prev_guardian])
        adb(["shell", "am", "broadcast", "-a", "com.oculus.vrpowermanager.automation_disable"])

    rows = parse_perf_lines(log)
    pss = [s["total_pss_kb"] / 1024.0 for s in mem_samples if "total_pss_kb" in s]
    result = {
        "project_id": a.project_id,
        "label": a.label,
        "measured_at": dt.datetime.now().isoformat(timespec="seconds"),
        "alive_at_end": alive,
        "perf": summarize_perf(rows),
        "perf_rows": len(rows),
        "memory": {
            "peak_pss_mb": round(max(pss), 1) if pss else None,
            "steady_pss_mb": round(pss[-1], 1) if pss else None,
            "samples": mem_samples,
            "gpumeminfo": gpu,
            "engine_mem_lines": parse_mem_lines(log)[-5:],
            "low_memory_kills": low_memory_kills(sys_log, package),
        },
        "tex_lines": [l for l in log.splitlines() if "tex:" in l][-3:],
    }
    out_dir = REPO / "projects" / a.project_id / "logs" / "runtime"
    out_dir.mkdir(parents=True, exist_ok=True)
    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    (out_dir / f"measure_{a.label}_{stamp}.json").write_text(json.dumps(result, indent=2),
                                                             encoding="utf-8")
    (out_dir / f"measure_{a.label}_{stamp}.log").write_text(log, encoding="utf-8")
    print(json.dumps({k: result[k] for k in ("label", "alive_at_end", "perf")}, indent=2))
    print(json.dumps({k: v for k, v in result["memory"].items() if k != "samples"}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
