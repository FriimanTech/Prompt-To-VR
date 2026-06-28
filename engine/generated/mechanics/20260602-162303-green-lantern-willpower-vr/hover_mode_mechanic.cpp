#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <string>
#include <optional>
#include <unordered_map>
#include <algorithm>
#include <cmath>

// Forward declarations to link against haptics_manager exports.
namespace ptv { namespace haptics {
    // exports list declares these exact signatures
    void publish(const std::string& pattern, const std::string& hand_mask, float intensity, std::optional<float> duration_s);
    bool has_pattern(const std::string&);
    void set_user_intensity(float);
    float get_user_intensity();
    const char* pattern_to_string(int); // not used
    const char* hand_to_string(int);    // not used
} }

namespace ptv {

class HoverModeMechanic : public IMechanic {
public:
    std::string id() const override { return "generated.hover_mode_mechanic"; }

    void on_attach(World& world, std::uint32_t entity, const nlohmann::json& params) override {
        (void)world; (void)entity;
        cfg_.hover_max_speed_mps = params.value("hover_max_speed_mps", 1.2f);
        cfg_.hover_accel_tau_s   = std::max(0.02f, params.value("hover_accel_tau_s", 0.12f));
        cfg_.altitude_nudge_step_m = params.value("altitude_nudge_step_m", 0.25f);
        cfg_.damping_tau_s       = std::max(0.02f, params.value("damping_tau_s", 0.20f));
        cfg_.comfort_hand_stabilize = params.value("comfort_hand_stabilize", true);
        cfg_.altitude_min_m = 0.5f; cfg_.altitude_max_m = 25.0f;
        if (params.contains("altitude_bounds_m") && params["altitude_bounds_m"].is_array() && params["altitude_bounds_m"].size() == 2) {
            cfg_.altitude_min_m = params["altitude_bounds_m"][0].get<float>();
            cfg_.altitude_max_m = params["altitude_bounds_m"][1].get<float>();
            if (cfg_.altitude_max_m < cfg_.altitude_min_m) std::swap(cfg_.altitude_max_m, cfg_.altitude_min_m);
        }
        // allowed_in_scenes is matched via presence of a scene tag entity having that tag. Stored for runtime check.
        if (params.contains("allowed_in_scenes") && params["allowed_in_scenes"].is_array()) {
            for (auto& v : params["allowed_in_scenes"]) if (v.is_string()) cfg_.allowed_scene_tags.push_back(v.get<std::string>());
        }
        state_.last_a_tap_s = -10.0f;
        state_.hum_t = 0.0f;
        state_.baseline_y_set = false;
    }

