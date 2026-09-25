#!/usr/bin/env python3
"""List joints in the Uthana-rigged GLB and probe BodySlots-style name matches.

Pure stdlib GLB JSON walk — no engine dependency. Mirrors the finger / limb
slot aliases in engine/runtime/src/animation/BodySlots.cpp closely enough to
warn before a Quest sideload if Uthana naming will miss.
"""

from __future__ import annotations

import json
import re
import struct
import sys
from pathlib import Path

_SCRIPTS = Path(__file__).resolve().parent
if str(_SCRIPTS) not in sys.path:
    sys.path.insert(0, str(_SCRIPTS))

from _paths import RIGGED_AVATAR_GLB, UTHANA_RIG_META  # noqa: E402


def _normalize_joint_name(name: str) -> str:
    lowered = name.lower()
    for prefix in ("mixamorig:", "mixamorig", "armature|"):
        if lowered.startswith(prefix):
            lowered = lowered[len(prefix) :]
            break
    return re.sub(r"[^a-z0-9]", "", lowered)


def _read_glb_json(path: Path) -> dict:
    data = path.read_bytes()
    if len(data) < 12 or data[0:4] != b"glTF":
        raise ValueError(f"Not a GLB: {path}")
    _magic, version, length = struct.unpack_from("<III", data, 0)
    if version != 2:
        raise ValueError(f"Unsupported GLB version {version}")
    offset = 12
    while offset + 8 <= len(data):
        chunk_len, chunk_type = struct.unpack_from("<II", data, offset)
        offset += 8
        chunk = data[offset : offset + chunk_len]
        offset += chunk_len
        if chunk_type == 0x4E4F534A:  # JSON
            return json.loads(chunk.decode("utf-8"))
    raise ValueError("GLB has no JSON chunk")


def _joint_names(gltf: dict) -> list[str]:
    nodes = gltf.get("nodes") or []
    names: list[str] = []
    skins = gltf.get("skins") or []
    if skins:
        seen: set[int] = set()
        for skin in skins:
            for idx in skin.get("joints") or []:
                if idx in seen:
                    continue
                seen.add(idx)
                node = nodes[idx] if idx < len(nodes) else {}
                names.append(node.get("name") or f"node_{idx}")
        return names
    # Fallback: every named node (unskinned / unexpected export).
    return [n.get("name") or f"node_{i}" for i, n in enumerate(nodes) if n.get("name")]


# Candidate lists mirrored from BodySlots (normalized form).
_SLOTS: dict[str, list[str]] = {
    "hips": ["hips", "pelvis", "hip", "bip01pelvis", "root"],
    "neck": ["neck", "neck01", "neck1", "necktwist01"],
    "head": ["head", "head01", "headtop", "bip01head"],
    "shoulder_l": ["leftshoulder", "shoulderl", "lshoulder", "leftclavicle"],
    "shoulder_r": ["rightshoulder", "shoulderr", "rshoulder", "rightclavicle"],
    "upper_arm_l": ["leftarm", "leftupperarm", "upperarml", "luparm"],
    "upper_arm_r": ["rightarm", "rightupperarm", "upperarmr", "ruparm"],
    "forearm_l": ["leftforearm", "leftlowerarm", "forearml", "llowarm"],
    "forearm_r": ["rightforearm", "rightlowerarm", "forearmr", "rlowarm"],
    "hand_l": ["lefthand", "handl", "lhand", "lefthand"],
    "hand_r": ["righthand", "handr", "rhand", "righthand"],
    "index_1_l": [
        "lefthandindex1",
        "index01l",
        "index1l",
        "findex01l",
        "index01",
        "index1",
    ],
    "index_1_r": [
        "righthandindex1",
        "index01r",
        "index1r",
        "findex01r",
        "index01",
        "index1",
    ],
    "thumb_1_l": [
        "lefthandthumb1",
        "thumb01l",
        "thumb1l",
        "thumb01",
        "thumb1",
    ],
    "thumb_1_r": [
        "righthandthumb1",
        "thumb01r",
        "thumb1r",
        "thumb01",
        "thumb1",
    ],
}


def _resolve(slot: str, normalized: set[str]) -> str | None:
    for cand in _SLOTS[slot]:
        if cand in normalized:
            return cand
    return None


def main() -> int:
    path = RIGGED_AVATAR_GLB
    if len(sys.argv) > 1:
        path = Path(sys.argv[1])
    if not path.is_file():
        raise SystemExit(
            f"Rigged GLB not found: {path}\nRun scripts/rig_avatar.py first."
        )

    gltf = _read_glb_json(path)
    joints = _joint_names(gltf)
    print(f"GLB: {path}")
    print(f"Joint count: {len(joints)}")
    print("--- joint names ---")
    for name in joints:
        print(f"  {name}")

    normalized = {_normalize_joint_name(n) for n in joints}
    print("--- BodySlots probe ---")
    misses: list[str] = []
    for slot in _SLOTS:
        hit = _resolve(slot, normalized)
        status = f"OK ({hit})" if hit else "MISS"
        if not hit:
            misses.append(slot)
        print(f"  {slot}: {status}")

    fingerish = [
        n
        for n in joints
        if re.search(r"thumb|index|middle|ring|pinky|finger", n, re.I)
    ]
    print(f"Finger-like joints: {len(fingerish)}")
    for n in fingerish:
        print(f"  {n}")

    if UTHANA_RIG_META.is_file():
        meta = json.loads(UTHANA_RIG_META.read_text(encoding="utf-8"))
        print(
            f"Uthana character_id={meta.get('character_id')} "
            f"confidence={meta.get('auto_rig_confidence')}"
        )

    if misses:
        print(
            "WARNING: some slots missed. Arm IK / finger curl may be incomplete "
            f"until names match BodySlots aliases: {', '.join(misses)}"
        )
        return 2
    print("All probed slots resolved.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
