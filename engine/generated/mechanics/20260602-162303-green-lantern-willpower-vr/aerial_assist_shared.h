#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <unordered_map>
#include <cmath>

#include "ptv/math/Math.h"

namespace ptv {

struct AerialAssistParams {
    bool enable{true};
    float lead_stiffness{0.6f};            // smoothing for reticle
    float magnetism_cone_deg{5.0f};        // cone within which to apply aim nudge
    float magnetism_strength{0.15f};       // 0..1 blend towards assist dir
    float altitude_nudge_step_m{0.2f};
    float projectile_speed_mps{20.0f};     // default; kept local to this mechanic
};

struct TargetSample {
    ptv::Vec3 pos{0.0f};
    ptv::Vec3 vel{0.0f};
    float     dist{1e9f};
    bool      valid{false};
};

struct AerialAssistState {
    // Smoothed anchor for reticle stabilization
    ptv::Vec3 filtered_anchor{0.0f};
    bool      has_anchor{false};

    // Cached settings
    AerialAssistParams p{};

    // Button edge tracking for altitude taps
    bool rs_up_last{false};
    bool rs_down_last{false};

    // Momentary refine (Left Trigger) last state
    bool lt_last{false};
};

// Simple helpers (no engine math beyond ptv::Vec3)
inline float dot(const ptv::Vec3& a, const ptv::Vec3& b){ return a.x*b.x + a.y*b.y + a.z*b.z; }
inline float len(const ptv::Vec3& a){ return std::sqrt(dot(a,a)); }
inline ptv::Vec3 norm(const ptv::Vec3& a){ float L = len(a); return (L>1e-6f)? ptv::Vec3{a.x/L,a.y/L,a.z/L} : ptv::Vec3{0.0f,0.0f,0.0f}; }
inline ptv::Vec3 lerp(const ptv::Vec3& a, const ptv::Vec3& b, float t){ return ptv::Vec3{ a.x + (b.x-a.x)*t, a.y + (b.y-a.y)*t, a.z + (b.z-a.z)*t }; }
inline float clamp(float v, float lo, float hi){ return std::fmax(lo, std::fmin(hi, v)); }

// Intercept solver: r = target_pos - shooter_pos; v = target_vel; s = projectile speed.
// Solve (v·v - s^2) t^2 + 2 (r·v) t + r·r = 0; pick smallest positive t. Returns t or std::nullopt.
inline std::optional<float> lead_time(const ptv::Vec3& r, const ptv::Vec3& v, float s){
    float a = dot(v,v) - s*s;
    float b = 2.0f * dot(r,v);
    float c = dot(r,r);
    const float eps = 1e-6f;
    if (std::fabs(a) < eps) {
        // Linear: s ~= |v|
        if (std::fabs(b) < eps) return std::nullopt;
        float t = -c / b;
        if (t > 0.0f) return t; else return std::nullopt;
    }
    float disc = b*b - 4.0f*a*c;
    if (disc < 0.0f) return std::nullopt;
    float sqrtD = std::sqrt(disc);
    float t1 = (-b - sqrtD) / (2.0f*a);
    float t2 = (-b + sqrtD) / (2.0f*a);
    float t  = 1e9f;
    if (t1 > 0.0f) t = std::min(t, t1);
    if (t2 > 0.0f) t = std::min(t, t2);
    if (t < 1e8f) return t; else return std::nullopt;
}

// Cone check: returns cosine threshold once for given degrees
inline float cos_deg(float d){ return std::cos(d * 3.1415926535f / 180.0f); }

// Per-entity mechanic state store
struct AerialAssistStore {
    std::unordered_map<std::uint32_t, AerialAssistState> map;
};

} // namespace ptv
