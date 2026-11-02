"""Parsers for what a running Quest build tells us about itself.

Three sources, all text:

* the engine's own 1 Hz ``perf:`` and ``mem:`` logcat lines,
* ``adb shell dumpsys meminfo <package>`` -- Android's PSS breakdown, which is
  the number Horizon OS's low-memory killer compares against Meta's
  per-app limit (5.75 GiB PSS on Quest 3 / 3S, "Memory / RAM"),
* ``adb shell gpumeminfo -p <pid> -m -o`` -- Meta's per-process GPU
  allocation summary (Vulkan device memory included).

Kept free of adb so the verify stage, the baseline script and the unit tests
share one implementation.
"""

from __future__ import annotations

import re
import statistics
from typing import Any, Optional

#: Meta's documented per-app PSS limit for Quest 3 / 3S.
QUEST3_PSS_LIMIT_MB = 5.75 * 1024.0

_PERF_RE = re.compile(
    r"perf:\s*(?P<fps>[\d.]+)\s*fps\s*\|\s*work\s*(?P<work>[\d.]+)\s*ms avg,\s*"
    r"(?P<work_worst>[\d.]+)\s*ms worst"
    r"(?:\s*\|\s*cpu\s*(?P<cpu>[\d.]+)\s*ms)?"
    r"(?:\s*\|\s*gpu\s*(?P<gpu>[\d.]+)\s*ms avg,\s*(?P<gpu_worst>[\d.]+)\s*ms worst)?"
    r"(?:\s*\|\s*calls\s*(?P<calls>\d+)\s*pipes\s*(?P<pipes>\d+)\s*sets\s*(?P<sets>\d+))?"
    r"\s*\|\s*draw\s*(?P<drawn>\d+)\s*of\s*(?P<considered>\d+)"
    r"(?:[^|]*?small\s*(?P<small>\d+)\s*room\s*(?P<room>\d+),\s*batches\s*(?P<batches>\d+))?"
    r"(?:.*?\|\s*tris\s*(?P<tris>\d+))?"
    r"(?:.*?ffr\s*(?P<ffr>-?\d+))?"
    r"(?:\s*scale\s*(?P<scale>[\d.]+))?"
)

_MEM_RE = re.compile(r"mem:\s*pss\s*(?P<pss>\d+)\s*MB")


def parse_perf_lines(text: str) -> list[dict[str, float]]:
    """Every ``perf:`` line in ``text`` as numbers (missing fields omitted)."""
    out: list[dict[str, float]] = []
    for line in text.splitlines():
        m = _PERF_RE.search(line)
        if not m:
            continue
        row = {k: float(v) for k, v in m.groupdict().items() if v is not None}
        out.append(row)
    return out


def summarize_perf(rows: list[dict[str, float]], skip_first: int = 3) -> dict[str, float]:
    """Medians of the steady-state rows (the first few cover loading)."""
    steady = rows[skip_first:] if len(rows) > skip_first + 2 else rows
    if not steady:
        return {}
    summary: dict[str, float] = {"samples": float(len(steady))}
    for key in ("fps", "work", "work_worst", "cpu", "gpu", "gpu_worst", "calls",
                "pipes", "sets", "drawn", "considered", "small", "room", "batches",
                "tris", "ffr", "scale"):
        vals = [r[key] for r in steady if key in r]
        if vals:
            summary[key] = round(statistics.median(vals), 2)
            if key in ("work", "cpu", "gpu"):
                summary[f"{key}_p95"] = round(_percentile(vals, 0.95), 2)
    return summary


def _percentile(values: list[float], q: float) -> float:
    if not values:
        return 0.0
    v = sorted(values)
    idx = min(len(v) - 1, max(0, int(round(q * (len(v) - 1)))))
    return v[idx]


def _kb(text: str, label: str) -> Optional[int]:
    """First integer after ``label`` on a line that starts with it."""
    pat = re.compile(rf"^\s*{label}\s+(\d+)", re.MULTILINE)
    m = pat.search(text)
    return int(m.group(1)) if m else None


def parse_dumpsys_meminfo(text: str) -> dict[str, Any]:
    """PSS breakdown (KB) from ``dumpsys meminfo <package>``.

    Returns an empty dict when the process was not running (dumpsys prints
    "No process found"), so callers can tell "no sample" from "zero".
    """
    if not text or "No process found" in text or "MEMINFO" not in text:
        return {}
    out: dict[str, Any] = {}
    m = re.search(r"TOTAL PSS:\s*(\d+)", text)
    if m:
        out["total_pss_kb"] = int(m.group(1))
    else:
        total = _kb(text, "TOTAL")
        if total is not None:
            out["total_pss_kb"] = total
    m = re.search(r"TOTAL RSS:\s*(\d+)", text)
    if m:
        out["total_rss_kb"] = int(m.group(1))
    for key, label in (("native_heap_kb", "Native Heap"),
                       ("gl_mtrack_kb", "GL mtrack"),
                       ("egl_mtrack_kb", "EGL mtrack"),
                       ("other_mtrack_kb", "Other mtrack"),
                       ("unknown_kb", "Unknown")):
        v = _kb(text, label)
        if v is not None:
            out[key] = v
    m = re.search(r"^\s*Graphics:\s*(\d+)", text, re.MULTILINE)
    if m:
        out["graphics_kb"] = int(m.group(1))
    return out


def parse_gpumeminfo(text: str) -> dict[str, float]:
    """Per-type GPU memory (MB) from ``gpumeminfo -p <pid> -m -o``.

    The machine-readable format (Quest 3, Horizon OS v7x) is one CSV row per
    allocation type -- ``vk_devicememory,<total bytes>,<mapped bytes>,<count>``
    -- plus ``total,<bytes>`` and ``mapped,<bytes>``. Returns MB per type,
    with ``total_mb`` / ``mapped_mb`` for the summary rows.
    """
    out: dict[str, float] = {}
    for line in text.splitlines():
        parts = [p.strip() for p in line.split(",")]
        if len(parts) < 2 or not parts[1].isdigit():
            continue
        key = parts[0].lower()
        mb = round(int(parts[1]) / (1024.0 * 1024.0), 2)
        if key in ("total", "mapped"):
            out[f"{key}_mb"] = mb
        else:
            out[f"{key}_mb"] = mb
    return out


def parse_mem_lines(text: str) -> list[int]:
    """PSS (MB) from every engine ``mem:`` line, in order."""
    return [int(m.group("pss")) for m in _MEM_RE.finditer(text)]


def low_memory_kills(logcat_text: str, package: str) -> list[str]:
    """Lines in which Android's low-memory killer names ``package``."""
    hits = []
    for line in logcat_text.splitlines():
        low = line.lower()
        if package.lower() not in low:
            continue
        if ("lowmemorykiller" in low or "lmkd" in low or "low memory kill" in low
                or "kill one process" in low):
            hits.append(line.strip())
    return hits
