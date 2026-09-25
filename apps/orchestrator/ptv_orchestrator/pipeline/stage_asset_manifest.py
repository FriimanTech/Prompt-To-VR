"""Stage 2: BuildSpec -> AssetManifest (Claude Sonnet).

Reconciliation. Two kinds of item are dropped after the model answers, both
because they cost real money and produce nothing usable:

* ``environment`` items -- the world is authored in C++ by
  ``world_generation``, not generated as a GLB.
* audio items -- every manifest item becomes a reference image plus a paid
  Tripo image-to-3D call, and a gunshot has no shape. A "Pistol Shot Audio"
  item yields a picture of a muzzle flash, a mesh of that picture, and a
  silent blob in the scene. The prompt forbids them; this is the
  belt-and-suspenders behind it, because the drop has to hold on a fallback
  model too.
"""

from __future__ import annotations

import json
import re
from typing import Iterable, List

from ptv_schemas import (
    AssetCategory,
    AssetManifest,
    PlayerExperience,
    ProjectStage,
    ProjectStatus,
)
from ptv_schemas.asset_manifest import AssetItem

from .context import PipelineContext, StageResult
from .llm import make_llm
from .player_experience import is_player_avatar_item
from .prompts import ASSET_MANIFEST_SYSTEM

# Words that only ever describe a SOUND. Matched as whole tokens against the
# id and name, so "soundproof_panel" and "Vox Machina Banner" survive.
# Deliberately excludes near-misses that name real geometry: "stinger" (a
# missile), "ambient" (lighting), "horn" (an animal's), "track" (a rail).
_AUDIO_TOKENS = frozenset(
    {
        "audio",
        "sfx",
        "soundfx",
        "sound",
        "sounds",
        "soundscape",
        "soundtrack",
        "music",
        "bgm",
        "foley",
        "voiceover",
        "vox",
        "ambience",
    }
)

# ...unless the item also names a physical sound-MAKING object, which is a
# perfectly good prop with a silhouette. Fail-open on purpose: a wrong
# exemption merely keeps an asset, while a wrong drop deletes one the game
# needs.
_SOUND_SOURCE_TOKENS = frozenset(
    {
        "speaker",
        "speakers",
        "soundbar",
        "subwoofer",
        "amplifier",
        "amp",
        "radio",
        "intercom",
        "megaphone",
        "loudspeaker",
        "siren",
        "klaxon",
        "alarm",
        "bell",
        "bells",
        "jukebox",
        "gramophone",
        "phonograph",
        "boombox",
        "headphones",
        "headset",
        "microphone",
        "mic",
        "drum",
        "guitar",
        "piano",
        "trumpet",
        "whistle",
        "turntable",
    }
)


def _tokens(*values: str) -> set[str]:
    out: set[str] = set()
    for value in values:
        out.update(t for t in re.split(r"[^a-z0-9]+", value.lower()) if t)
    return out


def is_audio_item(item: AssetItem) -> bool:
    """True when this item names a sound rather than a thing with a shape."""
    tokens = _tokens(item.id, item.name)
    if not tokens & _AUDIO_TOKENS:
        return False
    return not (tokens & _SOUND_SOURCE_TOKENS)


def _partition(
    items: Iterable[AssetItem],
) -> tuple[List[AssetItem], List[AssetItem], List[AssetItem]]:
    """Split items into (kept, dropped_environment, dropped_audio)."""
    kept: List[AssetItem] = []
    environment: List[AssetItem] = []
    audio: List[AssetItem] = []
    for item in items:
        if item.category == AssetCategory.environment:
            environment.append(item)
        elif is_audio_item(item):
            audio.append(item)
        else:
            kept.append(item)
    return kept, environment, audio


def run(ctx: PipelineContext) -> StageResult:
    ctx.emit(ProjectStage.asset_manifest, "running", "Planning AssetManifest")
    state = ctx.store.load_state(ctx.project_id)
    ctx.store.record_stage(state, ProjectStage.asset_manifest, ProjectStatus.running)

    build_spec = ctx.build_spec or ctx.store.load_build_spec(ctx.project_id)
    ctx.build_spec = build_spec

    client = make_llm(ctx.settings)
    resp = client.generate_json(
        prompt=(
            "BuildSpec (JSON):\n"
            + json.dumps(build_spec.model_dump(mode="json"), indent=2)
            + "\n\nProduce an AssetManifest JSON object for the assets above."
            + (
                "\n\nplayer_experience is 'hands_only': plan NO player-avatar"
                " / player-body item. Hand-shaped props (gloves, gauntlets)"
                " are ordinary props with wearable_region set."
                if build_spec.player_experience == PlayerExperience.hands_only
                else ""
            )
        ),
        schema=AssetManifest.model_json_schema(by_alias=True),
        system=ASSET_MANIFEST_SYSTEM,
        model=ctx.settings.claude_light_model,
    )
    manifest = AssetManifest.model_validate(resp.data)

    # Drop what the pipeline cannot turn into a usable GLB. Only applied when
    # something survives, because AssetManifest requires at least one item and
    # an empty manifest is a worse failure than a wasted Tripo call.
    kept, env_items, audio_items = _partition(manifest.items)
    notes: list[str] = []
    if kept and (env_items or audio_items):
        manifest = manifest.model_copy(update={"items": kept})
        if env_items:
            notes.append(
                f"dropped {len(env_items)} environment asset(s);"
                " world is C++-generated"
            )
        if audio_items:
            ids = ", ".join(i.id for i in audio_items)
            notes.append(
                f"dropped {len(audio_items)} audio asset(s) ({ids});"
                " the manifest describes geometry only"
            )
            ctx.emit(
                ProjectStage.asset_manifest,
                "info",
                "Sound has no shape, so these items would have cost a"
                f" reference image and a Tripo call for nothing: {ids}",
            )

    # Hands Only has no body: a player-avatar item would cost a reference
    # image, a Tripo mesh, a rig and animations for something never shown.
    if build_spec.player_experience == PlayerExperience.hands_only:
        bodies = [i for i in manifest.items if is_player_avatar_item(i)]
        rest = [i for i in manifest.items if not is_player_avatar_item(i)]
        if bodies and rest:
            manifest = manifest.model_copy(update={"items": rest})
            ids = ", ".join(i.id for i in bodies)
            notes.append(
                f"dropped {len(bodies)} player-avatar asset(s) ({ids});"
                " Hands Only uses the engine's default hands"
            )
            ctx.emit(
                ProjectStage.asset_manifest,
                "info",
                f"Hands Only: no player body is generated ({ids})",
            )

    ctx.asset_manifest = manifest
    art = ctx.store.save_asset_manifest(ctx.project_id, manifest)

    msg = f"Planned {len(manifest.items)} assets"
    if notes:
        msg += " (" + "; ".join(notes) + ")"
    ctx.store.record_stage(
        state,
        ProjectStage.asset_manifest,
        ProjectStatus.succeeded,
        message=msg,
        artifacts=[art.path],
    )
    ctx.emit(ProjectStage.asset_manifest, "succeeded", f"{len(manifest.items)} assets")
    return StageResult(
        stage=ProjectStage.asset_manifest, artifacts=[art.path], data=manifest
    )
