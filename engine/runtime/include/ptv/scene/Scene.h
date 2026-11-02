#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>
#include <unordered_map>

#include "ptv/api/InteractiveState.h"
#include "ptv/math/Math.h"

namespace ptv {

using Json = nlohmann::json;

/** What ``socket_generation`` knows about a socket beyond its transform. */
struct SocketInfoSpec {
    /** hand_grip / support_grip / support_wrap / hand_strap are in the OpenXR
     *  grip frame; every other kind faces along +Z. */
    std::string kind;
    /** Hand sockets: ``same`` (either hand uses this frame) or ``flip_z180``
     *  (the left hand is this frame turned half a turn about Z). */
    std::string mirror;
    /** support_wrap: the grip it wraps, and how far to the side of it the
     *  support palm sits (applied toward the free side at runtime). */
    std::string wrap_of;
    float lateral_offset_m{0.0f};
    float bore_diameter_m{0.0f};
    float handle_thickness_m{0.0f};
    std::string confidence;
};

/** ``mesh.hold``: how a held item is solved into one or two hands. */
struct HoldSpec {
    std::string kind{"none"};  // one_hand | two_hand_line | two_hand_wrap | forearm_strap
    bool aim_aligned{false};
    std::string two_hand{"none"};  // none | optional | required
    std::string primary_socket;
    std::string support_socket;
    std::string forearm_socket;
    std::string finger_pose{"wrap"};
    bool has_forward{false};
    Vec3 forward{0.0f, 0.0f, 1.0f};
    Vec3 up{0.0f, 1.0f, 0.0f};
};

struct MeshRef {
    std::string asset_id;
    std::string glb_path;
    float target_scale_meters{1.0f};
    /** Metres per source unit, measured once from LOD0 (see CMesh). 0 means
     *  per-file normalization to ``target_scale_meters``. */
    float unit_scale{0.0f};
    /** Origin-to-sole distance in metres (see CMesh). 0 = unknown. */
    float foot_offset_m{0.0f};
    /** ``mesh.model_frame`` from scene.json (see CMesh::has_model_frame). */
    bool has_model_frame{false};
    float model_yaw_offset_rad{0.0f};
    float model_sole_y{0.0f};
    float model_height{0.0f};
    std::string collision_shape{"aabb"};
    /** Decimated level-of-detail chain produced by the ``asset_optimization``
     * pipeline stage, coarsest last. ``lod_paths[0]`` is the same mesh as
     * ``glb_path``. Empty for assets that were never optimized, in which case
     * the renderer just always draws ``glb_path``. */
    std::vector<std::string> lod_paths;
    /** Triangle count of each entry in ``lod_paths`` (telemetry + budgeting). */
    std::vector<std::uint32_t> lod_triangle_counts;
    /** Simplification error of each LOD relative to the mesh's extent
     *  (meshoptimizer), for projected-error LOD selection. Empty = unknown:
     *  the renderer switches by distance instead. */
    std::vector<float> lod_errors_rel;
    /** World-space bounding-sphere radius (metres) of the scaled mesh. Used
     * for frustum + distance culling. 0 means "unknown": the renderer then
     * falls back to a conservative radius so nothing is wrongly culled. */
    float bounds_radius_m{0.0f};
    /** Named attachment points in the mesh's own local space, already in
     * METRES at ``target_scale_meters`` (the scene compiler converts them
     * from the unit-space values in ``asset_contract.json``, exactly as it
     * does for ``bounds_radius_m``).
     *
     * This is what lets one weapon-agnostic generated mechanic fire from the
     * barrel tip of a pistol, a rifle or a rocket launcher: the C++ asks for
     * ``"muzzle"``, and the per-asset numbers arrive as data. */
    std::unordered_map<std::string, Transform> sockets;
    /** Kind, mirror rule and measured sizes per socket (socket_generation). */
    std::unordered_map<std::string, SocketInfoSpec> socket_info;
    std::optional<HoldSpec> hold;
    /** True when the asset carries a skin, set by the scene compiler from the
     * optimizer's report. Lets the runtime decide to attach a ``CSkeleton``
     * and draw through the skinned pipeline without opening every GLB in the
     * scene to find out. */
    bool skinned{false};
    int joint_count{0};
    /** Semantic animation intent ("walk", "attack") -> the glTF clip name
     * actually present in the GLB.
     *
     * Tripo's retargeter names the clips, not this pipeline, so without this
     * map generated C++ would have to guess. It is the exact same contract
     * as ``sockets``: ask by a name fixed in advance, let per-asset data
     * supply the specifics. */
    std::unordered_map<std::string, std::string> animations;
    /** Locomotion intent -> the ground speed, in m/s, the clip looks like it
     * is travelling at. Derived by the scene compiler from the character's
     * height, because a clip retargeted with ``animate_in_place`` has no root
     * motion to measure. Used to match playback rate to distance covered. */
    std::unordered_map<std::string, float> animation_speeds;
};

// Procedural primitive authored by the world-generation stage.
struct PrimitiveRef {
    std::string kind{"box"};  // box | plane | cylinder | sphere | ramp
    Vec3 size{1.0f, 1.0f, 1.0f};
    Vec3 color{0.8f, 0.8f, 0.82f};
};

// Arbitrary procedural triangle mesh authored by the world-generation stage.
struct InlineMeshRef {
    std::vector<float> positions;
    std::vector<float> normals;
    std::vector<std::uint32_t> indices;
    Vec3 color{0.8f, 0.8f, 0.82f};
};

struct PhysicsProps {
    std::string kind{"static"};
    float mass_kg{1.0f};
    std::string collision_shape{"aabb"};
    float restitution{0.2f};
    float friction{0.5f};
};

/** Declared changeable state for one world object.
 *
 * Parsed as-is from the world document; ``ApplySceneToWorld`` matches ``id``
 * against an entity's own id and attaches a ``CInteractive``. Keeping them
 * separate in the document rather than nesting state inside the entity is
 * what lets a validator check the interactive list on its own -- for a
 * missing geometry match, a duplicate id, or a nav gate naming something
 * that was never declared. */
struct InteractiveSpec {
    std::string id;
    std::string kind;
    std::unordered_map<std::string, InteractiveValue> initial_state;
    bool locked{false};
    std::vector<std::string> tags;
    Vec3 hinge_point{0.0f};
    Vec3 hinge_axis{0.0f, 1.0f, 0.0f};
    float min_angle_deg{0.0f};
    float max_angle_deg{90.0f};
    Vec3 slide_axis{0.0f};
    float slide_distance_m{0.0f};
    std::vector<std::string> positions;
    std::string yields_asset_id;
};

/** A named place to arrive at in this scene.
 *
 * Arrival has to be named rather than positional because the sensible place to
 * stand depends on where you came FROM: returning to a hub from a mission
 * belongs at the mission's own door, not at the hub's front gate. The author
 * knows that and declares it; the engine only looks it up. */
struct SpawnPointSpec {
    std::string id;
    Vec3 position{0.0f};
    Vec3 facing{0.0f, 0.0f, -1.0f};
};

/** A way out of this scene and into another.
 *
 * Declared as a volume plus a target rather than as an entity with a mechanic,
 * because a portal is not something the player picks up or breaks -- it is a
 * region of space with a meaning, which is the same shape as a teleport target
 * and is handled in the same place. */
struct PortalSpec {
    std::string id;
    std::string to_scene;
    std::string target_spawn_id;
    Vec3 position{0.0f};
    /** Full extents of the trigger volume. */
    Vec3 size{1.0f, 2.0f, 0.5f};
    /** When true the player must deliberately act while inside the volume.
     *  Walking through a doorway you did not mean to take, and losing the room
     *  you were in, is the failure this prevents; an author opts out only for
     *  a portal that IS the intended path. */
    bool requires_interaction{true};
};

/** Walkable topology declared by the world builder, straight from
 *  ``world.scene.json``. Ids are resolved into graph indices at load. */
struct NavRegionSpec {
    std::string id;
    Vec3 min{0.0f};
    Vec3 max{0.0f};
    float floor_y{0.0f};
};

struct NavLinkSpec {
    std::string from;
    std::string to;
    Vec3 point{0.0f};
    float cost_multiplier{1.0f};
    bool bidirectional{true};
};

struct HealthProps {
    float max{100.0f};
    float invuln_after_hit_s{0.4f};
};

struct FactionProps {
    std::string id{"neutral"};
    std::vector<std::string> hostile_to;
};

/** Authored parenting: this entity rides a point on another one. */
struct AttachmentRef {
    std::string entity_id;
    std::string anchor{"grip"};
    Vec3 position{0.0f};
    Quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    /** Optional socket on the attached item to seat on ``anchor``. */
    std::string socket;
};

struct MechanicBinding {
    std::string id;                 // "builtin.grab" or "generated.<id>"
    std::string builtin_id;         // when present
    Json params{Json::object()};
};

/** One place something can be equipped on this entity, as authored.
 *
 * The document-side mirror of ``ptv::EquipSlot`` (ptv/ecs/World.h), kept
 * separate for the same reason ``HealthProps`` is separate from ``CHealth``:
 * the scene document is plain data that the offline tools read, and it must
 * not drag the ECS (and entt) in behind it. ``SceneLoader`` converts.
 *
 * A holster is declared on the WEARER, never on the holster prop -- a slot
 * is a place ON a body, while a socket is a point ON an item. */
struct EquipSlotSpec {
    std::string name;
    std::string body_slot;
    std::string host_item_slot;
    std::string host_socket;
    Vec3 local_position{0.0f};
    Quat local_rotation{1.0f, 0.0f, 0.0f, 0.0f};
    float snap_radius_m{0.0f};
    bool exclusive{true};
    std::vector<std::string> accepts_tags;
};

struct EntitySpec {
    std::string id;
    std::string name;
    Transform transform{};
    std::optional<MeshRef> mesh;
    std::optional<PrimitiveRef> primitive;
    std::optional<InlineMeshRef> mesh_inline;
    std::optional<PhysicsProps> physics;
    std::vector<MechanicBinding> bindings;
    std::vector<std::string> tags;
    std::optional<HealthProps> health;
    std::optional<FactionProps> faction;
    std::optional<AttachmentRef> attached_to;
    /** Places items may be equipped ON THIS ENTITY, straight from the scene
     *  document. Populated into ``CEquipSlots`` at load. A wearer with none
     *  offers ``builtin.equip`` no candidate slot at all, so this is what
     *  makes an authored holster work. */
    std::vector<EquipSlotSpec> equip_slots;
    /** The compiler found nothing that moves, is held or is read per frame:
     *  the renderer may merge it with its neighbours into one draw. */
    bool static_batch{false};
};

struct LightSpec {
    std::string id;
    std::string kind{"directional"};
    Transform transform{};
    Vec3 color{1.0f, 1.0f, 1.0f};
    float intensity{1.0f};
    Vec3 direction{0.0f, -1.0f, 0.0f};
    // Point-light falloff radius in meters (author-controlled). <= 0 means
    // "use the renderer's default". Ignored for directional lights.
    float range{0.0f};
};

// Optional gradient sky authored by the world-generation stage. When
// ``present`` is false the runtime falls back to the flat ``sky_color`` clear.
struct SkyGradient {
    bool present{false};
    Vec3 top{0.05f, 0.07f, 0.15f};
    Vec3 horizon{0.45f, 0.55f, 0.75f};
    Vec3 sun_direction{0.30f, -0.82f, 0.30f};
    Vec3 sun_color{1.0f, 0.97f, 0.9f};
    float sun_intensity{1.0f};
    // Global environment/atmosphere, all author-controlled (see IWorldBuilder
    // SkyDesc). ambient_color = lower-hemisphere (ground bounce); the upper
    // hemisphere blends toward ``top``.
    Vec3 ambient_color{0.20f, 0.21f, 0.24f};
    float ambient_intensity{0.30f};
    float fog_density{0.012f};
    float exposure{1.0f};
};

struct PlayerSpawn {
    Vec3 position{0.0f, 0.0f, 0.0f};
    Quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
};

/** Device rendering budget + quality policy for this scene.
 *
 * Emitted by the ``asset_optimization`` pipeline stage (see
 * ``ptv_schemas.performance``), which sizes it from the target device and how
 * much GLB content the project has. Defaults below are the conservative
 * "small scene on Quest" values used when a scene predates the stage.
 *
 * The philosophy: nothing here is a hard content limit. A compact game gets an
 * effectively infinite cull distance and full detail; a massive one gets
 * tighter LOD distances and foveation. The runtime governor then trims further
 * only if the measured frame time actually demands it.
 */
struct ScenePerformance {
    std::string tier{"standard"};
    int target_fps{72};
    std::uint32_t frame_triangle_budget{1'100'000};
    std::uint32_t resident_triangle_budget{2'400'000};
    std::uint32_t draw_call_budget{300};
    int msaa_samples{4};
    /** Fixed-foveated-rendering level: 0 = off .. 4 = top. */
    int foveation_level{0};
    /** Entities beyond this are not drawn. Huge (1e5) means "no distance
     * culling", which is what small scenes get so the player still sees the
     * whole world. */
    float cull_distance_m{100000.0f};
    /** Distance at which LOD1, LOD2, LOD3 take over. */
    std::vector<float> lod_switch_distances_m{12.0f, 30.0f, 65.0f};
    bool evict_when_over_budget{false};

