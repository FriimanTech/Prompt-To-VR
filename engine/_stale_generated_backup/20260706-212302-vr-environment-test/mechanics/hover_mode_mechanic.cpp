#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <glm/glm.hpp>
#include <entt/entt.hpp>

#include <string>
#include <algorithm>
#include <unordered_map>
#include <cmath>

#include "hover_mode_mechanic_shared.h"

namespace ptv {

// ---------- Helper impls ----------

glm::vec2 read_left_stick(const nlohmann::json& jb) {
    if (jb.contains("left_stick") && jb.at("left_stick").is_array() && jb.at("left_stick").size() == 2) {
        return glm::vec2(jb.at("left_stick")[0].get<float>(), jb.at("left_stick")[1].get<float>());
    }
    if (jb.contains("L_stick") && jb.at("L_stick").is_array() && jb.at("L_stick").size() == 2) {
        return glm::vec2(jb.at("L_stick")[0].get<float>(), jb.at("L_stick")[1].get<float>());
    }
    return glm::vec2(0.0f);
}

glm::vec2 read_right_stick(const nlohmann::json& jb) {
    if (jb.contains("right_stick") && jb.at("right_stick").is_array() && jb.at("right_stick").size() == 2) {
        return glm::vec2(jb.at("right_stick")[0].get<float>(), jb.at("right_stick")[1].get<float>());
    }
    if (jb.contains("R_stick") && jb.at("R_stick").is_array() && jb.at("R_stick").size() == 2) {
        return glm::vec2(jb.at("R_stick")[0].get<float>(), jb.at("R_stick")[1].get<float>());
    }
    return glm::vec2(0.0f);
}

float read_left_trigger_analog(const nlohmann::json& jb) {
    if (jb.contains("left_trigger") && jb.at("left_trigger").is_number())
        return std::clamp(jb.at("left_trigger").get<float>(), 0.0f, 1.0f);
    if (jb.contains("L_trigger") && jb.at("L_trigger").is_number())
        return std::clamp(jb.at("L_trigger").get<float>(), 0.0f, 1.0f);
    if (jb.contains("L_trigger_held") && jb.at("L_trigger_held").is_boolean())
        return jb.at("L_trigger_held").get<bool>() ? 1.0f : 0.0f;
    return 0.0f;
}

bool read_button_tap(const nlohmann::json& jb, const std::string& logical_name) {
    // Accept both exact and with suffix _tap
    if (jb.contains(logical_name) && jb.at(logical_name).is_boolean())
        return jb.at(logical_name).get<bool>();
    std::string with = logical_name + "_tap";
    if (jb.contains(with) && jb.at(with).is_boolean())
        return jb.at(with).get<bool>();
    return false;
}

static inline glm::vec3 to_glm(const ptv::Vec3& v){ return glm::vec3(v.x, v.y, v.z); }
static inline ptv::Vec3 to_ptv(const glm::vec3& v){ return ptv::Vec3{v.x, v.y, v.z}; }

ptv::Quat yaw_from_forward(const glm::vec3& fwd) {
    glm::vec3 h = glm::normalize(glm::vec3(fwd.x, 0.0f, fwd.z));
    if (!std::isfinite(h.x) || !std::isfinite(h.z) || glm::length(h) < 1e-4f) {
        return ptv::Quat{1.0f, 0.0f, 0.0f, 0.0f};
    }
    // Build yaw from atan2
    float yaw = std::atan2(h.x, h.z); // right-handed: +yaw turns right around +Y
    float cy = std::cos(0.5f * yaw);
    float sy = std::sin(0.5f * yaw);
    return ptv::Quat{cy, 0.0f, sy, 0.0f};
}

ptv::Quat stabilize_level(const ptv::Quat& current, float max_bank_deg, float dt) {
    // Convert to yaw-only target, slerp small step to remove pitch/roll while clamping bank.
    // Extract forward from current
    // For simplicity, derive yaw-only as yaw_from_forward of rotated (0,0,1)
    // Math.h doesn't expose rotate; approximate by using matrix would be overkill — we clamp by blending.
    // Assume small dt; blend factor removes pitch/roll gradually.
    float clamp_rad = glm::radians(std::max(0.0f, max_bank_deg));
    // We cannot easily inspect bank without full math helpers; approximate via slerp toward yaw-only from current forward inferred via world-up assumption.
    // Build a forward vector from quaternion (basic formula)
    const float w = current.w, x = current.x, y = current.y, z = current.z;
    glm::vec3 fwd{
        2.0f*(x*z + w*y),
        2.0f*(y*z - w*x),
        1.0f - 2.0f*(x*x + y*y)
    };
    ptv::Quat yaw_only = yaw_from_forward(fwd);

    // Blend factor: remove pitch/roll with time constant ~0.25s
    float tau = 0.25f;
    float alpha = 1.0f - std::exp(-dt / tau);

    // Slerp current -> yaw_only by alpha
    // Manual slerp for ptv::Quat (w,x,y,z)
    auto dot = current.w*yaw_only.w + current.x*yaw_only.x + current.y*yaw_only.y + current.z*yaw_only.z;
    ptv::Quat b = yaw_only;
    if (dot < 0.0f) { dot = -dot; b = ptv::Quat{-yaw_only.w, -yaw_only.x, -yaw_only.y, -yaw_only.z}; }
    float one_minus_dot = 1.0f - dot;
    float k0, k1;
    if (one_minus_dot < 1e-6f) { k0 = 1.0f - alpha; k1 = alpha; }
    else {
        float theta = std::acos(std::clamp(dot, -1.0f, 1.0f));
        float s = std::sin(theta);
        k0 = std::sin((1.0f - alpha) * theta) / s;
        k1 = std::sin(alpha * theta) / s;
    }
    ptv::Quat out{
        k0*current.w + k1*b.w,
        k0*current.x + k1*b.x,
        k0*current.y + k1*b.y,
        k0*current.z + k1*b.z
    };

    // Clamp bank: we approximate by mixing a bit back if bank exceeded — skip heavy computation, rely on gradual slerp.
    (void)clamp_rad;
    return out;
}

glm::vec3 first_order_velocity(glm::vec3 v, glm::vec3 v_target, float tau_s, float max_speed, float dt) {
    if (tau_s < 1e-4f) return glm::clamp(v_target, -max_speed, max_speed);
    float a = 1.0f - std::exp(-dt / std::max(1e-4f, tau_s));
    glm::vec3 out = v + a * (v_target - v);
    float sp = glm::length(out);
    if (sp > max_speed) out *= (max_speed / std::max(sp, 1e-6f));
    return out;
}

nlohmann::json find_self_binding_params(World& world, std::uint32_t entity, const std::string& self_id) {
    if (!world.has<CMechanicBinding>(entity)) return nlohmann::json::object();
    const auto& mb = world.get<CMechanicBinding>(entity);
    for (const auto& e : mb.entries) {
        if (e.id == self_id) {
            auto j = nlohmann::json::parse(e.params_json, nullptr, /*allow_exceptions=*/false);
            if (j.is_object()) return j;
            return nlohmann::json::object();
        }
    }
    return nlohmann::json::object();
}

bool near_no_fly_volume(World& world, const ptv::Vec3& position, float radius_m) {
    auto& reg = world.registry();
    auto view = reg.view<CTag, CTransform>();
    const float r2 = radius_m * radius_m;
    for (auto ent : view) {
        const auto& tag = view.get<CTag>(ent);
        if (std::find(tag.tags.begin(), tag.tags.end(), std::string("no_fly")) == tag.tags.end()) continue;
        const auto& tr = view.get<CTransform>(ent).t;
        glm::vec3 d = to_glm(tr.position) - to_glm(position);
        float d2 = glm::dot(d, d);
        if (d2 <= r2) return true;
    }
    return false;
}

// ---------- Mechanic ----------

class HoverModeMechanic : public IMechanic {
public:
    void on_attach(World& world, std::uint32_t entity, const nlohmann::json& params) override {
        HoverRuntime rt; // defaults
        cfg_.double_tap_ms = params.value("double_tap_ms", cfg_.double_tap_ms);
        cfg_.hover_max_speed_mps = params.value("hover_max_speed_mps", cfg_.hover_max_speed_mps);
        cfg_.accel_tau_s = params.value("accel_tau_s", cfg_.accel_tau_s);
        cfg_.decel_tau_s = params.value("decel_tau_s", cfg_.decel_tau_s);
        cfg_.right_stick_vertical = params.value("right_stick_vertical", cfg_.right_stick_vertical);
        cfg_.altitude_rate_mps = params.value("altitude_rate_mps", cfg_.altitude_rate_mps);
        cfg_.fine_altitude_rate_mps = params.value("fine_altitude_rate_mps", cfg_.fine_altitude_rate_mps);
        cfg_.brake_hold_s = params.value("brake_hold_s", cfg_.brake_hold_s);
        cfg_.bank_max_deg = params.value("bank_max_deg", cfg_.bank_max_deg);
        cfg_.bob_amp_m = params.value("bob_amp_m", cfg_.bob_amp_m);
        cfg_.ring_drain_per_s = params.value("ring_drain_per_s", cfg_.ring_drain_per_s);
        cfg_.emergency_hold_s = params.value("emergency_hold_s", cfg_.emergency_hold_s);
        cfg_.toggle_button = params.value("toggle_button", cfg_.toggle_button);
        cfg_.brake_button = params.value("brake_button", cfg_.brake_button);
        cfg_.left_stick_move = params.value("left_stick_move", cfg_.left_stick_move);
        cfg_.left_trigger_fine_alt = params.value("left_trigger_fine_alt", cfg_.left_trigger_fine_alt);
        cfg_.use_snap_turn = params.value("use_snap_turn", cfg_.use_snap_turn);
        cfg_.snap_turn_deg = params.value("snap_turn_deg", cfg_.snap_turn_deg);
        cfg_.smooth_turn_rate_dps = params.value("smooth_turn_rate_dps", cfg_.smooth_turn_rate_dps);
        cfg_.locomotion_frame = params.value("locomotion_frame", cfg_.locomotion_frame);
        cfg_.min_altitude_m = params.value("min_altitude_m", cfg_.min_altitude_m);
        cfg_.max_altitude_m = params.value("max_altitude_m", cfg_.max_altitude_m);

        if (world.has<CTransform>(entity)) {
            auto& tr = world.get<CTransform>(entity).t;
            rt.base_altitude_m = tr.position.y;
            rt.target_altitude_m = tr.position.y;
            // initial forward from rotation
            const float w = tr.rotation.w, x = tr.rotation.x, y = tr.rotation.y, z = tr.rotation.z;
            glm::vec3 fwd{
                2.0f*(x*z + w*y),
                2.0f*(y*z - w*x),
                1.0f - 2.0f*(x*x + y*y)
            };
            rt.target_forward = glm::normalize(glm::vec3(fwd.x, 0.0f, fwd.z));
            if (!std::isfinite(rt.target_forward.x) || glm::length(rt.target_forward) < 1e-3f)
                rt.target_forward = glm::vec3(0,0,1);
        }
        runtimes_[entity] = rt;
        (void)world;
    }

