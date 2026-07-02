#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>

namespace ptv { class World; struct CTag; }

namespace hover_internal {

struct HoverParams {
    float hover_max_speed_mps = 1.2f;
    float hover_accel_tau_s = 0.12f;
    float altitude_nudge_step_m = 0.25f;
    float altitude_min_m = 0.5f;
    float altitude_max_m = 25.0f;
    float damping_tau_s = 0.20f;
    bool  comfort_hand_stabilize = true;
    // Scene allowlist; empty means allowed everywhere
    std::vector<std::string> allowed_in_scenes; 
};

struct FrameInput {
    // Buttons and sticks from the harness via params_json on the binding entry each frame
    bool a_pressed = false;         // rising-edge press this frame
    float left_x = 0.0f;            // -1..1
    float left_y = 0.0f;            // -1..1 (forward)
    float right_y = 0.0f;           // -1..1 (up when > 0)
    bool altitude_assist = true;    // allow right stick vertical nudges
    std::string scene_id;           // optional, for allowlist checks
};

struct HoverState {
    bool active = false;
    float t_now = 0.0f;                 // local time accumulator (s)
    float last_a_press_t = -1e9f;       // last press time (s)
    float a_double_window_s = 0.35f;    // double-tap window
    float hum_pulse_timer = 0.0f;       // for periodic hover hum haptics

    // velocity on XZ (world)
    float vx = 0.0f;
    float vz = 0.0f;

    // altitude baseline+current clamp
    float altitude_baseline_y = 0.0f;   // set on enter
};

inline float clampf(float v, float lo, float hi) {
    return std::max(lo, std::min(hi, v));
}

inline void upsert_tag(ptv::World& w, std::uint32_t e, const std::string& key, const std::string& value_json);
inline void remove_tag_with_prefix(ptv::World& w, std::uint32_t e, std::string_view prefix);

// Params parsing helpers
inline HoverParams parse_params_object(const nlohmann::json& p) {
    HoverParams hp;
    if (p.is_object()) {
        hp.hover_max_speed_mps = p.value("hover_max_speed_mps", hp.hover_max_speed_mps);
        hp.hover_accel_tau_s = p.value("hover_accel_tau_s", hp.hover_accel_tau_s);
        hp.altitude_nudge_step_m = p.value("altitude_nudge_step_m", hp.altitude_nudge_step_m);
        hp.damping_tau_s = p.value("damping_tau_s", hp.damping_tau_s);
        hp.comfort_hand_stabilize = p.value("comfort_hand_stabilize", hp.comfort_hand_stabilize);
        if (p.contains("altitude_bounds_m") && p["altitude_bounds_m"].is_array() && p["altitude_bounds_m"].size() == 2) {
            hp.altitude_min_m = p["altitude_bounds_m"][0].get<float>();
            hp.altitude_max_m = p["altitude_bounds_m"][1].get<float>();
            if (hp.altitude_min_m > hp.altitude_max_m) std::swap(hp.altitude_min_m, hp.altitude_max_m);
        }
        if (p.contains("allowed_in_scenes") && p["allowed_in_scenes"].is_array()) {
            for (auto& s : p["allowed_in_scenes"]) if (s.is_string()) hp.allowed_in_scenes.push_back(s.get<std::string>());
        }
    }
    return hp;
}

// First-order response toward target with time-constant tau
inline float lowpass_step(float current, float target, float dt, float tau) {
    if (tau <= 1e-5f) return target;
    float a = clampf(dt / tau, 0.0f, 1.0f);
    return current + a * (target - current);
}

// Reads per-frame inputs from CMechanicBinding entry for this mechanic id.
FrameInput read_frame_input(ptv::World& w, std::uint32_t e, const char* mechanic_id);

// Haptics helpers (strings from dependency exports; no header includes across tasks)
static constexpr const char* kHapticsPublishPrefix = "haptics.publish:";
static constexpr const char* kPatternHoverLight = "hover_light";
static constexpr const char* kPatternClick = "bolt_fire_sharp"; // reuse as toggle click

inline void publish_haptics(ptv::World& w, std::uint32_t e, const std::string& pattern_id, const char* hand, float amp, float dur_s);

// Locomotion interop via tags
// We write lightweight tags other systems can read:
//  - "locomotion.mode:{\"state\":\"hover\"}"
//  - "locomotion.velocity:{\"vx\":...,\"vy\":...,\"vz\":...}"
//  - "aim.stabilizer:{\"enabled\":true,\"strength\":0.6}"
inline void write_hover_mode_tag(ptv::World& w, std::uint32_t e, bool active);
inline void write_velocity_tag(ptv::World& w, std::uint32_t e, float vx, float vy, float vz);
inline void write_aim_stabilizer_tag(ptv::World& w, std::uint32_t e, bool enabled, float strength);

// Scene allowlist check
inline bool scene_allowed(const HoverParams& hp, const std::string& scene_id) {
    if (hp.allowed_in_scenes.empty()) return true;
    return std::find(hp.allowed_in_scenes.begin(), hp.allowed_in_scenes.end(), scene_id) != hp.allowed_in_scenes.end();
}

} // namespace hover_internal