    // --- Animated agents ---
    //
    // Posing skeletons and running behaviours is CPU work that competes with
    // the render thread, so characters get their own budget: a scene can be
    // well inside its triangle budget and still miss frame rate because
    // thirty skeletons are being evaluated every frame.
    /** Ceiling on simultaneously animated characters. */
    int max_animated_agents{12};
    /** [full rate, half rate, stop] distances for pose evaluation. */
    std::vector<float> animation_lod_distances_m{8.0f, 15.0f, 40.0f};
    /** Behaviour update rate at those same three bands, in Hz. */
    std::vector<float> ai_tick_rates_hz{30.0f, 10.0f, 3.0f};

    // --- Weapons and effects ---
    //
    // Pools, not limits on gameplay: a full pool recycles its oldest member,
    // so an automatic weapon cannot spend a Quest's frame on projectiles.
    int max_live_projectiles{64};
    /** Projectiles drawn as meshes at once (arrows, rockets); the rest of a
     *  full pool draws as tracers. */
    int max_projectile_meshes{24};
    int max_fx_quads{1024};
    int max_beams{4};
    int max_decals{48};
    /** Short point lights from muzzle flashes and explosions. */
    int max_flash_lights{2};

    // --- Adaptive governor bounds ---
    bool governor_enabled{true};
    float governor_max_lod_bias{2.0f};
    float governor_min_cull_distance_m{25.0f};
    int governor_max_foveation_level{3};
    int governor_escalate_after_bad_frames{30};
    int governor_relax_after_good_frames{180};