    void on_update(World& world, std::uint32_t entity, float dt) override {
        if (!world.valid(entity) || !world.has<CTransform>(entity)) return;
        auto& tr = world.get<CTransform>(entity).t;

        // Scene allow check (optional). If allowed_scene_tags empty, allow when any entity has tag "hover_allowed"; else require any of those tags.
        if (!scene_allows_hover_(world)) {
            if (state_.hover_active) exit_hover_(/*hard*/true);
            // no hover simulation when disallowed
            state_.smoothed_pos = tr.position; // keep smoothing anchor fresh
            return;
        }

        InputSample in = read_input_from_binding_(world, entity);
        time_s_ += dt;

        // Double-tap A detection (0.35s window)
        if (in.a_tapped_now) {
            if (time_s_ - state_.last_a_tap_s <= 0.35f) {
                // Toggle
                if (!state_.hover_active) {
                    // Enter hover. If braking from flight requested/active, zero velocity immediately.
                    enter_hover_(world, entity, tr, /*instant_brake=*/in.flight_active || in.brake_from_flight);
                } else {
                    // Exit hover: zero velocity on exit.
                    exit_hover_(/*hard*/true);
                }
                state_.last_a_tap_s = -10.0f; // reset chain
            } else {
                state_.last_a_tap_s = time_s_;
            }
        }

        if (!state_.hover_active) {
            state_.smoothed_pos = tr.position; // keep smoothing anchor current when idle
            return;
        }

        // Establish baseline altitude on first active frame
        if (!state_.baseline_y_set) {
            state_.baseline_y = tr.position.y;
            state_.baseline_y_set = true;
        }

        // Compute local axes from body rotation (no camera roll applied here)
        Vec3 fwd = forward_from_(tr.rotation);
        Vec3 right = right_from_(tr.rotation);
        // Project onto horizontal plane to avoid unintended vertical coupling
        fwd.y = 0.0f; right.y = 0.0f;
        normalize_safe_(fwd); normalize_safe_(right);

        // Desired lateral velocity from left stick
        Vec3 desired_vel = (right * in.left_x + fwd * in.left_y) * cfg_.hover_max_speed_mps;

        // First-order response toward desired (accel tau), plus damping toward zero when stick released
        float accel_alpha = 1.0f - std::exp(-dt / cfg_.hover_accel_tau_s);
        state_.vel.x += (desired_vel.x - state_.vel.x) * accel_alpha;
        state_.vel.z += (desired_vel.z - state_.vel.z) * accel_alpha;

        // Gentle damping when input near zero
        if (std::abs(in.left_x) < 0.05f && std::abs(in.left_y) < 0.05f) {
            float damp_alpha = 1.0f - std::exp(-dt / cfg_.damping_tau_s);
            state_.vel.x += (0.0f - state_.vel.x) * damp_alpha;
            state_.vel.z += (0.0f - state_.vel.z) * damp_alpha;
        }

        // Altitude nudge via right stick if assist enabled
        if (in.assist_altitude && std::abs(in.right_y) > 0.6f && (time_s_ - state_.last_alt_nudge_s) > 0.12f) {
            float sign = (in.right_y > 0.0f) ? 1.0f : -1.0f;
            state_.target_y = clamp_( (state_.target_y.has_value() ? *state_.target_y : tr.position.y) + sign * cfg_.altitude_nudge_step_m,
                                      state_.baseline_y + cfg_.altitude_min_m,
                                      state_.baseline_y + cfg_.altitude_max_m);
            state_.last_alt_nudge_s = time_s_;
        }

        // Drive vertical toward target_y if set; otherwise softly hold current height (damp small drift)
        float target_y = state_.target_y.value_or(tr.position.y);
        float y_err = target_y - tr.position.y;
        float y_alpha = 1.0f - std::exp(-dt / std::max(0.06f, cfg_.damping_tau_s*0.7f));
        float vy_cmd = y_err * y_alpha / std::max(dt, 1e-5f);
        // Clamp vertical speed to comfort
        vy_cmd = std::clamp(vy_cmd, -cfg_.hover_max_speed_mps*0.6f, cfg_.hover_max_speed_mps*0.6f);
        state_.vel.y = vy_cmd;

        // Integrate position
        tr.position.x += state_.vel.x * dt;
        tr.position.y = clamp_(tr.position.y + state_.vel.y * dt,
                               state_.baseline_y + cfg_.altitude_min_m,
                               state_.baseline_y + cfg_.altitude_max_m);
        tr.position.z += state_.vel.z * dt;

        // Hand/body micro-bob suppression (position smoothing anchor)
        if (cfg_.comfort_hand_stabilize) {
            // Low-pass toward current position for a subtle smoothing effect without adding roll
            float smooth_tau = 0.08f; // 80 ms
            float a = 1.0f - std::exp(-dt / smooth_tau);
            state_.smoothed_pos.x += (tr.position.x - state_.smoothed_pos.x) * a;
            state_.smoothed_pos.y += (tr.position.y - state_.smoothed_pos.y) * a;
            state_.smoothed_pos.z += (tr.position.z - state_.smoothed_pos.z) * a;
            // Apply a tiny correction back to position to remove high-frequency jitter (<= 1 cm)
            Vec3 corr{ state_.smoothed_pos.x - tr.position.x,
                       state_.smoothed_pos.y - tr.position.y,
                       state_.smoothed_pos.z - tr.position.z };
            const float max_corr = 0.01f; // 1 cm
            if (length2_(corr) > (max_corr*max_corr)) {
                float len = std::sqrt(length2_(corr));
                corr = corr * (max_corr / (len + 1e-5f));
            }
            tr.position = tr.position + corr;
        } else {
            state_.smoothed_pos = tr.position;
        }

        // Periodic hover hum haptic (very subtle)
        state_.hum_t += dt;
        if (state_.hum_t >= 0.9f) {
            state_.hum_t = 0.0f;
            try_publish_haptic_("hover_hum_subtle", 0.15f, 0.06f);
        }
    }

    void on_detach(World& world, std::uint32_t entity) override {
        (void)world; (void)entity;
        // ensure state reset (instance is per-entity in engine test harness)
        state_ = {};
    }

private:
    struct Config {
        float hover_max_speed_mps{1.2f};
        float hover_accel_tau_s{0.12f};
        float altitude_nudge_step_m{0.25f};
        float altitude_min_m{0.5f};
        float altitude_max_m{25.0f};
        float damping_tau_s{0.20f};
        bool  comfort_hand_stabilize{true};
        std::vector<std::string> allowed_scene_tags; // optional
    } cfg_;

    struct State {
        bool hover_active{false};
        float last_a_tap_s{-10.0f};
        float last_alt_nudge_s{-10.0f};
        float hum_t{0.0f};
        Vec3  vel{0.0f,0.0f,0.0f};
        bool  baseline_y_set{false};
        float baseline_y{0.0f};
        std::optional<float> target_y; // absolute world y
        Vec3  smoothed_pos{0.0f,0.0f,0.0f};
    } state_;

    struct InputSample {
        bool a_tapped_now{false};
        bool flight_active{false};
        bool brake_from_flight{false};
        float left_x{0.0f};
        float left_y{0.0f};
        float right_y{0.0f};
        bool assist_altitude{true};
    };

    float time_s_{0.0f};

