#pragma once

#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"
#include <algorithm>
#include <nlohmann/json.hpp>
#include <cmath>
#include <cstdint>
#include <string>
#include <optional>

namespace ptv {

// Runtime state for zero-g navigation and comfort.
struct CZeroGState {
    bool active{false};
    bool anchored{false};
    ptv::Vec3 velocity{0.0f, 0.0f, 0.0f};
    ptv::Vec3 ang_velocity_rps{0.0f, 0.0f, 0.0f}; // roll/pitch/yaw or xyz axis rates (rad/s)
    ptv::Vec3 anchor_normal{0.0f, 1.0f, 0.0f};

    // Tunables (filled from params on attach; can be overwritten by live params if present)
    float accel_mps2{1.6f};
    float max_speed_mps{2.2f};
    float lin_damping{1.2f};   // 1/s in exponential decay exp(-d*t)
    float ang_damping{1.8f};   // 1/s
    float vignette_spin_scale{0.6f}; // comfort multiplier

    float vignette_intensity{0.0f}; // 0..1 suggestion for HUD system
};

// --- Small math helpers on engine math types (Vec3/Quat are simple xyz and wxyz) ---
inline ptv::Vec3 v3_add(const ptv::Vec3& a, const ptv::Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline ptv::Vec3 v3_sub(const ptv::Vec3& a, const ptv::Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline ptv::Vec3 v3_mul(const ptv::Vec3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float    v3_dot(const ptv::Vec3& a, const ptv::Vec3& b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
inline float    v3_len(const ptv::Vec3& a) { return std::sqrt(std::max(0.0f, v3_dot(a,a))); }
inline ptv::Vec3 v3_norm(const ptv::Vec3& a) { float L = v3_len(a); return (L > 1e-6f)? v3_mul(a, 1.0f/L) : ptv::Vec3{0,0,0}; }
inline ptv::Vec3 v3_cross(const ptv::Vec3& a, const ptv::Vec3& b) { return {a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x}; }

inline ptv::Quat q_mul(const ptv::Quat& A, const ptv::Quat& B) {
    // (w, x, y, z)
    return ptv::Quat{
        A.w*B.w - A.x*B.x - A.y*B.y - A.z*B.z,
        A.w*B.x + A.x*B.w + A.y*B.z - A.z*B.y,
        A.w*B.y - A.x*B.z + A.y*B.w + A.z*B.x,
        A.w*B.z + A.x*B.y - A.y*B.x + A.z*B.w
    };
}
inline ptv::Quat q_norm(const ptv::Quat& q) {
    float n = std::sqrt(q.w*q.w + q.x*q.x + q.y*q.y + q.z*q.z);
    if (n <= 1e-6f) return ptv::Quat{1,0,0,0};
    float s = 1.0f/n; return ptv::Quat{q.w*s, q.x*s, q.y*s, q.z*s};
}
inline ptv::Quat q_from_axis_angle(const ptv::Vec3& axis, float angle_rad) {
    ptv::Vec3 na = v3_norm(axis);
    float s = std::sin(0.5f*angle_rad);
    return q_norm(ptv::Quat{std::cos(0.5f*angle_rad), na.x*s, na.y*s, na.z*s});
}
inline ptv::Quat q_from_euler_xyz(float rx, float ry, float rz) {
    // intrinsic XYZ (roll X, pitch Y, yaw Z)
    ptv::Quat qx = q_from_axis_angle({1,0,0}, rx);
    ptv::Quat qy = q_from_axis_angle({0,1,0}, ry);
    ptv::Quat qz = q_from_axis_angle({0,0,1}, rz);
    return q_mul(qz, q_mul(qy, qx));
}
inline ptv::Vec3 q_rotate(const ptv::Quat& q, const ptv::Vec3& v) {
    // v' = q * (0,v) * conj(q)
    ptv::Quat p{0, v.x, v.y, v.z};
    ptv::Quat qc{q.w, -q.x, -q.y, -q.z};
    ptv::Quat r = q_mul(q, q_mul(p, qc));
    return {r.x, r.y, r.z};
}
inline ptv::Vec3 basis_forward(const ptv::Quat& q) { return q_rotate(q, {0,0,1}); }
inline ptv::Vec3 basis_right  (const ptv::Quat& q) { return q_rotate(q, {1,0,0}); }
inline ptv::Vec3 basis_up     (const ptv::Quat& q) { return q_rotate(q, {0,1,0}); }

// Rotate current orientation so its up aligns with target normal (minimal rotation)
inline ptv::Quat align_up_to_normal(const ptv::Quat& current, const ptv::Vec3& target_normal) {
    ptv::Vec3 up = basis_up(current);
    ptv::Vec3 n  = v3_norm(target_normal);
    float c = std::clamp(v3_dot(up, n), -1.0f, 1.0f);
    if (c > 0.9995f) return current; // already aligned
    ptv::Vec3 axis = v3_cross(up, n);
    float angle = std::acos(c);
    ptv::Quat dq = q_from_axis_angle(axis, angle);
    return q_norm(q_mul(dq, current));
}

// Exponential damping factor exp(-d * dt)
inline float damp_exp(float d, float dt) { return std::exp(-std::max(0.0f, d) * std::max(0.0f, dt)); }

// Finds and parses this mechanic's live params from CMechanicBinding (if present).
inline std::optional<nlohmann::json> read_live_params(World& w, std::uint32_t e, const char* mech_id) {
    if (!w.has<CMechanicBinding>(e)) return std::nullopt;
    auto& mb = w.get<CMechanicBinding>(e);
    for (const auto& ent : mb.entries) {
        if (ent.id == mech_id || ent.builtin_id == mech_id) {
            try {
                auto j = nlohmann::json::parse(ent.params_json, nullptr, /*allow_exceptions=*/false);
                if (j.is_object()) return j;
            } catch (...) { return std::nullopt; }
        }
    }
    return std::nullopt;
}

// Zero-g 6DOF navigation mechanic. The class lives in this shared header so
// both the register TU (registration) and the simulation TU (on_update body)
// see the same definition.
class ZeroGNavigation : public IMechanic {
public:
    void on_attach(World& world, std::uint32_t entity, const nlohmann::json& params) override {
        CZeroGState st;
        st.accel_mps2 = params.value("accel_mps2", st.accel_mps2);
        st.max_speed_mps = params.value("max_speed_mps", st.max_speed_mps);
        st.lin_damping = params.value("lin_damping", st.lin_damping);
        st.ang_damping = params.value("ang_damping", st.ang_damping);
        st.vignette_spin_scale = params.value("vignette_spin_scale", st.vignette_spin_scale);

        bool force_active = params.value("force_active", false);
        bool scene_zero_g = false;
        if (world.has<CTag>(entity)) {
            auto& tg = world.get<CTag>(entity);
            for (auto& s : tg.tags) { if (s == "zero_g") { scene_zero_g = true; break; } }
        }
        st.active = (force_active || scene_zero_g);

        world.emplace_or_replace<CZeroGState>(entity, st);
    }

    // Defined out-of-line in zero_g_navigation_simulation.cpp.
    void on_update(World& world, std::uint32_t entity, float dt) override;

    void on_detach(World& /*world*/, std::uint32_t /*entity*/) override {}

    std::string id() const override { return "generated.zero_g_navigation"; }
};

} // namespace ptv
