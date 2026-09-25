"""AssetManifest: per-asset reference image + Tripo request instructions."""

from __future__ import annotations

from enum import Enum
from typing import List, Optional

from pydantic import Field

from .common import IdStr, StrictModel
from .performance import AssetImportance


class AssetCategory(str, Enum):
    environment = "environment"
    prop = "prop"
    character = "character"
    fx = "fx"
    ui = "ui"


class GenerationStrategy(str, Enum):
    image_to_3d = "image_to_3d"
    text_to_3d = "text_to_3d"


class TripoTextureQuality(str, Enum):
    standard = "standard"
    detailed = "detailed"


class TripoTextureAlignment(str, Enum):
    original_image = "original_image"
    geometry = "geometry"


class AnimationIntent(str, Enum):
    """Semantic animations an asset can be given, independent of rig type.

    Deliberately NOT raw Tripo preset ids. Which presets exist depends on the
    rig type and rig model, neither of which is known until the free
    rig-check runs -- long after the manifest is written. The animation stage
    maps these onto concrete presets once it knows the rig, and silently has
    fewer options for a creature (a quadruped rig ships exactly one
    locomotion preset) than for a humanoid.
    """

    idle = "idle"
    walk = "walk"
    run = "run"
    attack = "attack"
    shoot = "shoot"
    hurt = "hurt"
    die = "die"
    jump = "jump"
    turn = "turn"
    climb = "climb"
    dance = "dance"
    wave = "wave"
    sit = "sit"


class TripoRequest(StrictModel):
    """Shape of the Tripo image-to-3D / text-to-3D request.

    Textured, PBR, image-aligned defaults are REQUIRED per project policy.
    """

    texture: bool = True
    pbr: bool = True
    texture_quality: TripoTextureQuality = TripoTextureQuality.standard
    texture_alignment: TripoTextureAlignment = (
        TripoTextureAlignment.original_image
    )
    face_limit: Optional[int] = Field(default=None, ge=64, le=50000)
    model_version: Optional[str] = Field(
        default=None,
        description=(
            "Optional explicit model version (e.g. 'v2.5-20250123', 'p1')."
            " When None the client uses its default."
        ),
    )


class ReferencePose(str, Enum):
    """The pose the reference image should depict.

    This exists because the downstream 3D asset inherits whatever pose the
    image had, and a character captured mid-stride is far less useful than one
    captured in a neutral spread: rigging, retargeting and every external 3D
    tool expect a T-pose, so a character generated in an action pose is
    effectively locked to this pipeline.

    * ``natural`` — whatever best shows the object off. Correct for props,
      scenery, FX and UI, and for a creature used as a grabbable prop rather
      than an actor.
    * ``t_pose`` — arms straight out to the sides, legs straight and slightly
      apart, facing camera. For humanoid and bipedal characters, including a
      cartoon duck whose wings read as arms.
    * ``neutral_quadruped`` — standing square with the legs clearly separated.
      A T-pose is meaningless for a four-legged, serpentine or aquatic
      creature; this is the equivalent neutral capture.
    """

    natural = "natural"
    t_pose = "t_pose"
    neutral_quadruped = "neutral_quadruped"


class ReferenceCoverage(str, Enum):
    """How much of a character the reference image should contain.

    Defaults to ``full_body``, including a head, and that default is
    load-bearing: a head that exists can be hidden at runtime, while a head
    that was never generated can never be shown. ``neck_down`` is therefore
    reserved for prompts that explicitly ask for a body-only avatar.
    """

    full_body = "full_body"
    neck_down = "neck_down"


class CharacterRole(str, Enum):
    """What a character asset is FOR.

    Category alone cannot answer this. A duck is ``category: character``
    whether it patrols the level as an enemy or sits on a shelf to be picked
    up, and the two want different reference images: the actor needs a T-pose
    so it can be rigged and reused, the prop does not.

    Replaces the ``style_notes`` substring sniffing the scene compiler used to
    do, which guessed the player avatar from phrases like "player body".
    """

    none = "none"
    npc = "npc"
    player_avatar = "player_avatar"


class WearableRegion(str, Enum):
    """Which part of the PLAYER'S body an item is designed to be worn on.

    Descriptive only today. In a Hands Only game an item marked ``hands`` or
    ``forearms`` (a glove, a gauntlet) is still generated and placed as an
    ordinary prop -- the player cannot yet wear it AS their hands, because
    nothing rigs a standalone hand model. The label is what lets a future
    hand-rigging update find those items without re-planning the game.
    """

    none = "none"
    hands = "hands"
    forearms = "forearms"
    head = "head"
    torso = "torso"
    legs = "legs"
    feet = "feet"