    // --- LOD and culling by projected size ---
    /** Switch to a coarser LOD once its simplification error projects to
     *  fewer pixels than this (per eye, at the eye buffer's resolution). */
    float lod_screen_error_px{1.0f};
    /** Fraction the error must clear before switching back to finer detail. */
    float lod_hysteresis{0.2f};
    /** Rendering-only cull for objects smaller than this on screen. */
    float small_object_cull_px{1.5f};

    // --- Resolution and refresh ---
    /** Fraction of the runtime-recommended eye resolution the swapchain is
     *  allocated at (1.0 = 1680x1760 on Quest 3; up to 1.25 on request). */
    float render_scale_ceiling{1.0f};
    /** Lowest fraction dynamic resolution may submit (imageRect). */
    float render_scale_floor{0.85f};
    int refresh_hz{72};

    // --- Internal toggles (engineering switches, not user settings) ---
    bool multiview{true};
    bool ffr_fdm{true};
    int frames_in_flight{2};
    bool static_batching{true};
    bool instancing{true};
    /** Room/portal culling from the world's nav regions and links. */
    bool room_culling{false};
    /** Nav regions the compiler found closed by walls on all four sides;
     *  only their contents may be hidden from a player elsewhere. */
    std::vector<std::string> rooms;
    /** Grid cell edge for merging static geometry into one draw. */
    float static_batch_cell_m{8.0f};

