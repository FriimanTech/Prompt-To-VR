#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <glm/glm.hpp>
#include <entt/entt.hpp>
#include <algorithm>
#include <cmath>
#include <string>

#include "ground_sprint_collision_guard_shared.h"

namespace ptv {

class GroundSprintCollisionGuard : public IMechanic {
public:
    void on_attach(World& w, std::uint32_t e, const nlohmann::json& p) override {
        params_ = p;
        // Ensure our transient components exist
        if (!w.has<CSprintState>(e)) w.emplace_or_replace<CSprintState>(e, CSprintState{});
        if (!w.has<CLastPoseGS>(e)) w.emplace_or_replace<CLastPoseGS>(e, CLastPoseGS{});
    }

    void on_update(World& w, std::uint32_t e, float dt) override {
        if (!w.valid(e) || !w.has<CTransform>(e)) return;
        auto& tr = w.get<CTransform>(e).t;
        auto& sprint = w.get<CSprintState>(e);
        auto& last = w.get<CLastPoseGS>(e);

        // Settings snapshot
        const float snap_turn_fallback_deg = params_.value("snap_turn_fallback_deg", 45.0f);
        const auto settings = read_settings_from_bindings(w, e, snap_turn_fallback_deg);

        // Inputs
        auto in = read_inputs_from_bindings(w, e);
        const float right_deadzone = params_.value("right_stick_deadzone", 0.2f);
        const float left_deadzone = params_.value("left_stick_deadzone", 0.15f);
        if (glm::length(in.left_stick) < left_deadzone) in.left_stick = glm::vec2(0.0f);
        if (std::abs(in.right_stick_x) < right_deadzone) in.right_stick_x = 0.0f;

        // Initialize last pose
        if (!last.initialized) {
            last.last_pos = tr.position;
            last.last_yaw_deg = 0.0f;
            last.initialized = true;
        }

        // Hover/flight gates
        const bool grounded_ok = !(in.hover_active || in.flight_active);

        // Sprint params
        const bool sprint_on_click = params_.value("left_stick_click_is_sprint", true);
        const float mult = params_.value("sprint_multiplier", 1.5f);
        const float tau_rise = params_.value("sprint_rise_tau_s", 0.08f);
        const float tau_fall = params_.value("sprint_fall_tau_s", 0.12f);
        const float max_dur = params_.value("sprint_max_duration_s", 2.0f);
        const float cooldown_s = params_.value("sprint_cooldown_s", 1.0f);
        const float max_speed_mps = params_.value("max_speed_mps", 3.5f);

        // Update sprint state
        bool want_sprint = sprint_on_click ? in.left_stick_click : (glm::length(in.left_stick) > 0.99f);
        if (!grounded_ok) want_sprint = false;

        if (sprint.cooldown_remaining_s > 0.0f) sprint.cooldown_remaining_s = std::max(0.0f, sprint.cooldown_remaining_s - dt);

        if (want_sprint && sprint.cooldown_remaining_s <= 0.0f) {
            sprint.active = true;
        }
        if (!want_sprint) {
            sprint.active = false;
        }
        if (sprint.active) {
            sprint.time_in_s += dt;
            if (sprint.time_in_s >= max_dur) { sprint.active = false; sprint.cooldown_remaining_s = cooldown_s; }
        } else {
            if (sprint.time_in_s > 0.0f && sprint.cooldown_remaining_s <= 0.0f) {
                // ended naturally or released; start cooldown if we actually sprinted
                if (sprint.time_in_s > 0.05f) sprint.cooldown_remaining_s = cooldown_s;
            }
            if (!want_sprint) sprint.time_in_s = 0.0f;
        }

        const float target_factor = (sprint.active ? mult : 1.0f);
        sprint.factor = exp_approach(sprint.factor, target_factor, (sprint.active ? tau_rise : tau_fall), dt);

        // Comfort vignette request (stored on component; a renderer/UI could consume)
        if (settings.vignette_enabled) {
            const float base = settings.vignette_base;
            const float k = settings.vignette_k;
            sprint.vignette_intensity = base + k * std::max(0.0f, sprint.factor - 1.0f);
        } else {
            sprint.vignette_intensity = 0.0f;
        }

        // Layer sprint over builtin slide by extending baseline per-frame displacement
        glm::vec3 curr{tr.position.x, tr.position.y, tr.position.z};
        glm::vec3 baseline_delta = curr - glm::vec3(last.last_pos.x, last.last_pos.y, last.last_pos.z);
        glm::vec3 desired_delta = baseline_delta * std::max(1.0f, sprint.factor);

        // Clamp to comfort max speed
        const float max_step = max_speed_mps * dt;
        float dlen = std::sqrt(std::max(0.0f, glm::dot(desired_delta, desired_delta)));
        if (dlen > max_step && dlen > 1e-5f) desired_delta *= (max_step / dlen);

        // Predict and guard against constructs
        const float guard_radius = params_.value("guard_radius_m", 0.35f);
        const float pushback_strength = params_.value("pushback_strength", 0.5f);
        const float max_pen = params_.value("max_penetration_m", 0.02f);

        glm::vec3 proposed = glm::vec3(last.last_pos.x, last.last_pos.y, last.last_pos.z) + desired_delta;
        glm::vec3 guarded = collision_guard_slide(w,
                                                  glm::vec3(last.last_pos.x, last.last_pos.y, last.last_pos.z),
                                                  proposed,
                                                  guard_radius,
                                                  pushback_strength,
                                                  max_pen);

        // Write back position override
        tr.position.x = guarded.x;
        tr.position.y = guarded.y;
        tr.position.z = guarded.z;

        // Smooth turning when enabled; otherwise rely on builtin snap-turn binding
        if (settings.smooth_turn_enabled && in.right_stick_x != 0.0f) {
            const float yaw = in.right_stick_x * settings.smooth_turn_rate_dps * dt;
            apply_yaw_deg_to_transform(tr, yaw);
        }

        // Update last
        last.last_pos = tr.position;
    }

    void on_detach(World& w, std::uint32_t e) override {
        if (w.has<CSprintState>(e)) w.remove<CSprintState>(e);
        if (w.has<CLastPoseGS>(e)) w.remove<CLastPoseGS>(e);
    }

    std::string id() const override { return "generated.ground_sprint_collision_guard"; }

private:
    nlohmann::json params_ = nlohmann::json::object();
};

PTV_REGISTER_MECHANIC("generated.ground_sprint_collision_guard", GroundSprintCollisionGuard)

} // namespace ptv
