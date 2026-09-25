"""BuildSpec: the top-level plan derived from the user's prompt."""

from __future__ import annotations

from enum import Enum
from typing import List, Optional

from pydantic import Field

from .common import IdStr, StrictModel


class GameType(str, Enum):
    arcade = "arcade"
    adventure = "adventure"
    puzzle = "puzzle"
    simulation = "simulation"
    shooter = "shooter"
    sandbox = "sandbox"
    narrative = "narrative"
    experience = "experience"


class TargetDevice(str, Enum):
    """Which VR target the generated game is built and launched for.

    * ``pcvr`` — Windows native ``.exe`` (Vulkan + Win32 OpenXR), run on the
      PC and viewed on the headset over Quest Link. The historical default.
    * ``quest`` — a standalone Android APK (Vulkan + Android OpenXR loader,
      NativeActivity) installed and run locally on the Quest headset.

    Stages 1-9 (BuildSpec .. scene compile, incl. codegen + worldgen) are
    identical for both; only the build + launch stages branch on this value.
    """

    pcvr = "pcvr"
    quest = "quest"


class LocomotionStyle(str, Enum):
    teleport = "teleport"
    smooth = "smooth"
    mixed = "mixed"
    stationary = "stationary"


class ComfortLevel(str, Enum):
    comfort = "comfort"
    standard = "standard"
    intense = "intense"


class MechanicType(str, Enum):
    """Whether a mechanic is a self-contained behavior or a shared foundation.

    * ``regular`` — a closed input -> action loop (e.g. "press A to jump",
      "grab an object", "take damage"). Most mechanics are regular.
    * ``hub`` — a shared foundation that OTHER mechanics build on and branch
      from (e.g. a ``shooting_hub`` that detects a held weapon + the correct
      hand's trigger, which the per-weapon blast profiles then extend). The
      BuildSpec only labels the type; downstream stages (MechanicPlan /
      CodeTask) decide the concrete dependency edges.
    """

    regular = "regular"
    hub = "hub"


class PlayerPerspective(str, Enum):
    """Where the player's viewpoint sits relative to their avatar.

    A modelled head must never imply that the camera lives inside the skull;
    those are independent decisions, and only the prompt can say which the
    game wants.

    * ``first_person_avatar`` — the ordinary case. The camera is at the
      avatar's eyes and the player is inside the body the way they are inside
      their own: they can look down at their hands, arms and legs, but they
      do not see the inside of their own head.
    * ``inside_head_free_look`` — the player is a separate, miniature entity
      inside their own avatar's head. Physical head rotation looks around the
      skull and does NOT rotate the host; the controllers drive the host.
    * ``inside_head_npc_host`` — the player is a miniature entity inside an
      autonomous NPC's head. The controllers drive the miniature player; the
      NPC lives its own life and carries the interior with it.
    * ``inside_head_piloted`` — the player is a miniature entity inside their
      own larger avatar's head, but normal VR controls map to the host: HMD
      rotation turns the host's head, the sticks drive its locomotion and
      turning. The "climb into a giant robot" case.
    """

    first_person_avatar = "first_person_avatar"
    inside_head_free_look = "inside_head_free_look"
    inside_head_npc_host = "inside_head_npc_host"
    inside_head_piloted = "inside_head_piloted"


class AvatarCoverage(str, Enum):
    """How much of the player's body the avatar asset actually contains.

    Default to ``full_body``. A head that exists can always be hidden at
    runtime, while a head that was never generated can never be shown, so
    ``neck_down`` is reserved for prompts that explicitly ask for a body-only
    avatar ("just the suit from the feet to the neck").
    """

    full_body = "full_body"
    neck_down = "neck_down"
    hands_only = "hands_only"


