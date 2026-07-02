#include "flight_system_shared.h"

#include <algorithm>
#include <cmath>

namespace ptv {

static glm::vec3 fs_get_position(const World& w, std::uint32_t e) {
    if (!w.has<CTransform>(e)) return glm::vec3(0.0f, 1.7f, 0.0f);
    const auto& tr = w.get<CTransform>(e).t;
    return glm::vec3(tr.position.x, tr.position.y, tr.position.z);
}

static void fs_set_position(World& w, std::uint32_t e, const glm::vec3& p) {
    if (!w.has<CTransform>(e)) return;
    auto& tr = w.get<CTransform>(e).t;
    tr.position.x = p.x; tr.position.y = p.y; tr.position.z = p.z;
}

static void fs_apply_soft_collision(const FlightParams& cfg, const FlightInputs& in, FlightStateInternal& st) {
    if (!in.debug_hit) return; // stubbed; real cast would go here
    glm::vec3 n = in.debug_hit_normal;
    float nlen2 = glm::dot(n, n);
    if (nlen2 < 1e-6f) return;
    n = glm::normalize(n);
    float vn = glm::dot(st.velocity, n);
    if (vn < 0.0f) {
        // Reflect and dampen using soft bounce coeff
        st.velocity = st.velocity - (1.0f + cfg.collision_soft_bounce_coeff) * vn * n;
    }
}

static void fs_check_oob_and_handle(const FlightParams& cfg, float dt, World& w, std::uint32_t e, FlightStateInternal& st) {
    glm::vec3 p = fs_get_position(w, e);

    if (st.mode != FlightMode::Returning) {
        float r = glm::length(p);
        if (r <= cfg.bounds_radius_m * 0.98f) {
            st.last_safe_pos = p; // update safe pos while comfortably inside
        }
        if (r > cfg.bounds_radius_m) {
            // Start auto-return
            st.mode = FlightMode::Returning;
            st.oob_active = true;
            st.oob_t = 0.0f;
            st.oob_start_pos = p;
            st.velocity = glm::vec3(0.0f);
            // HUD warning tag (consumed by HUD if present)
            fs_publish_tag(w, e, std::string("hud.warn:{\"text\":\"Returning to bounds...\",\"severity\":\"info\"}"));
            // Immediately advertise hover intent so other systems can prep stabilization
            fs_publish_hover_enable(w, e, true);
        }
    }

    if (st.mode == FlightMode::Returning) {
        st.oob_t += dt;
        float t = std::clamp(st.oob_t / std::max(0.001f, cfg.oob_return_delay_s), 0.0f, 1.0f);
        glm::vec3 pnew = glm::mix(st.oob_start_pos, st.last_safe_pos, t);
        fs_set_position(w, e, pnew);
        fs_publish_locomotion_velocity(w, e, glm::vec3(0.0f));
        if (t >= 1.0f) {
            st.mode = FlightMode::Hover;
            st.oob_active = false;
            // Land in hover with zero velocity
            fs_publish_hover_enable(w, e, true);
        }
    }
}

void fs_step_physics(const FlightParams& cfg, const FlightInputs& in, float dt, World& w, std::uint32_t e, FlightStateInternal& st) {
    // Check OOB return first; if active, override motion
    fs_check_oob_and_handle(cfg, dt, w, e, st);
    if (st.mode == FlightMode::Returning) return;

    // Enter/exit flight via A double-tap (deny enter if in no-fly)
    if (in.a_double_tap) {
        if (st.mode == FlightMode::Flight) {
            // Brake to hover immediately
            st.mode = FlightMode::Hover;
            st.velocity = glm::vec3(0.0f);
            fs_publish_locomotion_velocity(w, e, st.velocity);
            fs_publish_hover_enable(w, e, true);
        } else if (!in.in_no_fly) {
            st.mode = FlightMode::Flight;
            fs_publish_hover_enable(w, e, false);
        }
    }

    // In hover, allow gentle altitude nudge but keep zeroed velocity output
    if (st.mode == FlightMode::Hover) {
        glm::vec3 p = fs_get_position(w, e);
        p.y += cfg.altitude_rate_mps * std::clamp(in.right_stick_vert, -1.0f, 1.0f) * dt;
        fs_set_position(w, e, p);
        fs_publish_locomotion_velocity(w, e, glm::vec3(0.0f));
        return;
    }

    // Flight mode motion
    // Smooth towards target velocity with accel/decel taus (per-axis)
    auto choose_tau = [&](float cur, float tgt){ return (std::fabs(tgt) > std::fabs(cur)) ? cfg.accel_tau_s : cfg.decel_tau_s; };
    glm::vec3 vel = st.velocity;
    glm::vec3 tgt = st.target_velocity;

    float tau_x = choose_tau(vel.x, tgt.x);
    float tau_y = choose_tau(vel.y, tgt.y);
    float tau_z = choose_tau(vel.z, tgt.z);

    vel.x = fs_exp_smooth(vel.x, tgt.x, tau_x, dt);
    vel.y = fs_exp_smooth(vel.y, tgt.y, tau_y, dt);
    vel.z = fs_exp_smooth(vel.z, tgt.z, tau_z, dt);

    st.velocity = vel;

    // Optional soft collision avoidance/bounce
    fs_apply_soft_collision(cfg, in, st);

    // Integrate position
    glm::vec3 p = fs_get_position(w, e);
    p += st.velocity * dt;
    fs_set_position(w, e, p);

    // Publish current velocity for interop
    fs_publish_locomotion_velocity(w, e, st.velocity);

    // Haptics on boost engage
    if (st.boost_held && !st.last_boost_held) {
        fs_publish_haptics(w, e, "flight_boost", "both", 1.0f, 0.12f);
    }
    st.last_boost_held = st.boost_held;

    // Quick-evade haptics
    if (in.b_tap) {
        fs_publish_haptics(w, e, "evade_burst", "both", 1.0f, 0.08f);
    }
}

} // namespace ptv