class ReferenceImageRequest(StrictModel):
    """Reference image parameters sent to Gemini / Imagen.

    ``number_of_images`` is intentionally pinned to 1: nothing in the
    pipeline currently ranks / chooses between multiple candidate
    reference images for the same asset (the Tripo stage picks the
    first alphabetically). Allowing >1 just burns API credits with no
    quality benefit, so we cap the request at one image per asset
    until a real selection step exists.
    """

    prompt: str = Field(..., max_length=4000)
    aspect_ratio: str = Field(default="1:1", pattern=r"^\d{1,2}:\d{1,2}$")
    number_of_images: int = Field(default=1, ge=1, le=1)
    negative_prompt: Optional[str] = Field(default=None, max_length=1000)
    # Proposed by the manifest agent, then RECONCILED deterministically by the
    # reference-image stage: the agent supplies the creature-shape hint it is
    # good at, and the pipeline decides the pose from facts on disk rather
    # than trusting a model to remember a rule.
    pose: ReferencePose = ReferencePose.natural
    coverage: ReferenceCoverage = ReferenceCoverage.full_body


class AssetItem(StrictModel):
    id: IdStr
    name: str = Field(..., max_length=128)
    category: AssetCategory
    style_notes: str = Field(..., max_length=2000)
    # What this character is for, when it is one. Read by the reference-image
    # stage to decide pose and by the scene compiler to place the avatar,
    # replacing style_notes keyword matching.
    character_role: CharacterRole = CharacterRole.none
    # Body part the item is worn on, when it is a wearable (see
    # WearableRegion). ``none`` for everything else.
    wearable_region: WearableRegion = WearableRegion.none
    reference: ReferenceImageRequest
    strategy: GenerationStrategy = GenerationStrategy.image_to_3d
    tripo: TripoRequest = Field(default_factory=TripoRequest)
    target_scale_meters: float = Field(default=1.0, gt=0.0, lt=30.0)
    collision_shape: str = Field(
        default="aabb", max_length=32,
        description=(
            "Deterministic collision primitive: 'aabb', 'sphere', 'capsule',"
            " or 'mesh' (from GLB)."
        ),
    )
    rigged: bool = False
    animations: List[AnimationIntent] = Field(
        default_factory=list,
        max_length=5,
        description=(
            "Animations this asset needs, as semantic intents rather than"
            " Tripo preset ids. Only meaningful when 'rigged' is true; ignored"
            " otherwise. Each one is a separate billed retarget call, so ask"
            " for what the gameplay actually uses -- an enemy that patrols and"
            " attacks needs ['idle', 'walk', 'run', 'attack'], while a"
            " background villager may need only ['idle', 'walk']. Intents the"
            " asset's rig cannot express are dropped automatically."
        ),
    )
    socket_hints: List[str] = Field(
        default_factory=list,
        max_length=8,
        description=(
            "Named attachment points gameplay will need on this asset, e.g."
            " ['muzzle', 'grip'] for a weapon. The deterministic"
            " asset_contract stage derives their transforms from the geometry"
            " and the scene compiler writes them into scene.json, so generated"
            " C++ can fire from the barrel tip via"
            " ctx.sockets->socket_world(self, 'muzzle') without ever"
            " inspecting the mesh. Leave empty for scenery and any asset no"
            " mechanic attaches to."
        ),
    )
    # --- Optional performance hints -------------------------------------
    # Both are optional: the asset_optimization stage derives sensible values
    # from category/scale/interactivity when the planner leaves them unset.
    importance: Optional[AssetImportance] = Field(
        default=None,
        description=(
            "How much of the triangle budget this asset deserves. 'hero' for"
            " things inspected up close (characters, held props),"
            " 'background' for distant set dressing. Derived when omitted."
        ),
    )
    max_triangles: Optional[int] = Field(
        default=None, ge=100, le=200_000,
        description=(
            "Hard per-asset triangle cap after decimation. Overrides the"
            " budget allocator for this asset when set."
        ),
    )
    hold_rotation_offset_deg: Optional[List[float]] = Field(
        default=None,
        min_length=3,
        max_length=3,
        description=(
            "Optional XYZ Euler correction (degrees) to how this asset sits"
            " in the hand when grabbed by its grip socket. Leave unset: the"
            " grip socket already points the business end where the"
            " controller aims. Only for an asset that is known to hold wrong."
        ),
    )


class AssetManifest(StrictModel):
    prompt_id: IdStr
    items: List[AssetItem] = Field(..., min_length=1, max_length=48)