class HeadVisibility(str, Enum):
    """Whether the avatar's head renders, and to whom.

    This is a per-view decision, not a global one: hiding the head so the
    camera is not inside the skull must not also make the avatar headless in
    a mirror.

    * ``hidden_in_self_view`` — the default. Suppressed in the player's own
      view, fully present everywhere else (mirrors, reflections).
    * ``always_visible`` — never suppressed. For prompts where the camera is
      not at the eyes at all.
    * ``toggleable`` — starts hidden in self view, and the game exposes a
      control to show or hide it, e.g. admiring an outfit in a mirror.
    """

    hidden_in_self_view = "hidden_in_self_view"
    always_visible = "always_visible"
    toggleable = "toggleable"


class PlayerAvatarSpec(StrictModel):
    """What the player's own body is, and how it behaves.

    Every field is an independent decision driven by the prompt. Nothing here
    may be inferred from another field: a full head does not imply an
    inside-the-skull camera, and wanting a mirror does not imply wanting legs
    that animate.
    """

    coverage: AvatarCoverage = AvatarCoverage.full_body
    head_visibility: HeadVisibility = HeadVisibility.hidden_in_self_view
    # In-sync leg animation while the player travels. Default true: assume
    # the avatar's legs walk when the player walks unless the prompt says the
    # avatar should just slide.
    leg_animation: bool = True
    # Set when the prompt asks for a mirror, avatar customization, or seeing
    # your own reflection. Drives whether a mirror surface is placed and
    # whether the head must stay renderable.
    needs_mirror: bool = False


class PlayerExperience(str, Enum):
    """What the player IS in the game. Chosen by the user at project
    creation, not by the model -- like ``target_device``.

    * ``hands_only`` -- the player is a pair of hands: the engine's default
      rigged hands, no body, no head mesh. No full-body avatar asset is
      planned or generated. Hand-shaped gameplay props (gloves, gauntlets)
      are still generated as ordinary props.
    * ``full_avatar`` -- the player has a full GLB body generated by the
      pipeline (reference image -> Tripo -> rig), viewed first person, whose
      own hands are the player's hands.
    """

    hands_only = "hands_only"
    full_avatar = "full_avatar"


class MovementMode(str, Enum):
    """A way of travelling across the environment."""

    teleport = "teleport"
    slide_head = "slide_head"
    slide_controller = "slide_controller"


class TurnStyle(str, Enum):
    """A way of turning with the right stick."""

    snap = "snap"
    smooth = "smooth"


class VignetteSpec(StrictModel):
    """Comfort vignette ("tunnel vision") -- OFF unless the prompt asks.

    Applies only to continuous motion: sliding and smooth turning. Snap
    turning and teleport are already the comfort options and never get one.
    """

    slide: bool = False
    smooth_turn: bool = False
    # Show a switch for it in the movement settings menu.
    user_toggle: bool = False


class MovementSpec(StrictModel):
    """How the player moves and turns, decided from the prompt.

    Both lists may be EMPTY. An empty ``modes`` is a stationary game (a
    rhythm game, a fixed-position archery range, a turret) -- the player can
    still move physically in their playspace but has no artificial
    locomotion. An empty ``turning`` means the player turns their real body.
    """

    modes: List[MovementMode] = Field(
        default_factory=lambda: [
            MovementMode.teleport,
            MovementMode.slide_head,
            MovementMode.slide_controller,
        ],
        max_length=3,
    )
    # Which mode is active at start. Only meaningful with 2+ modes.
    default_mode: Optional[MovementMode] = None
    turning: List[TurnStyle] = Field(
        default_factory=lambda: [TurnStyle.snap], max_length=2
    )
    default_turn: Optional[TurnStyle] = None
    vignette: VignetteSpec = Field(default_factory=VignetteSpec)
    # A settings panel the player opens to choose between the modes above.
    # True when the prompt asks for a settings / comfort menu, or when the
    # game offers a choice (2+ modes, or both turn styles).
    settings_menu: bool = True


class EnvironmentMode(str, Enum):
    """Whether a scene's environment is exclusive or spatially nested.

    * ``exclusive`` — one at a time. Entering it unloads the previous
      environment, which is what keeps a multi-map game inside the Quest's
      memory budget.
    * ``nested`` — loaded alongside a host environment and attached to an
      entity in it, so both are active simultaneously. The inside of an
      NPC's head, or a compartment inside a moving spacecraft.
    """

    exclusive = "exclusive"
    nested = "nested"


