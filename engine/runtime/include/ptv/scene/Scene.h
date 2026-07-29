#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>
#include <unordered_map>

#include "ptv/math/Math.h"

namespace ptv {

using Json = nlohmann::json;

struct MeshRef {
    std::string asset_id;
    std::string glb_path;
    float target_scale_meters{1.0f};
    std::string collision_shape{"aabb"};
    /** Decimated level-of-detail chain produced by the ``asset_optimization``
     * pipeline stage, coarsest last. ``lod_paths[0]`` is the same mesh as
     * ``glb_path``. Empty for assets that were never optimized, in which case
     * the renderer just always draws ``glb_path``. */
    std::vector<std::string> lod_paths;
    /** Triangle count of each entry in ``lod_paths`` (telemetry + budgeting). */
    std::vector<std::uint32_t> lod_triangle_counts;
    /** World-space bounding-sphere radius (metres) of the scaled mesh. Used
     * for frustum + distance culling. 0 means "unknown": the renderer then
     * falls back to a conservative radius so nothing is wrongly culled. */
    float bounds_radius_m{0.0f};
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

struct MechanicBinding {
    std::string id;                 // "builtin.grab" or "generated.<id>"
    std::string builtin_id;         // when present
    Json params{Json::object()};
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

    // --- Adaptive governor bounds ---
    bool governor_enabled{true};
    float governor_max_lod_bias{2.0f};
    float governor_min_cull_distance_m{25.0f};
    int governor_max_foveation_level{3};
    int governor_escalate_after_bad_frames{30};
    int governor_relax_after_good_frames{180};
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
    /** When true (default), Application shows the Meta-style movement picker
     * on first run. When false, ``default_locomotion_mode`` is used and the
     * menu is skipped (arcade / seated titles). */
    bool locomotion_menu_enabled{true};
    /** Used only when ``locomotion_menu_enabled`` is false. One of:
     * ``teleport``, ``slide_head_relative``, ``slide_controller_relative``. */
    std::string default_locomotion_mode{};
    /** Tunables for Application.cpp (override hardcoded defaults). */
    float slide_max_speed_mps{1.5f};
    float slide_accel_tau_s{0.12f};
    float slide_decel_tau_s{0.18f};
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
    std::vector<LightSpec> lights;
    std::vector<EntitySpec> entities;
};

SceneDoc LoadSceneJson(const std::string& path);

}  // namespace ptv
