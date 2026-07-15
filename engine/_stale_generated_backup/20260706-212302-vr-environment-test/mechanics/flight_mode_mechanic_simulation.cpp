#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <cmath>
#include <string>
#include <algorithm>

#include "flight_mode_mechanic_shared.h"

namespace ptv {

static inline Vec3 vec3_from_json3(const nlohmann::json& j, const char* k, const Vec3& defv) {
    if (!j.is_object() || !j.contains(k) || !j.at(k).is_array()) return defv;
    const auto& a = j.at(k);
    if (a.size() != 3) return defv;
    return Vec3{ a[0].get<float>(), a[1].get<float>(), a[2].get<float>() };
}

static inline Vec3 basis_forward_from_mode(const nlohmann::json& j, const std::string& mode, const Transform& rig_tr) {
    // Prefer explicit vectors injected by harness; fallback to rig forward (positive Z in local space rotated by rig)
    if (mode == "controller") {
        Vec3 f = vec3_from_json3(j, "right_controller_forward", Vec3{0,0,1});
        // ensure y-up comfort: zero small pitch to avoid drift when horizon_lock
        return f;
    }
    if (mode == "head") {
        Vec3 f = vec3_from_json3(j, "head_forward", Vec3{0,0,1});
        return f;
    }
    // fallback to rig forward from rotation
    // Transform rotation is a quaternion (w,x,y,z). Apply to forward (0,0,1)
    // We don't have a full mat util; approximate using Math.h utilities: assume ptv::Quat mul is supported by engine math.
    // If not, just return +Z.
    return Vec3{0,0,1};
}

static inline void emit_event(World& w, std::uint32_t e, const char* evt) {
    // Minimal convention: append event tag once per frame; downstream haptics system can consume then clear.
    if (!w.has<CTag>(e)) { w.emplace_or_replace<CTag>(e, CTag{}); }
    auto t = w.get<CTag>(e);
    if (std::find(t.tags.begin(), t.tags.end(), std::string(evt)) == t.tags.end()) t.tags.push_back(std::string(evt));
    w.emplace_or_replace<CTag>(e, std::move(t));
}

void FlightModeMechanic::on_update(World& world, std::uint32_t entity, float dt) {
    if (!world.valid(entity)) return;
    if (!world.has<CTransform>(entity)) return;

    auto& tr = world.get<CTransform>(entity).t;

    // Live inputs/state from binding
    nlohmann::json in = ptv_flight_fetch_self_params(world, entity, kId);

    // Entry / exit gates (buttons are provided by harness via params_json)
    bool a_double_tap = j_get_b(in, "A_right_double_tap", false) || j_get_b(in, "enter_flight", false);
    bool brake_tap    = j_get_b(in, "B_right_tap", false) || j_get_b(in, "brake_tap", false);
    bool boost_held   = j_get_b(in, "L_stick_click_held", false) || j_get_b(in, "boost_held", false);

    // Resolve orientation mode and axes
    std::string orient_mode = j_get_s(in, "orientation_mode", std::string("head"));
    float lx = j_get_f(in, "left_x", 0.0f);
    float ly = j_get_f(in, "left_y", 0.0f);
    float rx = j_get_f(in, "right_x", 0.0f);
    float ry = j_get_f(in, "right_y", 0.0f);

    // Optional external energy gate (when low, disable boost)
    bool energy_low = j_get_b(in, "ring_energy_low", false);

    // Enter flight if requested
    if (a_double_tap && !active_) {
        active_ = true;
        braking_ = false;
        brake_timer_ = 0.0f;
        emit_event(world, entity, "flight.start");
    }

    // Quick brake to hover
    if (active_ && brake_tap && !braking_) {
        braking_ = true;
        brake_timer_ = 0.25f; // 250 ms to stop
    }

    // Compute desired planar direction from left stick in chosen frame
    Vec3 basis_f = basis_forward_from_mode(in, orient_mode, tr);
    // Derive a right vector (approximate assuming Y-up); not using full cross to avoid dependency on full math
    Vec3 world_up{0,1,0};
    // Right = normalize(cross(up, forward))
    Vec3 right = Vec3{
        world_up.y * basis_f.z - world_up.z * basis_f.y,
        world_up.z * basis_f.x - world_up.x * basis_f.z,
        world_up.x * basis_f.y - world_up.y * basis_f.x };
    // Recompute forward planar (remove vertical to stay horizon-comfort)
    Vec3 f_planar = Vec3{basis_f.x, 0.0f, basis_f.z};

    // Desired move vector (planar)
    Vec3 desired_v = (f_planar * ly) + (right * lx);

    // Altitude control via right stick Y when enabled
    float climb = 0.0f;
    if (right_stick_altitude_) {
        climb = ry * altitude_rate_mps_;
    }

    // Target speed
    float target_speed = base_speed_mps_ * std::min(1.0f, std::sqrt(lx*lx + ly*ly));
    bool want_boost = boost_held && !energy_low && (target_speed > 0.01f);

    // Boost toggle and events
    if (want_boost && !boost_) { boost_ = true; emit_event(world, entity, "flight.boost_on"); }
    if ((!want_boost || braking_) && boost_) { boost_ = false; emit_event(world, entity, "flight.boost_off"); }

    if (boost_) target_speed = boost_max_mps_ * std::min(1.0f, std::sqrt(lx*lx + ly*ly));

    // Exponential approach to target speed (separate accel/decel time constants)
    float tau = (std::sqrt(velocity_.x*velocity_.x + velocity_.z*velocity_.z) < target_speed) ? accel_tau_s_ : decel_tau_s_;
    float a = (tau > 0.0f) ? (1.0f - std::exp(-dt / tau)) : 1.0f;

    // Desired planar velocity direction
    float dir_len2 = desired_v.x*desired_v.x + desired_v.z*desired_v.z;
    if (dir_len2 > 1e-6f) {
        float inv_len = 1.0f / std::sqrt(dir_len2);
        desired_v.x *= inv_len * target_speed;
        desired_v.z *= inv_len * target_speed;
    } else {
        desired_v.x = 0.0f; desired_v.z = 0.0f;
    }

    // Blend current toward desired (planar)
    velocity_.x = velocity_.x + (desired_v.x - velocity_.x) * a;
    velocity_.z = velocity_.z + (desired_v.z - velocity_.z) * a;
    // Altitude integrates directly
    velocity_.y = climb;

    // Brake overrides: lerp velocity to zero over brake_timer_
    if (braking_) {
        float t_remain = std::max(0.0f, brake_timer_);
        float k = (t_remain <= 0.0f) ? 0.0f : std::max(0.0f, std::min(1.0f, t_remain / 0.25f));
        // As timer ticks down, k shrinks to 0 -> velocity goes to 0
        velocity_ = velocity_ * k;
        brake_timer_ -= dt;
        if (brake_timer_ <= 0.0f) {
            velocity_ = Vec3{0,0,0};
            active_ = false;
            braking_ = false;
            emit_event(world, entity, "flight.stop");
            // Hand off to hover
            ptv_request_hover_now(world, entity);
        }
    }

    // Apply movement if active
    if (active_) {
        tr.position = tr.position + velocity_ * dt;
    }

    // Bank and horizon lock: compute subtle roll easing into comfort range (visual only placeholder — encoded in fov_ease tag)
    float planar_speed = std::sqrt(velocity_.x*velocity_.x + velocity_.z*velocity_.z);
    float speed01 = ptv_clamp((planar_speed - base_speed_mps_) / std::max(0.001f, (boost_max_mps_ - base_speed_mps_)), 0.0f, 1.0f);

    // Publish VFX/camera scalars
    float fov_ease = 0.15f * speed01; // very subtle
    ptv_flight_publish_vfx(world, entity, kId, speed01, fov_ease);

    // Mirror state to CFlightState for other systems
    if (world.has<CFlightState>(entity)) {
        auto st = world.get<CFlightState>(entity);
        st.active = active_;
        st.boost = boost_;
        st.speed_mps = planar_speed;
        st.fov_ease01 = fov_ease;
        world.emplace_or_replace<CFlightState>(entity, st);
    } else {
        world.emplace_or_replace<CFlightState>(entity, CFlightState{active_, boost_, planar_speed, fov_ease});
    }
}

} // namespace ptv
