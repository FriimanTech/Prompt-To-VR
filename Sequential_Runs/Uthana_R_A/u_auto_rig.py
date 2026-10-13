#!/usr/bin/env python3
"""Standalone Uthana auto-rig / add character.

Edit the USER CONFIGURATION section, then run this file.
Prints character_id — copy it into locomotion_animation.py,
text_to_motion.py, or download_retarget_motion.py.

Official ``client.characters.create_from_file`` (uthana 0.5.1) matches the
docs for auto_rig / front_facing / include_fingers, but still omits
``auto_rig_strip_namespace`` and ``auto_rig_feet_on_floor`` (documented at
https://uthana.com/docs/api/capabilities/auto-rig-and-add-character).
This script therefore posts the documented GraphQL multipart create with
those flags, then downloads via the official client.
"""

from __future__ import annotations

import asyncio
import json
import sys
from pathlib import Path

import httpx

_SCRIPT_DIR = Path(__file__).resolve().parent
if str(_SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(_SCRIPT_DIR))

from uthana_common import (  # noqa: E402
    fail,
    make_client,
    require_file,
    require_uthana_api_key,
)

# ============================================================
# USER CONFIGURATION
# ============================================================
#
# API key is loaded from UTHANA_API_KEY in the repo-root .env
# (or the process environment). Do not paste the key here.

INPUT_MODEL_PATH = Path(
    r"C:\Users\micha\Documents\Prompt-To-VR\projects"
    r"\20260907-144543-shooting-range-game\assets"
    r"\player_range_avatar\model.glb"
)
CHARACTER_NAME = "Shooting_Range_Avatar"

# File path for the rigged GLB, or a directory (then rigged_character.glb).
# Set to None to skip the download (character_id is still printed).
OUTPUT_GLB_PATH: Path | None = Path(
    r"C:\Users\micha\Documents\Prompt-To-VR\Sequential_Runs\Uthana_R_A"
)

# Website API defaults (create_from_file cannot pass these yet):
# - strip_namespace: false → we force True (drop mixamorig: prefixes)
# - feet_on_floor: false → hips at origin; True puts feet on the floor
AUTO_RIG_STRIP_NAMESPACE = True
AUTO_RIG_FEET_ON_FLOOR = True

# ============================================================
# IMPLEMENTATION
# ============================================================

# Website API supports strip_namespace / feet_on_floor; create_from_file does not.
_CREATE_CHARACTER = """
mutation CreateCharacter(
    $name: String!,
    $file: Upload!,
    $auto_rig: Boolean,
    $auto_rig_front_facing: Boolean,
    $rerig_target: String,
    $include_fingers: Boolean,
    $auto_rig_strip_namespace: Boolean,
    $auto_rig_feet_on_floor: Boolean
) {
    create_character(
        name: $name,
        file: $file,
        auto_rig: $auto_rig,
        auto_rig_front_facing: $auto_rig_front_facing,
        rerig_target: $rerig_target,
        include_fingers: $include_fingers,
        auto_rig_strip_namespace: $auto_rig_strip_namespace,
        auto_rig_feet_on_floor: $auto_rig_feet_on_floor
    ) {
        character {
            id
            name
        }
        auto_rig_confidence
    }
}
"""


def _resolve_output_glb(path: Path) -> Path:
    resolved = path.expanduser().resolve()
    if resolved.is_dir() or resolved.suffix.lower() != ".glb":
        return resolved / "rigged_character.glb"
    return resolved


async def _create_with_strip_namespace(
    api_key: str,
    model_path: Path,
    *,
    name: str,
) -> tuple[str, float | None]:
    variables = {
        "name": name,
        "file": None,
        "auto_rig": True,
        "auto_rig_front_facing": True,
        "rerig_target": None,
        "include_fingers": True,
        "auto_rig_strip_namespace": AUTO_RIG_STRIP_NAMESPACE,
        "auto_rig_feet_on_floor": AUTO_RIG_FEET_ON_FLOOR,
    }
    operations = json.dumps({"query": _CREATE_CHARACTER, "variables": variables})
    map_data = json.dumps({"0": ["variables.file"]})

    async with httpx.AsyncClient(auth=(api_key, ""), timeout=360.0) as http:
        with model_path.open("rb") as handle:
            response = await http.post(
                "https://uthana.com/graphql",
                data={"operations": operations, "map": map_data},
                files={"0": (model_path.name, handle, "application/octet-stream")},
            )
    payload = response.json()
    if response.status_code >= 400 or payload.get("errors"):
        fail(
            "Uthana create_character failed "
            f"(HTTP {response.status_code}): {json.dumps(payload, indent=2)}"
        )
    data = (payload.get("data") or {}).get("create_character") or {}
    character = data.get("character") or {}
    character_id = character.get("id")
    if not character_id:
        fail(f"No character id in response: {payload}")
    confidence = data.get("auto_rig_confidence")
    return character_id, confidence


async def main() -> int:
    api_key = require_uthana_api_key()
    model_path = require_file(INPUT_MODEL_PATH, label="INPUT_MODEL_PATH")

    print(
        f"Uploading {model_path} "
        f"(auto_rig + front_facing + fingers + "
        f"strip_namespace={AUTO_RIG_STRIP_NAMESPACE} + "
        f"feet_on_floor={AUTO_RIG_FEET_ON_FLOOR})..."
    )
    character_id, confidence = await _create_with_strip_namespace(
        api_key,
        model_path,
        name=CHARACTER_NAME,
    )
    print(f"character_id={character_id}")
    print(f"auto_rig_confidence={confidence}")
    print("Copy character_id into the next script's CHARACTER_ID field.")

    if OUTPUT_GLB_PATH is not None:
        out_path = _resolve_output_glb(OUTPUT_GLB_PATH)
        out_path.parent.mkdir(parents=True, exist_ok=True)
        print(f"Downloading rigged GLB to {out_path}...")
        client = make_client()
        glb_bytes = await client.characters.download(
            character_id,
            output_format="glb",
        )
        out_path.write_bytes(glb_bytes)
        print(f"Wrote {out_path} ({len(glb_bytes)} bytes)")

    print("Next: python query_locomotion_styles.py")
    print("  or: python locomotion_animation.py / text_to_motion.py")
    return 0


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main()))
