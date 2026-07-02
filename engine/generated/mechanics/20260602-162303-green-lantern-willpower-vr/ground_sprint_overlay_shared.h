#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <algorithm>

namespace ptv {

struct SprintTunables {
    float speed_multiplier{1.6f};
    float accel_tau_s{0.10f};
    float decel_tau_s{0.18f};
    float max_duration_s{2.0f};
    float cooldown_s{2.0f};
    bool vignette_on{true};
    float vignette_override{-1.0f};
    float deadzone_override{-1.0f};
};

struct SprintState {
    bool sprint_active{false};
    bool prev_click{false};
    float t_in_sprint{0.0f};
    float cooldown_t{0.0f};
    float speed_scalar{1.0f};
    float vignette_level{0.0f};
};

// Exponential smoothing towards target with time-constant tau (seconds). tau<=0 -> snap
inline float approach_exp(float current, float target, float dt, float tau) {
    if (tau <= 0.0f) return target;
    float a = 1.0f - std::exp(-dt / tau);
    return current + (target - current) * std::clamp(a, 0.0f, 1.0f);
}

// Safe JSON getters
inline bool j_get_bool(const nlohmann::json& j, const char* key, bool defv) {
    if (!j.is_object() || !j.contains(key)) return defv;
    try { return j.at(key).get<bool>(); } catch (...) { return defv; }
}
inline float j_get_float(const nlohmann::json& j, const char* key, float defv) {
    if (!j.is_object() || !j.contains(key)) return defv;
    try { return j.at(key).get<float>(); } catch (...) { return defv; }
}

// Build a small JSON string quickly
inline std::string make_json_2f(const char* k0, float v0, const char* k1, float v1) {
    nlohmann::json j; j[k0]=v0; j[k1]=v1; return j.dump();
}

} // namespace ptv