    // --- Memory (Phase 13; Meta's Quest 3 limit is 5.75 GiB PSS) ---
    float memory_target_mb{4608.0f};
    float memory_ceiling_mb{5120.0f};
    float memory_limit_mb{5888.0f};
    float texture_memory_budget_mb{320.0f};
    /** Streamable meshes + textures resident at once (the memory model's
     *  target minus the always-resident set and transients). */
    float resident_memory_budget_mb{2048.0f};

    // --- User locks ("Custom Quest Optimizations"). The governor never
    //     moves a locked knob; "auto" means unlocked. ---
    std::string lock_ffr{"auto"};        // auto | off | fixed | max
    int lock_ffr_level{-1};              // fixed / max level (1..3)
    std::string lock_resolution{"auto"}; // auto | off | high
    std::string lock_lod{"auto"};        // auto | detail | performance
    std::string lock_draw_distance{"auto"}; // auto | far | performance
    std::string lock_room_culling{"auto"};  // auto | off
};

/** One designed projectile (``scene.json`` ``projectiles``), so code can
 *  launch "the rocket" by id with the design's flight and look. */
struct ProjectileTableEntry {
    std::string id;
    std::string kind;            // bullet | rocket | arrow | energy_bolt | grenade | other
    std::string template_entity; // hidden mesh template, empty = tracer
    float speed_mps{360.0f};
    float gravity_scale{1.0f};
    float drag{0.0f};
    float radius_m{0.0f};
    float lifetime_s{3.0f};
    std::string on_hit{"despawn"};
    float splash_radius_m{0.0f};
    float splash_damage{0.0f};
    float diameter_m{0.009f};
    float length_m{0.015f};
};

struct SceneDoc {
    int version{0};
    std::string prompt_id;
    std::string scene_id;
    std::string title;
    std::string name;
    float player_height_meters{1.7f};
    std::string locomotion{"teleport"};
    std::string comfort{"comfort"};
    PlayerSpawn spawn{};
    /** Legacy field, parsed so old scenes still load. The engine no longer
     *  shows a movement picker of its own: a game that wants one binds
     *  ``builtin.movement_settings_menu``. */
    bool locomotion_menu_enabled{false};
    /** Which locomotion mode is active at start. One of ``teleport``,
     *  ``slide_head_relative``, ``slide_controller_relative``. When the scene
     *  binds only one mode that one is used regardless. */
    std::string default_locomotion_mode{};
    /** ``snap`` or ``smooth``: which turn mode owns the right stick at start
     *  when the scene binds both. */
    std::string default_turn_mode{"snap"};
    /** Where the player's visible hands come from:
     *    * ``default`` -- the engine's rigged default hands (Hands Only);
     *    * ``avatar``  -- the player avatar's own hands (Full Player Avatar),
     *      so the engine creates only invisible hand anchors.
     */
    std::string hands_source{"default"};
    /** Longest-axis size of the default hand mesh (wrist stub to fingertip),
     *  metres. 0.27 draws it as large as a Full Player Avatar's hands: the
     *  goblin-shooter avatar's wrist -> middle knuckle is 0.168 m, the default
     *  hand's 0.128 m at 0.20. */
    float hands_scale_m{0.27f};
    /** Extra rotation (XYZ Euler, degrees) applied in the grip frame on top
     *  of the measured palm alignment, for tuning how a hand sits on a
     *  particular controller. Also applied to an avatar's wrists. */
    Vec3 hands_grip_trim_deg{0.0f};
    /** Draw the grip frame and the hand's palm frame, for calibrating the
     *  alignment on a headset. */
    bool hands_debug_axes{false};
    /** Tunables for Application.cpp (override hardcoded defaults). */
    float slide_max_speed_mps{1.5f};
    float slide_accel_tau_s{0.12f};
    float slide_decel_tau_s{0.18f};
    /** Sprint on the slide (hold the left stick in while pushing forward).
     *  ``sprint_activation`` is "hold", "toggle" or "off". */
    float sprint_speed_mps{3.0f};
    std::string sprint_activation{"hold"};
    float sprint_accel_tau_s{0.35f};
    float sprint_decel_tau_s{0.25f};
    float sprint_forward_cone_deg{70.0f};
    float sprint_min_stick{0.6f};
    float teleport_launch_speed_mps{6.0f};
    float teleport_max_horizontal_m{12.0f};
    /** Y plane used for teleport arc landing tests (meters). */
    float teleport_floor_y{0.0f};
    float snap_turn_degrees{45.0f};
    float snap_turn_cooldown_s{0.30f};
    Vec3 sky_color{0.05f, 0.07f, 0.11f};
    float ambient_intensity{0.25f};
    SkyGradient sky{};
    ScenePerformance performance{};
    std::vector<ProjectileTableEntry> projectiles;
    std::vector<LightSpec> lights;
    std::vector<EntitySpec> entities;
    /** Empty when the world builder declared no topology, in which case
     *  navigation degrades to local steering. */
    std::vector<NavRegionSpec> nav_regions;
    std::vector<NavLinkSpec> nav_links;
    /** Declared changeable state, straight from the world builder's
     *  ``interactives`` block. Bound to entities by matching id at load, so
     *  the geometry and the state that describes it stay one object. */
    std::vector<InteractiveSpec> interactives;

    /** Named arrival points, and the ways out of this scene. */
    std::vector<SpawnPointSpec> spawn_points;
    std::vector<PortalSpec> portals;

    /** scene_id -> the file that scene compiled to, as a path RELATIVE to
     *  this document.
     *
     *  The runtime needs to load a sibling scene without knowing how the
     *  orchestrator lays out a project, and it has to work identically inside
     *  a Quest APK, where there is no filesystem to go looking around in. So
     *  the compiler writes down where everything went and the engine just
     *  reads it. A portal naming a scene that is not in here cannot be taken,
     *  and says so once rather than failing silently. */
    std::unordered_map<std::string, std::string> scene_index;
};

SceneDoc LoadSceneJson(const std::string& path);

}  // namespace ptv
