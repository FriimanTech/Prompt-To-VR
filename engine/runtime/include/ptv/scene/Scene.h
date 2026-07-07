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
};

struct PlayerSpawn {
    Vec3 position{0.0f, 0.0f, 0.0f};
    Quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
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
    std::vector<LightSpec> lights;
    std::vector<EntitySpec> entities;
};

SceneDoc LoadSceneJson(const std::string& path);

}  // namespace ptv
