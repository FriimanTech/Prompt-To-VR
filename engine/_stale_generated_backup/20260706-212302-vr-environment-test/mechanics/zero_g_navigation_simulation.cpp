#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <string>
#include <cmath>
#include "zero_g_navigation_shared.h"

namespace ptv {

// Helper to read float2 arrays like [x, y]
static inline void read_vec2(const nlohmann::json& j, const char* key, float& x, float& y, float dx=0.0f, float dy=0.0f) {
    x = dx; y = dy;
    if (j.contains(key) && j.at(key).is_array() && j.at(key).size() >= 2) {
        x = j.at(key)[0].get<float>();
        y = j.at(key)[1].get<float>();
    }
}
static inline ptv::Vec3 read_vec3_def(const nlohmann::json& j, const char* key, const ptv::Vec3& defv) {
    if (j.contains(key) && j.at(key).is_array() && j.at(key).size() >= 3) {
        return ptv::Vec3{ j.at(key)[0].get<float>(), j.at(key)[1].get<float>(), j.at(key)[2].get<float>() };
    }
    return defv;
}

void ZeroGNavigation::on_update(World& world, std::uint32_t entity, float dt) {
    if (!world.valid(entity) || !world.has<CTransform>(entity)) return;
    auto& tr = world.get<CTransform>(entity).t;

    // Ensure state exists.
    if (!world.has<CZeroGState>(entity)) world.emplace_or_replace<CZeroGState>(entity, CZeroGState{});
    auto& st = world.get<CZeroGState>(entity);

    // Pull live params (acceptance harness can drive these every frame).
    auto live = read_live_params(world, entity, "generated.zero_g_navigation");
    nlohmann::json p = nlohmann::json::object();
    if (live.has_value() && live->is_object()) p = *live;

    // Runtime toggles
    bool enable = p.value("enable", st.active);
    st.vignette_spin_scale = p.value("vignette_spin_scale", st.vignette_spin_scale);
    st.accel_mps2   = p.value("accel_mps2", st.accel_mps2);
    st.max_speed_mps= p.value("max_speed_mps", st.max_speed_mps);
    st.lin_damping  = p.value("lin_damping", st.lin_damping);
    st.ang_damping  = p.value("ang_damping", st.ang_damping);

    if (!enable) {
        // Soft settle when disabled
        float k = damp_exp(st.lin_damping, dt);
        st.velocity = v3_mul(st.velocity, k);
        float ka = damp_exp(st.ang_damping, dt);
        st.ang_velocity_rps = v3_mul(st.ang_velocity_rps, ka);
        st.vignette_intensity = 0.0f;
        st.active = false;
        return;
    }
    st.active = true;

    // Read inputs (left/right sticks, frame selection, grip, optional surface normal hint)
    float lx=0.0f, ly=0.0f, rx=0.0f, ry=0.0f; // lx: strafe, ly: forward; rx: yaw/roll, ry: pitch
    read_vec2(p, "left_stick", lx, ly, 0.0f, 0.0f);
    read_vec2(p, "right_stick", rx, ry, 0.0f, 0.0f);
    bool grip_anchor = p.value("grip_anchor", true);
    bool grip_held   = p.value("grip_held", false);
    std::string frame = p.value("control_frame", std::string("hmd")); // "hmd" or "controller"

    // Compute thrust in world space.
    ptv::Vec3 thrust_world{0,0,0};
    if (frame == "controller") {
        // If provided, use controller forward in params, else fall back to HMD forward
        ptv::Vec3 ctrl_fwd = read_vec3_def(p, "controller_forward", basis_forward(tr.rotation));
        ptv::Vec3 ctrl_right = read_vec3_def(p, "controller_right", basis_right(tr.rotation));
        thrust_world = v3_add(v3_mul(ctrl_right, lx), v3_mul(ctrl_fwd, ly));
    } else {
        // Head-relative (HMD = player rig here)
        ptv::Vec3 fwd = basis_forward(tr.rotation);
        ptv::Vec3 right = basis_right(tr.rotation);
        thrust_world = v3_add(v3_mul(right, lx), v3_mul(fwd, ly));
    }
    thrust_world = v3_norm(thrust_world);

    // 6DOF linear dynamics with gentle drift
    if (!st.anchored) {
        st.velocity = v3_add(st.velocity, v3_mul(thrust_world, st.accel_mps2 * dt));
        // Speed clamp
        float speed = v3_len(st.velocity);
        if (speed > st.max_speed_mps) {
            st.velocity = v3_mul(v3_norm(st.velocity), st.max_speed_mps);
        }
        // Damping
        float k = damp_exp(st.lin_damping, dt);
        st.velocity = v3_mul(st.velocity, k);
        // Integrate position
        tr.position = v3_add(tr.position, v3_mul(st.velocity, dt));
    }

    // Angular assist from right stick: map to yaw/pitch/roll rates (rad/s)
    // Convention: rx -> yaw (around up), ry -> pitch (around right). Optional roll via p.value("roll_input", 0.0f)
    float roll_in = p.value("roll_input", 0.0f);
    ptv::Vec3 desired_rps{ roll_in, ry, rx }; // X=roll, Y=pitch, Z=yaw
    // Blend into ang velocity (first-order)
    st.ang_velocity_rps = v3_add( v3_mul(st.ang_velocity_rps, damp_exp(st.ang_damping, dt)), v3_mul(desired_rps, (1.0f - damp_exp(st.ang_damping, dt))) );

    // Integrate orientation
    ptv::Quat dq = q_from_euler_xyz(st.ang_velocity_rps.x * dt, st.ang_velocity_rps.y * dt, st.ang_velocity_rps.z * dt);
    tr.rotation = q_norm(q_mul(dq, tr.rotation));

    // Anchoring
    if (grip_anchor) {
        if (grip_held && !st.anchored) {
            st.anchored = true;
            st.velocity = {0,0,0};
            st.ang_velocity_rps = {0,0,0};
            // Use hint normal if provided, else current up (keeps pose stable)
            st.anchor_normal = read_vec3_def(p, "surface_normal", basis_up(tr.rotation));
        } else if (!grip_held && st.anchored) {
            st.anchored = false;
        }

        if (st.anchored) {
            // Align to surface normal smoothly
            ptv::Quat target = align_up_to_normal(tr.rotation, st.anchor_normal);
            // Slerp-lite via nlerp for stability
            const float a = std::clamp(p.value("anchor_align_rate", 6.0f) * dt, 0.0f, 1.0f);
            // nlerp(q, target, a)
            ptv::Quat q{ tr.rotation.w*(1.0f-a) + target.w*a,
                         tr.rotation.x*(1.0f-a) + target.x*a,
                         tr.rotation.y*(1.0f-a) + target.y*a,
                         tr.rotation.z*(1.0f-a) + target.z*a };
            tr.rotation = q_norm(q);
        }
    }

    // Comfort vignette based on angular speed
    float omega = v3_len(st.ang_velocity_rps); // rad/s
    // Map: 0 rad/s -> 0, 3 rad/s -> ~1 (tunable)
    float vign = std::clamp(st.vignette_spin_scale * (omega / 3.0f), 0.0f, 1.0f);
    st.vignette_intensity = vign;
}

} // namespace ptv
