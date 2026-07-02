#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <cmath>
#include <unordered_map>
#include <string>

#include "hover_mode_shared.h"

using nlohmann::json;
using namespace hover_internal;

namespace ptv {

class HoverModeMechanic : public IMechanic {
public:
    void on_attach(World& w, std::uint32_t e, const json& params) override {
        (void)w; (void)e;
        params_ = parse_params_object(params);
        // Fresh state; baseline captured on first activation
    }

    void on_update(World& w, std::uint32_t e, float dt) override {
        auto& st = state_[e];
        st.t_now += (dt > 0.0f ? dt : w.delta_time());

        // Inputs each frame
        FrameInput in = read_frame_input(w, e, id().c_str());

        // Respect scene allowlist if provided
        bool allowed = scene_allowed(params_, in.scene_id);

        // Double-tap A to toggle
        if (in.a_pressed) {
            if (st.t_now - st.last_a_press_t <= st.a_double_window_s && allowed) {
                st.active = !st.active;
                // Toggle haptics click
                publish_haptics(w, e, kPatternClick, "right", 0.35f, 0.05f);
                publish_haptics(w, e, kPatternClick, "left", 0.25f, 0.05f);
                if (st.active) {
                    // Enter hover: instant brake, capture baseline, enable stabilizer tag
                    st.vx = 0.0f; st.vz = 0.0f;
                    if (w.has<CTransform>(e)) st.altitude_baseline_y = w.get<CTransform>(e).t.position.y;
                    write_aim_stabilizer_tag(w, e, params_.comfort_hand_stabilize, params_.comfort_hand_stabilize ? 0.6f : 0.0f);
                    write_hover_mode_tag(w, e, true);
                    publish_haptics(w, e, kPatternHoverLight, "both", 0.55f, 0.12f);
                } else {
                    // Exit hover: zero velocity, disable stabilizer, clear mode
                    st.vx = 0.0f; st.vz = 0.0f;
                    write_velocity_tag(w, e, 0.0f, 0.0f, 0.0f);
                    write_aim_stabilizer_tag(w, e, false, 0.0f);
                    write_hover_mode_tag(w, e, false);
                }
            }
            st.last_a_press_t = st.t_now;
        }

        if (!st.active || !allowed) {
            // Drift to stop if we somehow had residual velocity
            if ((std::fabs(st.vx) + std::fabs(st.vz)) > 1e-4f) {
                st.vx = lowpass_step(st.vx, 0.0f, dt, params_.damping_tau_s);
                st.vz = lowpass_step(st.vz, 0.0f, dt, params_.damping_tau_s);
                write_velocity_tag(w, e, st.vx, 0.0f, st.vz);
            }
            return;
        }

        // While active: desired lateral velocity from left stick (world XZ). Clamp to max speed.
        float mag = std::sqrt(in.left_x*in.left_x + in.left_y*in.left_y);
        if (mag > 1.0f) { in.left_x /= mag; in.left_y /= mag; }
        float target_vx = params_.hover_max_speed_mps * in.left_x;
        float target_vz = params_.hover_max_speed_mps * in.left_y;

        // Accelerate / decelerate with tau
        st.vx = lowpass_step(st.vx, target_vx, dt, params_.hover_accel_tau_s);
        st.vz = lowpass_step(st.vz, target_vz, dt, params_.hover_accel_tau_s);

        // Extra damping toward zero when sticks released
        if (mag < 0.05f) {
            st.vx = lowpass_step(st.vx, 0.0f, dt, params_.damping_tau_s);
            st.vz = lowpass_step(st.vz, 0.0f, dt, params_.damping_tau_s);
        }

        // Apply to transform position each tick (local, since no physics integrator exposed here)
        if (w.has<CTransform>(e)) {
            auto& tr = w.get<CTransform>(e).t;
            tr.position.x += st.vx * dt;
            tr.position.z += st.vz * dt;

            // Altitude nudge by right stick if assist
            float vy = 0.0f;
            if (in.altitude_assist) {
                const float thr = 0.60f;
                if (in.right_y > thr) {
                    tr.position.y += params_.altitude_nudge_step_m;
                } else if (in.right_y < -thr) {
                    tr.position.y -= params_.altitude_nudge_step_m;
                }
                // Clamp within baseline +/- bounds
                float minY = st.altitude_baseline_y + params_.altitude_min_m;
                float maxY = st.altitude_baseline_y + params_.altitude_max_m;
                tr.position.y = clampf(tr.position.y, minY, maxY);
                // vy only used for tag telemetry this frame (0 except when we stepped)
                // We approximate using difference to baseline sign if needed; keep 0 for now.
                vy = 0.0f;
            }

            // Write velocity telemetry tag for interop
            write_velocity_tag(w, e, st.vx, 0.0f /*vy*/, st.vz);
        }

        // Periodic subtle hover hum while active
        st.hum_pulse_timer += dt;
        if (st.hum_pulse_timer >= 0.8f) {
            st.hum_pulse_timer = 0.0f;
            publish_haptics(w, e, kPatternHoverLight, "both", 0.25f, 0.06f);
        }
    }

    void on_detach(World& w, std::uint32_t e) override {
        // Clean up our tags
        remove_tag_with_prefix(w, e, "locomotion.mode:");
        remove_tag_with_prefix(w, e, "locomotion.velocity:");
        remove_tag_with_prefix(w, e, "aim.stabilizer:");
        state_.erase(e);
    }

    std::string id() const override { return "generated.hover_mode_mechanic"; }

private:
    HoverParams params_{};
    std::unordered_map<std::uint32_t, HoverState> state_;
};

} // namespace ptv

PTV_REGISTER_MECHANIC("generated.hover_mode_mechanic", ptv::HoverModeMechanic)