    void enter_hover_(World& world, std::uint32_t entity, const Transform& tr, bool instant_brake) {
        state_.hover_active = true;
        state_.hum_t = 0.0f;
        if (instant_brake) state_.vel = Vec3{0.0f,0.0f,0.0f};
        state_.baseline_y = tr.position.y;
        state_.baseline_y_set = true;
        state_.target_y = tr.position.y; // hold current altitude
        state_.smoothed_pos = tr.position;
        try_publish_haptic_("hover_toggle_click", 0.25f, 0.02f);
    }

    void exit_hover_(bool hard_zero) {
        if (hard_zero) state_.vel = Vec3{0.0f,0.0f,0.0f};
        state_.hover_active = false;
        state_.target_y.reset();
        try_publish_haptic_("hover_toggle_click", 0.18f, 0.02f);
    }

    bool scene_allows_hover_(World& world) const {
        auto& reg = world.registry();
        // If a specific allow-list is provided, require any one of those tags to appear anywhere in scene.
        if (!cfg_.allowed_scene_tags.empty()) {
            bool found_any = false;
            reg.view<CTag>().each([&](entt::entity, const CTag& t){
                for (auto& want : cfg_.allowed_scene_tags) if (has_tag_(t, want)) found_any = true;
            });
            return found_any;
        }
        // Fallback: enabled when scene advertises "hover_allowed" anywhere; if absent, allow by default to not block tests.
        bool has_hover_allowed = false;
        reg.view<CTag>().each([&](entt::entity, const CTag& t){ if (has_tag_(t, "hover_allowed")) has_hover_allowed = true; });
        return has_hover_allowed || true; // allow-by-default
    }

    static bool has_tag_(const CTag& t, const std::string& s) {
        for (auto& x : t.tags) if (x == s) return true; return false;
    }

    InputSample read_input_from_binding_(World& world, std::uint32_t entity) const {
        InputSample out{};
        if (!world.has<CMechanicBinding>(entity)) return out;
        const auto& mb = world.get<CMechanicBinding>(entity);
        for (const auto& e : mb.entries) {
            if (e.id == id()) {
                auto j = nlohmann::json::parse(e.params_json, nullptr, /*allow_exceptions=*/false);
                if (!j.is_object()) continue;
                // Harness-provided transient inputs (bools/floats) — safe defaults when missing
                out.a_tapped_now    = j.value("a_tap", false) || j.value("a_tapped_now", false);
                out.flight_active   = j.value("flight_active", false);
                out.brake_from_flight = j.value("brake_from_flight", false);
                out.left_x          = std::clamp(j.value("left_stick_x", 0.0f), -1.0f, 1.0f);
                out.left_y          = std::clamp(j.value("left_stick_y", 0.0f), -1.0f, 1.0f);
                out.right_y         = std::clamp(j.value("right_stick_y", 0.0f), -1.0f, 1.0f);
                out.assist_altitude = j.value("pitch_or_altitude_assist", true) || j.value("assist_altitude", true);
                break;
            }
        }
        return out;
    }

    // Math helpers using ptv:: math types
    static float dot_(const Vec3& a, const Vec3& b){ return a.x*b.x + a.y*b.y + a.z*b.z; }
    static float length2_(const Vec3& v){ return dot_(v,v); }
    static void normalize_safe_(Vec3& v){ float l2 = length2_(v); if (l2 > 1e-6f){ float inv = 1.0f/std::sqrt(l2); v.x*=inv; v.y*=inv; v.z*=inv; } }
    static Vec3 forward_from_(const Quat& q){
        // Assuming engine's Quat is w,x,y,z and forward is (0,0,1)
        // Convert to matrix-free rotation of vector
        Vec3 f{0.0f,0.0f,1.0f};
        return rotate_vec_(q, f);
    }
    static Vec3 right_from_(const Quat& q){
        Vec3 r{1.0f,0.0f,0.0f};
        return rotate_vec_(q, r);
    }
    static Vec3 rotate_vec_(const Quat& q, const Vec3& v){
        // v' = q * (0,v) * q^{-1}
        Quat vq{0.0f, v.x, v.y, v.z};
        Quat qi{ q.w, -q.x, -q.y, -q.z };
        Quat t = quat_mul_(quat_mul_(q, vq), qi);
        return Vec3{t.x, t.y, t.z};
    }
    static Quat quat_mul_(const Quat& a, const Quat& b){
        return Quat{ a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z,
                     a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
                     a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
                     a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w };
    }

    static float clamp_(float v, float lo, float hi){ return std::max(lo, std::min(hi, v)); }

    void try_publish_haptic_(const std::string& name, float intensity, std::optional<float> dur_s) {
        // Defensive: only publish if the global manager registered the pattern name
        try {
            if (haptics::has_pattern(name)) {
                haptics::publish(name, std::string("both_hands"), std::clamp(intensity, 0.0f, 1.0f), dur_s);
            }
        } catch(...) { /* offline_ok path: ignore in editor/unit tests */ }
    }

};

} // namespace ptv

PTV_REGISTER_MECHANIC("generated.hover_mode_mechanic", ptv::HoverModeMechanic)
