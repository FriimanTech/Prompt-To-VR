#include "flight_system_shared.h"

#include <algorithm>
#include <string>
#include <vector>

namespace ptv {

FlightParams fs_parse_params(const nlohmann::json& p) {
    FlightParams cfg;
    if (!p.is_object()) return cfg;
    cfg.cruise_max_speed_mps = p.value("cruise_max_speed_mps", cfg.cruise_max_speed_mps);
    cfg.boost_multiplier     = p.value("boost_multiplier", cfg.boost_multiplier);
    cfg.accel_tau_s          = p.value("accel_tau_s", cfg.accel_tau_s);
    cfg.decel_tau_s          = p.value("decel_tau_s", cfg.decel_tau_s);
    cfg.altitude_rate_mps    = p.value("altitude_rate_mps", cfg.altitude_rate_mps);
    cfg.bank_assist          = p.value("bank_assist", cfg.bank_assist);
    cfg.comfort_vignette_on_boost = p.value("comfort_vignette_on_boost", cfg.comfort_vignette_on_boost);
    cfg.oob_return_delay_s   = p.value("oob_return_delay_s", cfg.oob_return_delay_s);
    cfg.collision_soft_bounce_coeff = p.value("collision_soft_bounce_coeff", cfg.collision_soft_bounce_coeff);
    cfg.no_fly_volumes_tag   = p.value("no_fly_volumes_tag", cfg.no_fly_volumes_tag);
    cfg.bounds_radius_m      = p.value("bounds_radius_m", cfg.bounds_radius_m);
    return cfg;
}

static std::optional<nlohmann::json> fs_find_own_params_json(World& w, std::uint32_t e, const std::string& my_mech_id) {
    if (!w.has<CMechanicBinding>(e)) return std::nullopt;
    auto& entries = w.get<CMechanicBinding>(e).entries;
    for (const auto& entry : entries) {
        if (entry.id == my_mech_id || entry.builtin_id == my_mech_id) {
            auto j = nlohmann::json::parse(entry.params_json, nullptr, /*allow_exceptions=*/false);
            if (j.is_object()) return j;
        }
    }
    return std::nullopt;
}

FlightInputs fs_read_inputs_from_binding(World& w, std::uint32_t e, const std::string& my_mech_id) {
    FlightInputs in;
    auto jp = fs_find_own_params_json(w, e, my_mech_id);
    if (!jp.has_value()) return in;
    const auto& j = *jp;

    // Left stick
    if (j.contains("l_stick_x") && j.contains("l_stick_y")) {
        in.left_stick.x = j.value("l_stick_x", 0.0f);
        in.left_stick.y = j.value("l_stick_y", 0.0f);
    }
    in.left_stick_click = j.value("l_click_held", false);

    // Right vertical
    in.right_stick_vert = j.value("r_vert", 0.0f);

    // Buttons
    in.a_double_tap = j.value("a_double_tap", false);
    in.b_tap = j.value("b_tap", false);

    // Frame & stubs
    in.ref_frame = j.value("frame", std::string("head"));
    in.in_no_fly = j.value("in_no_fly", false);

    if (j.contains("debug_hit") && j.value("debug_hit", false)) {
        in.debug_hit = true;
        if (j.contains("debug_hit_normal") && j["debug_hit_normal"].is_array() && j["debug_hit_normal"].size() == 3) {
            in.debug_hit_normal.x = j["debug_hit_normal"][0].get<float>();
            in.debug_hit_normal.y = j["debug_hit_normal"][1].get<float>();
            in.debug_hit_normal.z = j["debug_hit_normal"][2].get<float>();
        }
    }

    return in;
}

void fs_step_controls(const FlightParams& cfg, const FlightInputs& in, float dt, FlightStateInternal& st) {
    (void)cfg; (void)dt;

    // Toggle/Brake handling is decided in physics step with access to transform; here mark intents
    st.boost_held = in.left_stick_click;

    // Compute target velocity in a simple world-aligned frame (XZ from left stick);
    // reference frame selection stub: if controller-relative was required, another system would rotate this
    // vector. Here we keep it world-aligned for stability (head/controller-relative resolved upstream).
    glm::vec3 target{in.left_stick.x, 0.0f, in.left_stick.y};

    float speed = glm::length(glm::vec2(target.x, target.z));
    if (speed > 1e-5f) target = glm::normalize(target);

    float max_spd = cfg.cruise_max_speed_mps * (st.boost_held ? cfg.boost_multiplier : 1.0f);

    target *= max_spd;

    // Altitude assist via right stick vertical
    target.y += cfg.altitude_rate_mps * std::clamp(in.right_stick_vert, -1.0f, 1.0f);

    st.target_velocity = target;

    // Quick evade: lateral impulse to +X if unknown; nudge instantaneous velocity
    if (in.b_tap) {
        glm::vec3 lateral{1.0f, 0.0f, 0.0f};
        // If we had a recent incoming dir (towards us), push opposite that projected on XZ
        glm::vec3 inc = st.recent_incoming_dir;
        inc.y = 0.0f;
        if (glm::dot(inc, inc) > 1e-4f) {
            lateral = -glm::normalize(inc);
        }
        st.velocity += lateral * 3.0f; // small burst
    }
}

} // namespace ptv
