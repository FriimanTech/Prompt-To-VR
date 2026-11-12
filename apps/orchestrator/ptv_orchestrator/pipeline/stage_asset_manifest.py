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
    BuildSpec,
    PlayerExperience,
    ProjectStage,
    ProjectStatus,
)
from ptv_schemas.asset_manifest import AssetItem, CharacterRole

from . import requirements_ledger as rl
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


def missing_roster_assets(manifest: AssetManifest, build_spec: BuildSpec) -> list[str]:
    """NPC roster variants with no manifest item."""
    have = {i.id for i in manifest.items}
    return [
        v.asset_id
        for fam in build_spec.npc_roster
        for v in fam.variants
        if v.asset_id not in have
    ]


def normalise_roster_items(
    manifest: AssetManifest, build_spec: BuildSpec
) -> tuple[AssetManifest, list[str]]:
    """Roster variants are NPC characters: make the manifest say so, so they
    get a T-pose reference, a rig and animations."""
    variant_ids = {v.asset_id for fam in build_spec.npc_roster for v in fam.variants}
    if not variant_ids:
        return manifest, []
    fixed: list[str] = []
    items = []
    for item in manifest.items:
        if item.id in variant_ids and (
            item.category != AssetCategory.character
            or item.character_role != CharacterRole.npc
        ):
            item = item.model_copy(update={
                "category": AssetCategory.character,
                "character_role": CharacterRole.npc,
            })
            fixed.append(item.id)
        items.append(item)
    if fixed:
        manifest = manifest.model_copy(update={"items": items})
    return manifest, fixed


def run(ctx: PipelineContext) -> StageResult:
    ctx.emit(ProjectStage.asset_manifest, "running", "Planning AssetManifest")
    state = ctx.store.load_state(ctx.project_id)
    ctx.store.record_stage(state, ProjectStage.asset_manifest, ProjectStatus.running)

    build_spec = ctx.build_spec or ctx.store.load_build_spec(ctx.project_id)
    ctx.build_spec = build_spec

    client = make_llm(ctx.settings)
    ledger_text = rl.brief(rl.load(ctx.project_dir), "asset_manifest")
    resp = client.generate_json(
        prompt=(
            "BuildSpec (JSON):\n"
            + json.dumps(build_spec.model_dump(mode="json"), indent=2)
            + ("\n\n" + ledger_text if ledger_text else "")
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

    # Every NPC variant the BuildSpec planned is its own model. One dropped
    # here is a whole kind of enemy that never exists, so ask again once.
    missing = missing_roster_assets(manifest, build_spec)
    if missing:
        ctx.emit(
            ProjectStage.asset_manifest,
            "warning",
            f"manifest is missing NPC variant asset(s) {missing}; asking again",
        )
        resp = client.generate_json(
            prompt=(
                "BuildSpec (JSON):\n"
                + json.dumps(build_spec.model_dump(mode="json"), indent=2)
                + "\n\nProduce an AssetManifest JSON object for the assets above."
                + "\n\nYour previous manifest left out these NPC variants from"
                f" the BuildSpec npc_roster: {missing}. Every roster variant"
                " is its own character item (category 'character',"
                " character_role 'npc') with its own look and size."
            ),
            schema=AssetManifest.model_json_schema(by_alias=True),
            system=ASSET_MANIFEST_SYSTEM,
            model=ctx.settings.claude_light_model,
        )
        manifest = AssetManifest.model_validate(resp.data)
        still = missing_roster_assets(manifest, build_spec)
        if still:
            ctx.emit(
                ProjectStage.asset_manifest,
                "warning",
                f"NPC variant asset(s) {still} still missing; those types will"
                " not exist in the game",
            )
    manifest, fixed = normalise_roster_items(manifest, build_spec)
    if fixed:
        ctx.emit(
            ProjectStage.asset_manifest,
            "info",
            f"marked NPC variant(s) {fixed} as rigged characters",
        )

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

    # Every asset / visual requirement needs an item that delivers it.
    ledger = rl.load(ctx.project_dir)
    if ledger is not None:
        known = {r.id for r in ledger.requirements}

        def _claims(m):
            return rl.merge_claims(
                rl.claims_from_manifest(m, known),
                rl.name_claims(ledger, "asset_manifest",
                               [(i.id, i.name) for i in m.items]))

        missing = rl.gate(ledger, "asset_manifest", _claims(manifest))
        if missing:
            ctx.emit(ProjectStage.asset_manifest, "warning",
                     f"{len(missing)} asset requirement(s) unclaimed; asking again")
            resp = client.generate_json(
                prompt=(
                    "BuildSpec (JSON):\n"
                    + json.dumps(build_spec.model_dump(mode="json"), indent=2)
                    + "\n\n" + rl.brief(ledger, "asset_manifest")
                    + "\n\nYour previous manifest (below) has no item for these"
                      " requirements. Return the COMPLETE manifest again with items"
                      " added or extended so each is delivered, listing the ids in"
                      " `satisfies`:\n" + rl.uncovered_text(missing)
                    + "\n\nPrevious manifest:\n" + manifest.model_dump_json(indent=1)[:60000]
                ),
                schema=AssetManifest.model_json_schema(by_alias=True),
                system=ASSET_MANIFEST_SYSTEM,
                model=ctx.settings.claude_light_model,
            )
            retry = AssetManifest.model_validate(resp.data)
            kept_retry, _, _ = _partition(retry.items)
            if build_spec.player_experience == PlayerExperience.hands_only:
                kept_retry = [i for i in kept_retry if not is_player_avatar_item(i)]
            if kept_retry:
                retry = retry.model_copy(update={"items": kept_retry})
                retry, _ = normalise_roster_items(retry, build_spec)
                if not missing_roster_assets(retry, build_spec) or missing_roster_assets(
                        manifest, build_spec):
                    manifest = retry
            missing = rl.gate(ledger, "asset_manifest", _claims(manifest))
        rl.save(ctx.project_dir, ledger)
        if missing:
            msg = ("the asset manifest has no item for these requirements:\n"
                   + rl.uncovered_text(missing))
            ctx.store.record_stage(state, ProjectStage.asset_manifest,
                                   ProjectStatus.failed, message=msg[:400])
            ctx.emit(ProjectStage.asset_manifest, "failed", msg[:400])
            raise rl.RequirementGateError(msg)

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
