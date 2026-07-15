#pragma once

#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"
#include <nlohmann/json.hpp>
#include <glm/glm.hpp>
#include <string>
#include <cstdint>
#include <optional>

namespace ptv {

struct HoverConfig {
    float double_tap_ms = 350.0f;
    float hover_max_speed_mps = 2.0f;
    float accel_tau_s = 0.18f;   // default a bit soft for comfort
    float decel_tau_s = 0.22f;
    bool right_stick_vertical = true;
    float altitude_rate_mps = 1.0f;
    float fine_altitude_rate_mps = 0.3f;
    float brake_hold_s = 1.5f;
    float bank_max_deg = 8.0f;
    float bob_amp_m = 0.02f;
    float bob_period_s = 3.5f;   // subtle and slow
    float ring_drain_per_s = 0.5f;
    float emergency_hold_s = 2.0f;
    std::string toggle_button = "A_right";
    std::string brake_button = "B_right";
    bool left_stick_move = true;
    bool left_trigger_fine_alt = true;
    // Turning
    bool use_snap_turn = false;
    float snap_turn_deg = 45.0f;
    float smooth_turn_rate_dps = 120.0f; // multiplied by right stick X
    // Orientation reference: "head" or "controller"
    std::string locomotion_frame = "head";
    // Altitude clamps (relative to starting altitude if not provided by world)
    float min_altitude_m = 0.1f;
    float max_altitude_m = 30.0f;
};

struct HoverRuntime {
    bool active = false;
    float t_now_s = 0.0f;
    float last_toggle_tap_s = -1000.0f;
    float brake_hold_timer_s = 0.0f;

    // Kinematics
    glm::vec3 vel{0.0f};
    glm::vec3 target_forward{0.0f, 0.0f, 1.0f};

    // Altitude
    float base_altitude_m = 1.7f;
    float target_altitude_m = 1.7f;

    // Bob
    float bob_phase_s = 0.0f;

    // Energy (emulated if real system unavailable)
    float ring_energy = 100.0f;
    float emergency_threshold = 8.0f;
    float emergency_hold_timer_s = 0.0f;

    // UX
    float no_fly_cooldown_s = 0.0f; // gate tooltip spam if implemented elsewhere
};

// Helpers shared by implementation

// Returns movement input vector (x right, y forward) from parsed binding json.
glm::vec2 read_left_stick(const nlohmann::json& jb);

// Returns right stick vector (x yaw, y vertical if enabled).
glm::vec2 read_right_stick(const nlohmann::json& jb);

// Returns analog [0..1] for left trigger if present.
float read_left_trigger_analog(const nlohmann::json& jb);

// True if the named input posted a tap this frame (edge-driven by harness)
bool read_button_tap(const nlohmann::json& jb, const std::string& logical_name);

// Compute a leveled orientation slerped toward removing pitch/roll (bank clamp in degrees)
ptv::Quat stabilize_level(const ptv::Quat& current, float max_bank_deg, float dt);

// Apply first-order response toward a target velocity (tau seconds), clamped to max_speed.
glm::vec3 first_order_velocity(glm::vec3 v, glm::vec3 v_target, float tau_s, float max_speed, float dt);

// Find this mechanic's binding entry json (params_json) if present on entity; empty object otherwise.
// Note: CMechanicBinding::Entry::params_json is a string; we parse to json here.
nlohmann::json find_self_binding_params(World& world, std::uint32_t entity, const std::string& self_id);

// Simple distance check for nearby no-fly volumes by tag; returns true if within radius of any tagged entity.
bool near_no_fly_volume(World& world, const ptv::Vec3& position, float radius_m);

// Yaw-only quaternion facing given forward (projected on XZ), world up=+Y. Returns identity if forward too small.
ptv::Quat yaw_from_forward(const glm::vec3& fwd);

} // namespace ptv