class SceneKind(str, Enum):
    """The role a scene plays in the game's structure.

    Purely descriptive: it tells the world architect what kind of place to
    build and tells the transition layer how the scene is reached.
    """

    standalone = "standalone"
    hub = "hub"
    mission = "mission"
    chapter = "chapter"
    interior = "interior"


class SceneLinkKind(str, Enum):
    """How the player gets from one environment to another."""

    menu_select = "menu_select"
    trigger_volume = "trigger_volume"
    portal = "portal"
    vehicle = "vehicle"
    story_next = "story_next"


class SceneLink(StrictModel):
    """A one-way transition out of the scene that declares it.

    Links are what make the environment graph explicit instead of implied.
    A star of links is a hub game, a chain is a story, and an arbitrary set
    is a teleport-device game; nothing in the engine assumes a shape.
    """

    to_scene: IdStr
    kind: SceneLinkKind = SceneLinkKind.portal
    # Named spawn point in the TARGET scene to arrive at.
    spawn_point_id: str = Field(default="", max_length=64)
    # Named spawn point in THIS scene to come back to, so returning from a
    # mission or an exhibit puts the player at the door they left through
    # rather than at the world origin.
    return_spawn_point_id: str = Field(default="", max_length=64)


class NestedHost(StrictModel):
    """What a ``nested`` environment is attached to."""

    # The character asset whose body hosts this interior.
    asset_id: IdStr
    # Canonical body slot on the host (see engine BodySlots), e.g. "head".
    anchor_slot: str = Field(default="head", max_length=64)
    # Longest-axis extent, in metres, the interior must fit inside after
    # scaling. A human head is ~0.22 m.
    fit_to_host_extent_m: float = Field(default=0.22, gt=0.0, le=10.0)


class SceneEntry(StrictModel):
    id: IdStr
    name: str = Field(..., max_length=128)
    # Full VR-game scope: scene summaries may include layout, story beats,
    # encounters, lighting mood, audio direction, etc. so the downstream
    # SceneSpec stage has enough authoring detail to compose a real level.
    summary: str = Field(..., max_length=4000)
    entry_point: bool = False
    expected_playtime_minutes: float = Field(
        default=5.0, ge=0.5, le=240.0
    )

    # --- Environment shape (consumed by world generation) ---------------
    kind: SceneKind = SceneKind.standalone
    environment_mode: EnvironmentMode = EnvironmentMode.exclusive
    # Required when environment_mode is nested.
    host: Optional[NestedHost] = None
    # Metres across that the world architect should author at. A nested
    # interior is authored at human scale and scaled down to fit its host,
    # because authoring a walkable level inside a 0.22 m box produces
    # unusable geometry and nav regions.
    authoring_scale_m: float = Field(default=0.0, ge=0.0, le=2000.0)
    links: List[SceneLink] = Field(default_factory=list, max_length=32)
    # False lets several scenes share one generated environment instead of
    # paying for a duplicate; ``reuse_environment_from`` then says which.
    needs_unique_environment: bool = True
    reuse_environment_from: Optional[IdStr] = None


class InteractionRule(StrictModel):
    """A prompt-specified exception to uniform interaction behaviour.

    Equivalent objects behave identically in every environment by default,
    because the affordance binder is a deterministic table applied uniformly.
    This is the only way to make them differ, so an intentional difference is
    always explicit and reviewable: "players cannot pick grass in the hub"
    becomes one row rather than a hope about agent diligence.
    """

    scene_id: IdStr
    # Affordance kind ("harvestable", "door") or an affordance tag
    # ("grabbable") the rule applies to.
    affordance: str = Field(..., max_length=64)
    allowed: bool = False
    notes: str = Field(default="", max_length=400)


