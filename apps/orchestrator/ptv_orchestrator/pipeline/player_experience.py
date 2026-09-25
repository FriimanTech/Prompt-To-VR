"""Keep ``BuildSpec.player_experience`` in step with the project's own choice.

``ProjectState.player_experience`` is the single source of truth: the user
picks Hands Only or Full Player Avatar in the UI at create time, next to the
target device. ``BuildSpec.player_experience`` is a MIRROR of it -- the same
arrangement as ``target_device.py`` -- so the build_spec stage overwrites
whatever the model returned and existing projects are reconciled at run
start.

Hands Only also pins the player's body: there is no full-body avatar, so the
avatar coverage is ``hands_only`` whatever the model wrote.
"""

from __future__ import annotations

from ptv_schemas import AssetItem, BuildSpec, PlayerExperience
from ptv_schemas.asset_manifest import AssetCategory, CharacterRole
from ptv_schemas.build_spec import AvatarCoverage

# Phrases that mark a BuildSpec / manifest asset as the player's own body.
# The manifest's ``character_role`` is authoritative; these only catch a
# model that forgot to set it.
_AVATAR_PHRASES = (
    "player avatar",
    "player body",
    "player's body",
    "first-person body",
    "first person body",
    "player character model",
    "full-body avatar",
    "full body avatar",
)


def sync_build_spec_experience(
    spec: BuildSpec, experience: PlayerExperience
) -> tuple[BuildSpec, bool]:
    """Return ``(spec, changed)`` with ``player_experience`` forced to match,
    and the avatar coverage pinned for Hands Only."""
    update: dict[str, object] = {}
    if spec.player_experience != experience:
        update["player_experience"] = experience
    if (
        experience == PlayerExperience.hands_only
        and spec.player_avatar.coverage != AvatarCoverage.hands_only
    ):
        update["player_avatar"] = spec.player_avatar.model_copy(
            update={"coverage": AvatarCoverage.hands_only}
        )
    if not update:
        return spec, False
    return spec.model_copy(update=update), True


def is_player_avatar_item(item: AssetItem) -> bool:
    """True when a manifest item is the player's own full body."""
    if item.character_role == CharacterRole.player_avatar:
        return True
    if item.category != AssetCategory.character:
        return False
    text = f"{item.id} {item.name} {item.style_notes}".lower()
    return any(p in text for p in _AVATAR_PHRASES)


def is_player_avatar_entry(asset_id: str, name: str, summary: str,
                           category: str) -> bool:
    """Same test for a BuildSpec ``AssetEntry``, which has no role field."""
    if category.lower() not in ("character", "avatar", "player"):
        return False
    text = f"{asset_id} {name} {summary}".lower()
    return any(p in text for p in _AVATAR_PHRASES)


def strip_player_avatar_assets(spec: BuildSpec) -> tuple[BuildSpec, list[str]]:
    """Remove player-body assets from a Hands Only BuildSpec.

    Returns the spec and the ids removed. Never removes the last asset (the
    schema needs one) and never touches hand-shaped props: a gauntlet is a
    prop, not a body.
    """
    if spec.player_experience != PlayerExperience.hands_only:
        return spec, []
    keep = []
    dropped: list[str] = []
    for a in spec.assets:
        if is_player_avatar_entry(a.id, a.name, a.summary, a.category):
            dropped.append(a.id)
        else:
            keep.append(a)
    if not dropped or not keep:
        return spec, []
    return spec.model_copy(update={"assets": keep}), dropped
