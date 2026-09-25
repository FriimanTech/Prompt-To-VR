#!/usr/bin/env python3
"""Standalone Tripo V3 Image-to-3D.

Edit the USER CONFIGURATION section, then run this file.
"""

from __future__ import annotations

import sys
from pathlib import Path

_SCRIPT_DIR = Path(__file__).resolve().parent
if str(_SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(_SCRIPT_DIR))

from tripo_common import (
    TripoError,
    create_task,
    download_task_models,
    fail,
    poll_task,
    require_file,
    upload_file,
)

# ============================================================
# USER CONFIGURATION
# ============================================================
#
# API key is loaded from TRIPO_API_KEY in the repo-root .env
# (or the process environment). Do not paste the key here.

INPUT_IMAGE_PATH = Path(r"C:\\Users\\micha\\Downloads\\Real_Hands.png")
OUTPUT_DIRECTORY = Path(r"C:\\Users\\micha\\Desktop\\Sequential_Runs\\Image_To_3D_Tripo\\Test_Tripo_Output")

# Required. V3 has no server-side default.
#   v3.1-20260211  latest, best quality
#   v3.0-20250812  stable
#   v2.5-20250123  balanced (omit GEOMETRY_QUALITY if you use this)
#   P1-20260311    low-poly game assets (omit GEOMETRY_QUALITY)
MODEL = "v2.5-20250123"

TEXTURE = True
PBR = True
TEXTURE_QUALITY = "standard"  # standard | detailed | extreme  (fast needs texture_version)
GEOMETRY_QUALITY = "standard"  # standard | detailed (Ultra). Omit for P1 / v2.5.

ENABLE_IMAGE_AUTOFIX = False  # docs default; True can help messy photos
ORIENTATION = "align_image"  # default | align_image
TEXTURE_ALIGNMENT = "original_image"  # original_image | geometry
AUTO_SIZE = False  # True = real-world meters (useful for VR)

# Leave these unset for models you plan to rig / retarget later.
# Tripo uses adaptive topology when FACE_LIMIT is omitted.
# Do not set EXPORT_ORIENTATION if this mesh will go through post-processing.
FACE_LIMIT: int | None = None
QUAD: bool | None = None  # True forces FBX
SMART_LOW_POLY: bool | None = None
GENERATE_PARTS: bool | None = None
COMPRESS: str | None = None  # e.g. "geometry"
EXPORT_ORIENTATION: str | None = None  # do not set for later rig/retarget

# ============================================================
# IMPLEMENTATION
# ============================================================

_IMAGE_SUFFIXES = {".png", ".jpg", ".jpeg", ".webp"}
_IMAGE_MAX_BYTES = 20 * 1024 * 1024


def _geometry_quality_allowed(model: str) -> bool:
    version = model.strip().lower()
    if version.startswith("p1"):
        return False
    try:
        major = int(version.lstrip("v").split("-", 1)[0].split(".", 1)[0])
    except (ValueError, IndexError):
        return False
    return major >= 3


def build_body(file_token: str) -> dict:
    body: dict = {
        "input": file_token,
        "model": MODEL,
        "texture": TEXTURE,
        "pbr": PBR,
        "texture_quality": TEXTURE_QUALITY,
        "enable_image_autofix": ENABLE_IMAGE_AUTOFIX,
        "orientation": ORIENTATION,
        "texture_alignment": TEXTURE_ALIGNMENT,
        "auto_size": AUTO_SIZE,
    }
    if GEOMETRY_QUALITY and _geometry_quality_allowed(MODEL):
        body["geometry_quality"] = GEOMETRY_QUALITY
    if FACE_LIMIT is not None:
        body["face_limit"] = int(FACE_LIMIT)
    if QUAD is not None:
        body["quad"] = QUAD
    if SMART_LOW_POLY is not None:
        body["smart_low_poly"] = SMART_LOW_POLY
    if GENERATE_PARTS is not None:
        body["generate_parts"] = GENERATE_PARTS
    if COMPRESS:
        body["compress"] = COMPRESS
    if EXPORT_ORIENTATION:
        body["export_orientation"] = EXPORT_ORIENTATION
    return body


def main() -> int:
    image = require_file(INPUT_IMAGE_PATH, kind="Input image")
    if image.suffix.lower() not in _IMAGE_SUFFIXES:
        fail(f"Unsupported image type {image.suffix}. Use PNG, JPEG, or WebP.")
    size = image.stat().st_size
    if size > _IMAGE_MAX_BYTES:
        fail(f"Image is {size} bytes; Tripo image uploads must be <= 20 MB.")

    file_token = upload_file(image, label="image")

    task_id = create_task("/generation/image-to-model", build_body(file_token))
    task = poll_task(task_id)

    download_task_models(
        task,
        OUTPUT_DIRECTORY,
        filename_stem=image.stem,
    )
    print()
    print(f"Generation task ID: {task_id}")
    print("(Keep this if you want to pass the model into later Tripo tasks.)")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except TripoError as exc:
        fail(str(exc))