class MechanicEntry(StrictModel):
    id: IdStr
    name: str = Field(..., max_length=128)
    # regular = closed input->action loop; hub = shared foundation other
    # mechanics branch off of (e.g. shooting_hub). See MechanicType.
    type: MechanicType = MechanicType.regular
    # Mechanic summaries are the primary brief the MechanicPlan / CodeTask
    # stages consume. Keep them long enough to describe Quest 3 button
    # bindings, edge cases, comfort rules, etc.
    summary: str = Field(..., max_length=4000)
    depends_on_assets: List[IdStr] = Field(default_factory=list)
    required_in_scenes: List[IdStr] = Field(default_factory=list)


class AssetEntry(StrictModel):
    id: IdStr
    name: str = Field(..., max_length=128)
    category: str = Field(..., max_length=32)
    # Asset summaries flow into image prompts + style notes; full-game
    # prompts often include explicit shot direction / mood / material
    # cues so we allow up to 4000 chars here.
    summary: str = Field(..., max_length=4000)


class TestEntry(StrictModel):
    id: IdStr
    description: str = Field(..., max_length=2000)
    automated: bool = False


class BuildSpec(StrictModel):
    """Top-level specification for a generation run.

    Sizing notes (updated for full VR-game generation):

    * ``scenes``  — 1..50. Many prompts describe hub + multiple mission
      / training / arena spaces, so we lift the cap from 6 -> 50.
    * ``mechanics`` — 1..unbounded. A fully detailed VR game can call
      for dozens of distinct mechanics (movement, combat, abilities,
      UI, save / load, NPC interaction, etc.). The planner is
      responsible for keeping the list realistic.
    * ``assets`` — 1..48. Per-game asset budgets are still bounded by
      the Tripo / Gemini cost ceiling; this matches the AssetManifest
      cap.
    * ``acceptance_tests`` — 1..unbounded. The number of checks scales
      with the number of mechanics.
    * ``constraints`` — up to 50 free-form rules (raised from 32).
    """

    prompt_id: IdStr
    title: str = Field(..., max_length=128)
    pitch: str = Field(..., max_length=2000)
    game_type: GameType
    # Which VR device the game is built for. Set from the create-time routing
    # choice (ProjectState.target_device); downstream stages read it to pick
    # the desktop .exe build vs the Quest APK build.
    target_device: TargetDevice = TargetDevice.pcvr
    locomotion: LocomotionStyle = LocomotionStyle.teleport
    comfort: ComfortLevel = ComfortLevel.comfort
    player_height_meters: float = Field(default=1.7, gt=0.0, lt=3.0)

    # Where the player's viewpoint sits, and what their body is. Kept as two
    # separate fields because camera placement and body shape are
    # independent: a full-headed avatar is still viewed from the eyes unless
    # the prompt asks for something stranger.
    player_perspective: PlayerPerspective = PlayerPerspective.first_person_avatar
    player_avatar: PlayerAvatarSpec = Field(default_factory=PlayerAvatarSpec)
    # Hands Only vs Full Player Avatar, mirrored from
    # ProjectState.player_experience (the user's create-time choice).
    player_experience: PlayerExperience = PlayerExperience.full_avatar
    # Locomotion modes, turning, comfort vignette and the settings menu.
    movement: MovementSpec = Field(default_factory=MovementSpec)

    scenes: List[SceneEntry] = Field(..., min_length=1, max_length=50)
    mechanics: List[MechanicEntry] = Field(..., min_length=1)
    assets: List[AssetEntry] = Field(..., min_length=1, max_length=48)

    npc_summary: Optional[str] = Field(default=None, max_length=4000)
    hud_summary: Optional[str] = Field(default=None, max_length=4000)
    constraints: List[str] = Field(default_factory=list, max_length=50)
    acceptance_tests: List[TestEntry] = Field(..., min_length=1)

    # Per-scene exceptions to otherwise-uniform interaction behaviour. Empty
    # is the norm and means "equivalent objects behave the same everywhere".
    interaction_rules: List[InteractionRule] = Field(
        default_factory=list, max_length=64
    )
