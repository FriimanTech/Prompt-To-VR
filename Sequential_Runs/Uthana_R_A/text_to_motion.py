#!/usr/bin/env python3
"""Standalone Uthana text-to-motion (2.0 sync or 3.0 async job).

Edit the USER CONFIGURATION section, then run this file.
Prints motion_id and character_id — pass both into
download_retarget_motion.py to download the animated GLB.

When MODEL is "text-to-motion-3.0", only the TTM 3.0 knobs are sent
(create_job + poll). When MODEL is "text-to-motion-2.0", the advanced
2.0 knobs supported by uthana>=0.5.1 are sent via ttm.create.
"""

from __future__ import annotations

import asyncio
import sys
from pathlib import Path
from typing import Any

_SCRIPT_DIR = Path(__file__).resolve().parent
if str(_SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(_SCRIPT_DIR))

from uthana_common import fail, make_client  # noqa: E402

# ============================================================
# USER CONFIGURATION
# ============================================================
#
# API key is loaded from UTHANA_API_KEY in the repo-root .env
# (or the process environment). Do not paste the key here.

PROMPT = "a person waving hello"

# "text-to-motion-3.0" (default, async job) or "text-to-motion-2.0" (sync).
MODEL = "text-to-motion-3.0"

# Optional. Leave None to use Uthana's default character (Tar).
CHARACTER_ID: str | None = None

# --- text-to-motion-3.0 only (ignored when MODEL is 2.0) ---
# Target length in seconds, rounded to an integer between 4–10. Default 8.
LENGTH_3 = 8
# Rewrite the prompt into physical motion direction. Default True.
REWRITE_PROMPT = True
# When True, use the faster TTM 3.0 variant. Default False = standard 3.0.
FAST = False

# --- text-to-motion-2.0 only (ignored when MODEL is 3.0) ---
FOOT_IK = False
# Website API docs list steps (1–150); uthana 0.5.1 client omits it — not sent.
STEPS = 50
CFG_SCALE = 2.0  # 0–10, default 2
LENGTH_2 = 10.0  # 0.25–10 seconds, default 10
SEED: int | None = None  # 1–99999, or None for random
INTERNAL_IK = True

# Polling for TTM 3.0 jobs.
POLL_INTERVAL_S = 5.0
POLL_TIMEOUT_S = 600.0

# ============================================================
# IMPLEMENTATION
# ============================================================

_MODEL_3 = "text-to-motion-3.0"
_MODEL_2 = "text-to-motion-2.0"


def _job_get(job: Any, key: str) -> Any:
    """Read a field from a dict-like Job (uthana README uses job['id'])."""
    if isinstance(job, dict):
        return job.get(key)
    try:
        return job[key]
    except (KeyError, TypeError, IndexError):
        pass
    return getattr(job, key, None)


def _extract_motion_id(job: Any) -> str | None:
    result = _job_get(job, "result")
    if isinstance(result, dict):
        nested = result.get("result")
        if isinstance(nested, dict) and nested.get("id"):
            return nested["id"]
        if result.get("id"):
            return result["id"]
    return None


async def _run_ttm_3(client: Any) -> tuple[str, str | None]:
    kwargs: dict[str, Any] = {
        "model": _MODEL_3,
        "length": int(LENGTH_3),
        "rewrite_prompt": REWRITE_PROMPT,
        "fast": FAST,
    }
    if CHARACTER_ID:
        kwargs["character_id"] = CHARACTER_ID

    print(
        f"Creating TTM 3.0 job (prompt={PROMPT!r}, length={LENGTH_3}, "
        f"rewrite_prompt={REWRITE_PROMPT}, fast={FAST})..."
    )
    job = await client.ttm.create_job(PROMPT, **kwargs)
    job_id = _job_get(job, "id")
    if not job_id:
        fail(f"No job id in create_job response: {job!r}")

    elapsed = 0.0
    while _job_get(job, "status") not in ("FINISHED", "FAILED"):
        if elapsed >= POLL_TIMEOUT_S:
            fail(f"TTM 3.0 job {job_id} timed out after {POLL_TIMEOUT_S}s.")
        await asyncio.sleep(POLL_INTERVAL_S)
        elapsed += POLL_INTERVAL_S
        job = await client.jobs.get(job_id)
        print(f"Job status: {_job_get(job, 'status')}")

    if _job_get(job, "status") == "FAILED":
        fail(f"TTM 3.0 job failed: {job!r}")

    motion_id = _extract_motion_id(job)
    if not motion_id:
        fail(f"No motion id in finished job: {job!r}")
    return motion_id, CHARACTER_ID


async def _run_ttm_2(client: Any) -> tuple[str, str | None]:
    # uthana 0.5.1 ttm.create accepts these kwargs only (no steps).
    kwargs: dict[str, Any] = {
        "model": _MODEL_2,
        "foot_ik": FOOT_IK,
        "cfg_scale": CFG_SCALE,
        "length": LENGTH_2,
        "internal_ik": INTERNAL_IK,
    }
    if CHARACTER_ID:
        kwargs["character_id"] = CHARACTER_ID
    if SEED is not None:
        kwargs["seed"] = SEED

    print(
        f"Creating TTM 2.0 motion (prompt={PROMPT!r}, length={LENGTH_2}, "
        f"cfg_scale={CFG_SCALE}; STEPS={STEPS} not sent — client omits it)..."
    )
    result = await client.ttm.create(PROMPT, **kwargs)
    motion_id = getattr(result, "motion_id", None)
    character_id = getattr(result, "character_id", None)
    if isinstance(result, dict):
        motion_id = motion_id or result.get("motion_id")
        character_id = character_id or result.get("character_id")
    if not motion_id:
        fail(f"No motion_id in ttm.create response: {result!r}")
    return motion_id, character_id or CHARACTER_ID


async def main() -> int:
    if not PROMPT.strip():
        fail("Set PROMPT to a non-empty motion description.")

    model = MODEL.strip()
    if model not in (_MODEL_3, _MODEL_2):
        fail(
            f"MODEL must be {_MODEL_3!r} or {_MODEL_2!r}, got {MODEL!r}."
        )

    client = make_client()
    if model == _MODEL_3:
        motion_id, character_id = await _run_ttm_3(client)
    else:
        motion_id, character_id = await _run_ttm_2(client)

    print(f"motion_id={motion_id}")
    print(f"character_id={character_id}")
    print("Copy both into download_retarget_motion.py.")
    return 0


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main()))
