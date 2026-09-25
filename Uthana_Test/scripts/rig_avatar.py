#!/usr/bin/env python3
"""Upload player_range_avatar to Uthana, auto-rig with fingers, download GLB.

The published ``uthana`` client (0.4.x) does not expose
``auto_rig_strip_namespace``, so this script uses the documented GraphQL
multipart mutation and forces strip_namespace=True.
"""

from __future__ import annotations

import asyncio
import json
import sys
from pathlib import Path

import httpx

# Allow ``python scripts/rig_avatar.py`` from any cwd.
_SCRIPTS = Path(__file__).resolve().parent
if str(_SCRIPTS) not in sys.path:
    sys.path.insert(0, str(_SCRIPTS))

from _paths import (  # noqa: E402
    AVATAR_ASSET_DIR,
    RIGGED_AVATAR_GLB,
    SOURCE_AVATAR_GLB,
    UTHANA_RIG_META,
    require_uthana_api_key,
)

# Matches https://uthana.com/docs/api/capabilities/auto-rig-and-add-character
# plus auto_rig_strip_namespace (documented default false; we force true).
_CREATE_CHARACTER = """
mutation CreateCharacter(
    $name: String!,
    $file: Upload!,
    $auto_rig: Boolean,
    $auto_rig_front_facing: Boolean,
    $rerig_target: String,
    $include_fingers: Boolean,
    $auto_rig_strip_namespace: Boolean
) {
    create_character(
        name: $name,
        file: $file,
        auto_rig: $auto_rig,
        auto_rig_front_facing: $auto_rig_front_facing,
        rerig_target: $rerig_target,
        include_fingers: $include_fingers,
        auto_rig_strip_namespace: $auto_rig_strip_namespace
    ) {
        character {
            id
            name
        }
        auto_rig_confidence
    }
}
"""


async def _create_with_strip_namespace(
    api_key: str, glb_path: Path, *, name: str
) -> tuple[str, float | None]:
    variables = {
        "name": name,
        "file": None,
        "auto_rig": True,
        "auto_rig_front_facing": True,
        "rerig_target": None,
        "include_fingers": True,
        "auto_rig_strip_namespace": True,
    }
    operations = json.dumps({"query": _CREATE_CHARACTER, "variables": variables})
    map_data = json.dumps({"0": ["variables.file"]})

    async with httpx.AsyncClient(auth=(api_key, ""), timeout=360.0) as http:
        with glb_path.open("rb") as f:
            response = await http.post(
                "https://uthana.com/graphql",
                data={"operations": operations, "map": map_data},
                files={"0": (glb_path.name, f, "application/octet-stream")},
            )
    payload = response.json()
    if response.status_code >= 400 or payload.get("errors"):
        raise RuntimeError(
            f"Uthana create_character failed "
            f"(HTTP {response.status_code}): {json.dumps(payload, indent=2)}"
        )
    data = (payload.get("data") or {}).get("create_character") or {}
    character = data.get("character") or {}
    character_id = character.get("id")
    if not character_id:
        raise RuntimeError(f"No character id in response: {payload}")
    confidence = data.get("auto_rig_confidence")
    return character_id, confidence


async def _download_glb(api_key: str, character_id: str) -> bytes:
    from uthana import Uthana

    client = Uthana(api_key)
    return await client.characters.download(character_id, output_format="glb")


async def main() -> int:
    api_key = require_uthana_api_key()
    if not SOURCE_AVATAR_GLB.is_file():
        raise SystemExit(f"Source avatar GLB not found: {SOURCE_AVATAR_GLB}")

    AVATAR_ASSET_DIR.mkdir(parents=True, exist_ok=True)
    print(f"Uploading {SOURCE_AVATAR_GLB} (auto_rig + fingers + strip_namespace)...")
    character_id, confidence = await _create_with_strip_namespace(
        api_key,
        SOURCE_AVATAR_GLB,
        name="player_range_avatar_uthana_test",
    )
    print(f"character_id={character_id} auto_rig_confidence={confidence}")

    print("Downloading rigged GLB...")
    glb_bytes = await _download_glb(api_key, character_id)
    RIGGED_AVATAR_GLB.write_bytes(glb_bytes)
    print(f"Wrote {RIGGED_AVATAR_GLB} ({len(glb_bytes)} bytes)")

    meta = {
        "character_id": character_id,
        "auto_rig_confidence": confidence,
        "source_glb": str(SOURCE_AVATAR_GLB),
        "output_glb": str(RIGGED_AVATAR_GLB),
        "params": {
            "auto_rig": True,
            "auto_rig_front_facing": True,
            "include_fingers": True,
            "auto_rig_strip_namespace": True,
        },
    }
    UTHANA_RIG_META.write_text(json.dumps(meta, indent=2), encoding="utf-8")
    print(f"Wrote {UTHANA_RIG_META}")
    print("Next: python Uthana_Test/scripts/inspect_rig.py")
    return 0


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main()))