    void on_update(World& world, std::uint32_t entity, float dt) override {
        if (!world.valid(entity)) return;
        auto it = runtimes_.find(entity);
        if (it == runtimes_.end()) return;
        HoverRuntime& rt = it->second;
        rt.t_now_s += dt;
        rt.bob_phase_s += dt;
        if (rt.no_fly_cooldown_s > 0.0f) rt.no_fly_cooldown_s = std::max(0.0f, rt.no_fly_cooldown_s - dt);

        // Read current binding inputs for this mechanic
        nlohmann::json jb = find_self_binding_params(world, entity, id());

        // Button taps
        bool toggle_tap = read_button_tap(jb, cfg_.toggle_button);
        bool brake_tap = read_button_tap(jb, cfg_.brake_button);

        // Double-tap detect for toggle
        if (toggle_tap) {
            float dtap_ms = cfg_.double_tap_ms;
            if ((rt.t_now_s - rt.last_toggle_tap_s) * 1000.0f <= dtap_ms) {
                // Attempt toggle
                if (!rt.active) {
                    // Entry checks: disallow near no-fly volumes
                    if (world.has<CTransform>(entity)) {
                        auto& tr = world.get<CTransform>(entity).t;
                        if (near_no_fly_volume(world, tr.position, 1.5f)) {
                            // Deny, show tooltip via HUD system if present — TODO hook
                            rt.no_fly_cooldown_s = 1.0f;
                        } else {
                            rt.active = true;
                            rt.brake_hold_timer_s = 0.2f; // short initial stabilize
                            rt.vel = glm::vec3(0.0f);
                            rt.base_altitude_m = tr.position.y;
                            rt.target_altitude_m = tr.position.y;
                        }
                    }
                } else {
                    // Exit hover
                    rt.active = false;
                    rt.brake_hold_timer_s = 0.0f;
                    // Smoothly level out on exit (minor nudge)
                    if (world.has<CTransform>(entity)) {
                        auto& tr = world.get<CTransform>(entity).t;
                        tr.rotation = stabilize_level(tr.rotation, cfg_.bank_max_deg, dt * 4.0f);
                    }
                }
            }
            rt.last_toggle_tap_s = rt.t_now_s;
        }

        if (!world.has<CTransform>(entity)) return;
        auto& tr = world.get<CTransform>(entity).t;

        // Quick brake
        if (rt.active && brake_tap) {
            rt.vel = glm::vec3(0.0f);
            rt.brake_hold_timer_s = cfg_.brake_hold_s;
        }

        // Emergency energy emulation and drain
        // If a real energy system exists, test harness can pass ring_energy and emergency_threshold
        rt.ring_energy = jb.value("ring_energy", rt.ring_energy);
        rt.emergency_threshold = jb.value("emergency_threshold", rt.emergency_threshold);
        if (rt.active) {
            float drain = cfg_.ring_drain_per_s;
            if (drain > 0.0f) rt.ring_energy = std::max(0.0f, rt.ring_energy - drain * dt);
            if (rt.ring_energy < rt.emergency_threshold && rt.emergency_hold_timer_s <= 0.0f) {
                rt.emergency_hold_timer_s = cfg_.emergency_hold_s;
            }
            if (rt.emergency_hold_timer_s > 0.0f) {
                rt.emergency_hold_timer_s = std::max(0.0f, rt.emergency_hold_timer_s - dt);
                // During emergency hold, freeze horizontal velocity
                rt.vel.x = 0.0f; rt.vel.z = 0.0f;
                if (rt.emergency_hold_timer_s <= 0.0f) {
                    // Auto-land: disable hover
                    rt.active = false;
                }
            }
        }

        if (!rt.active) return; // nothing to do

        // Turning input
        glm::vec2 rstick = read_right_stick(jb);
        if (cfg_.use_snap_turn) {
            // Expect harness to set a boolean like "snap_turn" or nonzero rstick.x pulse; do simple gate
            if (std::abs(rstick.x) > 0.8f) {
                float sign = (rstick.x > 0.0f) ? 1.0f : -1.0f;
                // Apply snap instantly, but rate-limit crudely via brake_hold_timer as debounce
                if (rt.brake_hold_timer_s <= 0.0f) {
                    // Build yaw delta
                    float deg = cfg_.snap_turn_deg * sign;
                    float rad = glm::radians(deg);
                    float cy = std::cos(0.5f * rad), sy = std::sin(0.5f * rad);
                    ptv::Quat dq{cy, 0.0f, sy, 0.0f};
                    // Compose: new = dq * tr.rotation (Hamilton product)
                    const auto& a = dq; const auto& b = tr.rotation;
                    tr.rotation = ptv::Quat{
                        a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z,
                        a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
                        a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
                        a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w
                    };
                    rt.brake_hold_timer_s = 0.2f; // debounce window
                }
            }
        } else {
            float yaw_rate = cfg_.smooth_turn_rate_dps * glm::clamp(rstick.x, -1.0f, 1.0f);
            float rad = glm::radians(yaw_rate) * dt;
            if (std::abs(rad) > 1e-5f) {
                float cy = std::cos(0.5f * rad), sy = std::sin(0.5f * rad);
                ptv::Quat dq{cy, 0.0f, sy, 0.0f};
                const auto& a = dq; const auto& b = tr.rotation;
                tr.rotation = ptv::Quat{
                    a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z,
                    a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
                    a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
                    a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w
                };
            }
        }

        // Orientation stabilization (keep level)
        tr.rotation = stabilize_level(tr.rotation, cfg_.bank_max_deg, dt);

        // Movement input
        glm::vec2 lstick = cfg_.left_stick_move ? read_left_stick(jb) : glm::vec2(0.0f);

        // Decide locomotion frame (head vs controller). Without controller pose, default to head/body yaw (entity rotation)
        glm::vec3 fwd = glm::normalize(glm::vec3(0.0f, 0.0f, 1.0f));
        {
            // Derive world forward from current yaw
            const auto& q = tr.rotation;
            const float w = q.w, x = q.x, y = q.y, z = q.z;
            glm::vec3 dir{
                2.0f*(x*z + w*y),
                2.0f*(y*z - w*x),
                1.0f - 2.0f*(x*x + y*y)
            };
            fwd = glm::normalize(glm::vec3(dir.x, 0.0f, dir.z));
            if (!std::isfinite(fwd.x) || glm::length(fwd) < 1e-4f) fwd = glm::vec3(0,0,1);
        }
        glm::vec3 right = glm::normalize(glm::cross(glm::vec3(0,1,0), fwd));

        glm::vec3 v_target = fwd * lstick.y + right * lstick.x;
        if (glm::length(v_target) > 1e-4f)
            v_target = glm::normalize(v_target) * cfg_.hover_max_speed_mps * std::clamp(glm::length(lstick), 0.0f, 1.0f);

        float tau = (glm::length(lstick) > 0.05f) ? cfg_.accel_tau_s : cfg_.decel_tau_s;

        if (rt.brake_hold_timer_s > 0.0f) {
            rt.brake_hold_timer_s = std::max(0.0f, rt.brake_hold_timer_s - dt);
            v_target = glm::vec3(0.0f);
            tau = 0.08f; // sticky hold
        }

        rt.vel = first_order_velocity(rt.vel, v_target, tau, cfg_.hover_max_speed_mps, dt);

        // Altitude adjust (right stick Y and/or left trigger for fine)
        float alt_rate = 0.0f;
        if (cfg_.right_stick_vertical) {
            alt_rate += rstick.y * cfg_.altitude_rate_mps;
        }
        if (cfg_.left_trigger_fine_alt) {
            float lt = read_left_trigger_analog(jb);
            // Positive for up when held; allow down when pressing with negative modifier if provided (optional key)
            bool fine_down = jb.value("fine_alt_down", false);
            float fine_sign = fine_down ? -1.0f : 1.0f;
            alt_rate += fine_sign * lt * cfg_.fine_altitude_rate_mps;
        }
        rt.target_altitude_m = std::clamp(rt.target_altitude_m + alt_rate * dt, cfg_.min_altitude_m, cfg_.max_altitude_m);

        // Integrate position with bob
        glm::vec3 p = to_glm(tr.position);
        p += rt.vel * dt;
        float bob = cfg_.bob_amp_m * std::sin(6.2831853f * (rt.bob_phase_s / std::max(0.1f, cfg_.bob_period_s)));
        p.y = rt.target_altitude_m + bob;
        tr.position = to_ptv(p);

        // Modest vignette hint would be set here via a post effect system if available (speed dependent) — TODO: integrate when subsystem exists.
    }

    void on_detach(World&, std::uint32_t entity) override {
        runtimes_.erase(entity);
    }

    std::string id() const override { return "generated.hover_mode_mechanic"; }

private:
    HoverConfig cfg_{};
    std::unordered_map<std::uint32_t, HoverRuntime> runtimes_;
};

PTV_REGISTER_MECHANIC("generated.hover_mode_mechanic", HoverModeMechanic)

} // namespace ptv
