#pragma once

#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <glm/glm.hpp>
#include <string>
#include <optional>

namespace ptv {

// Public symbol for downstream reference (do not register here)
class FlightSystemMechanic;

struct FlightParams {
    float cruise_max_speed_mps = 6.0f;
    float boost_multiplier = 1.6f;
    float accel_tau_s = 0.20f;
    float decel_tau_s = 0.25f;
    float altitude_rate_mps = 2.0f;
    bool  bank_assist = true;
    bool  comfort_vignette_on_boost = true;
    float oob_return_delay_s = 1.0f;
    float collision_soft_bounce_coeff = 0.35f;
    std::string no_fly_volumes_tag = "no_fly";
    float bounds_radius_m = 300.0f; // simple spherical OOB guard
};

enum class FlightMode : std::uint8_t { Hover = 0, Flight = 1, Returning = 2 };

struct FlightStateInternal {
    FlightMode mode = FlightMode::Hover;
    glm::vec3 velocity{0.0f};        // m/s
    glm::vec3 target_velocity{0.0f};
    bool boost_held = false;
    bool last_boost_held = false;
    bool brake_intent = false;

    // Out-of-bounds return
    bool oob_active = false;
    float oob_t = 0.0f;
    glm::vec3 last_safe_pos{0.0f, 1.7f, 0.0f};
    glm::vec3 oob_start_pos{0.0f};

    // Recent threat/evasion direction (world-space). If unknown, use +X (HMD right surrogate)
    glm::vec3 recent_incoming_dir{-1.0f, 0.0f, 0.0f};
};

// Combined per-frame inputs read from this entity's CMechanicBinding entry
struct FlightInputs {
    // Left stick thrust in XZ plane; y used iff caller supplies hover ascent/descend bias
    glm::vec2 left_stick{0.0f};
    bool left_stick_click = false;   // boost modifier when held
    float right_stick_vert = 0.0f;   // altitude assist scaler (-1..1)
    bool a_double_tap = false;       // toggle hover/flight or brake-to-hover
    bool b_tap = false;              // quick evade burst
    // Frame selection: "head" or "controller" (fallback head)
    std::string ref_frame = "head";

    // Optional stubs supplied by harness for testing
    bool in_no_fly = false;          // if true, deny entering flight
    bool debug_hit = false;          // if true, treat as soft collision this frame
    glm::vec3 debug_hit_normal{0.0f, 1.0f, 0.0f}; // world-space normal
};

// Helpers implemented in .cpp files
FlightParams fs_parse_params(const nlohmann::json& p);
FlightInputs fs_read_inputs_from_binding(World& w, std::uint32_t e, const std::string& my_mech_id);
void fs_publish_tag(World& w, std::uint32_t e, const std::string& tag);
void fs_publish_locomotion_velocity(World& w, std::uint32_t e, const glm::vec3& v);
void fs_publish_hover_enable(World& w, std::uint32_t e, bool on);
void fs_publish_aim_stabilizer(World& w, std::uint32_t e, bool enabled, float strength01);
void fs_publish_vignette(World& w, std::uint32_t e, float strength01);
void fs_publish_haptics(World& w, std::uint32_t e, const std::string& pattern_id, const std::string& hand, float amp, float dur_s);

// Physics & comfort steps
void fs_step_controls(const FlightParams& cfg, const FlightInputs& in, float dt, FlightStateInternal& st);
void fs_step_physics(const FlightParams& cfg, const FlightInputs& in, float dt, World& w, std::uint32_t e, FlightStateInternal& st);
void fs_step_comfort(const FlightParams& cfg, const FlightInputs& in, float dt, World& w, std::uint32_t e, FlightStateInternal& st);

// Small math helpers
inline float fs_exp_smooth(float current, float target, float tau_s, float dt) {
    if (tau_s <= 0.0f) return target;
    float a = 1.0f - std::exp(-dt / tau_s);
    return current + (target - current) * a;
}

inline glm::vec3 fs_exp_smooth_vec3(const glm::vec3& c, const glm::vec3& t, float tau_s, float dt) {
    return glm::vec3(
        fs_exp_smooth(c.x, t.x, tau_s, dt),
        fs_exp_smooth(c.y, t.y, tau_s, dt),
        fs_exp_smooth(c.z, t.z, tau_s, dt)
    );
}

} // namespace ptv
