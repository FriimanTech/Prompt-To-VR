"""Which texture files a GLB references, and whether they exist.

Shared by ``scene_compile`` (every mesh a scene places must resolve its
textures before the scene is written) and ``quest_build`` (every file the APK
needs is packaged, and nothing it references is missing). Reads only the JSON
chunk of the GLB.
"""

from __future__ import annotations

import json
import struct
from pathlib import Path
from typing import Any


def glb_texture_refs(glb: Path) -> list[dict[str, Any]]:
    """Every texture a GLB references: ``{"uri", "ktx2", "embedded"}``.

    ``uri`` / ``ktx2`` are relative to the GLB's directory (empty when absent);
    ``embedded`` is true for an image stored inside the GLB."""
    try:
        data = glb.read_bytes()
        n = struct.unpack_from("<I", data, 12)[0]
        doc = json.loads(data[20:20 + n])
    except (OSError, ValueError, struct.error):
        return []
    images = doc.get("images") or []
    refs: list[dict[str, Any]] = []
    for tex in doc.get("textures") or []:
        src = tex.get("source")
        img = images[src] if isinstance(src, int) and 0 <= src < len(images) else {}
        ktx2 = ((tex.get("extensions") or {}).get("PTV_texture_astc") or {}).get("uri", "")
        uri = img.get("uri", "") or ""
        if uri.startswith("data:"):
            uri = ""
        refs.append({"uri": uri, "ktx2": ktx2,
                     "embedded": "bufferView" in img or bool(img.get("uri", "").startswith("data:"))})
    return refs


def check_mesh_textures(glb: Path, *, need_ktx2: bool) -> tuple[list[str], list[str], int]:
    """Problems with one GLB's textures.

    Returns ``(missing, no_ktx2, referenced)``: files that do not exist,
    textures that have no ASTC twin (only matters for Quest), and how many
    textures the GLB references at all."""
    missing: list[str] = []
    no_ktx2: list[str] = []
    refs = glb_texture_refs(glb)
    for ref in refs:
        if ref.get("embedded"):
            continue
        for key in ("uri", "ktx2"):
            rel = ref.get(key)
            if rel and not (glb.parent / rel).is_file():
                missing.append(f"{glb.name}: {rel}")
        if need_ktx2 and not ref.get("ktx2"):
            no_ktx2.append(f"{glb.name}: {ref.get('uri') or '?'}")
    return missing, no_ktx2, len(refs)
