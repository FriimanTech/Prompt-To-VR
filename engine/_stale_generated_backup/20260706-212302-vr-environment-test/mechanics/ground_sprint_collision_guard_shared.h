#pragma once

#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"
#include <nlohmann/json.hpp>
#include <glm/glm.hpp>
#include <string>
#include <vector>
#include <cstdint>
#include <algorithm>

namespace ptv {

// Transient sprint state attached to the player entity
struct CSprintState {
    bool active{false};
    float time_in_s{0.0f};
    float cooldown_remaining_s{0.0f};
    float factor{1.0f};
    float vignette_intensity{0.0f};
};

// Local cache of last-known pose to layer deltas over builtin movement
struct CLastPoseGS {
    Vec3 last_pos{0.0f, 0.0f, 0.0f};
    float last_yaw_deg{0.0f};
    bool initialized{false};
};

// Soft settings snapshot parsed from bindings (locomotion_settings_menu)
struct CLocomotionSettingsCacheGS {
    bool smooth_turn_enabled{false};
    float smooth_turn_rate_dps{120.0f};
    bool vignette_enabled{false};
    float vignette_base{0.0f};
    float vignette_k{0.6f};
    float snap_turn_deg{45.0f};
};

// Helper: safe JSON bool read
inline bool json_get_bool(const nlohmann::json& j, const char* key, bool def_v) {
    if (j.is_object() && j.contains(key)) {
        try { return j.at(key).get<bool>(); } catch (...) { return def_v; }
    }
    return def_v;
}

// Helper: safe JSON float read
inline float json_get_float(const nlohmann::json& j, const char* key, float def_v) {
    if (j.is_object() && j.contains(key)) {
        try { return j.at(key).get<float>(); } catch (...) { return def_v; }
    }
    return def_v;
}

// Extract current input signals from CMechanicBinding entries.
// We accept any entry that carries these keys at runtime.
struct InputGS {
    glm::vec2 left_stick{0.0f, 0.0f};
    float right_stick_x{0.0f};
    bool left_stick_click{false};
    bool hover_active{false};
    bool flight_active{false};
};

inline InputGS read_inputs_from_bindings(World& w, std::uint32_t e) {
    InputGS out;
    if (!w.has<CMechanicBinding>(e)) return out;
    const auto& mb = w.get<CMechanicBinding>(e);
    for (const auto& ent : mb.entries) {
        if (ent.params_json.empty()) continue;
        auto j = nlohmann::json::parse(ent.params_json, nullptr, /*allow_exceptions=*/false);
        if (!j.is_object()) continue;
        // Stick vectors
        if (j.contains("left_stick")) {
            try {
                auto v = j.at("left_stick");
                if (v.is_array() && v.size() >= 2) {
                    out.left_stick.x = v[0].get<float>();
                    out.left_stick.y = v[1].get<float>();
                }
            } catch (...) {}
        }
        out.right_stick_x = std::max(out.right_stick_x, json_get_float(j, "right_stick_x", out.right_stick_x));
        // Buttons
        out.left_stick_click = out.left_stick_click || json_get_bool(j, "left_stick_click", false);
        // Mode gates possibly provided by hover/flight mechanics
        if (ent.id == "generated.hover_mode_mechanic" || ent.builtin_id == "generated.hover_mode_mechanic") {
            out.hover_active = out.hover_active || json_get_bool(j, "active", false);
        }
        if (ent.id == "generated.flight_mode_mechanic" || ent.builtin_id == "generated.flight_mode_mechanic") {
            out.flight_active = out.flight_active || json_get_bool(j, "active", false);
        }
    }
    return out;
}

inline CLocomotionSettingsCacheGS read_settings_from_bindings(World& w, std::uint32_t e, float snap_turn_fallback_deg) {
    CLocomotionSettingsCacheGS s;
    s.snap_turn_deg = snap_turn_fallback_deg;
    if (!w.has<CMechanicBinding>(e)) return s;
    const auto& mb = w.get<CMechanicBinding>(e);
    for (const auto& ent : mb.entries) {
        if (ent.id != "generated.locomotion_settings_menu") continue;
        auto j = nlohmann::json::parse(ent.params_json, nullptr, /*allow_exceptions=*/false);
        if (!j.is_object()) continue;
        s.smooth_turn_enabled = json_get_bool(j, "smooth_turn_enabled", s.smooth_turn_enabled);
        s.smooth_turn_rate_dps = json_get_float(j, "smooth_turn_rate_dps", s.smooth_turn_rate_dps);
        s.vignette_enabled = json_get_bool(j, "vignette_enabled", s.vignette_enabled);
        s.vignette_base = json_get_float(j, "vignette_base", s.vignette_base);
        s.vignette_k = json_get_float(j, "vignette_k", s.vignette_k);
        s.snap_turn_deg = json_get_float(j, "snap_turn_degrees", s.snap_turn_deg);
    }
    return s;
}

// Simple yaw apply to a Transform's quaternion around +Y axis
inline void apply_yaw_deg_to_transform(Transform& t, float yaw_deg) {
    // Convert degrees to radians
    const float rad = yaw_deg * 0.017453292519943295f;
    // Build yaw quaternion (w, x, y, z)
    Quat qyaw{std::cos(rad * 0.5f), 0.0f, std::sin(rad * 0.5f), 0.0f};
    // Hamilton product: qyaw * t.rotation
    const Quat& a = qyaw; const Quat& b = t.rotation;
    Quat r;
    r.w = a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z;
    r.x = a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y;
    r.y = a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x;
    r.z = a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w;
    t.rotation = r;
}

// Project movement against a simple spherical guard around constructs and slide along tangent.
inline glm::vec3 collision_guard_slide(World& w, const glm::vec3& current_pos, const glm::vec3& proposed_pos,
                                       float guard_radius_m, float pushback_strength, float max_penetration_m) {
    glm::vec3 adjusted = proposed_pos;
    auto& reg = w.registry();
    auto view = reg.view<CTag, CTransform>();
    for (auto entity : view) {
        const auto& tag = view.get<CTag>(entity);
        const auto& tr = view.get<CTransform>(entity).t;
        bool is_construct = false;
        for (const auto& s : tag.tags) {
            if (s == "construct_barrier" || s == "construct_platform" || s == "placed_construct") { is_construct = true; break; }
        }
        if (!is_construct) continue;
        glm::vec3 cpos{tr.position.x, tr.position.y, tr.position.z};
        glm::vec3 to_new = adjusted - cpos;
        float dist = std::sqrt(std::max(0.0f, glm::dot(to_new, to_new)));
        if (dist < guard_radius_m) {
            // Compute penetration
            float pen = guard_radius_m - dist;
            if (pen > 0.0f) {
                glm::vec3 n = (dist > 1e-4f) ? (to_new / dist) : glm::vec3(0,1,0);
                // Push out minimally (clamped)
                float resolve = std::min(pen, max_penetration_m);
                adjusted += n * (resolve + pushback_strength * resolve);
                // Slide along tangent: remove normal component from displacement (current->adjusted)
                glm::vec3 disp = adjusted - current_pos;
                float nd = glm::dot(disp, n);
                adjusted = current_pos + (disp - n * nd);
            }
        }
    }
    return adjusted;
}

// Exponential approach helper
inline float exp_approach(float current, float target, float tau_s, float dt) {
    if (tau_s <= 1e-5f) return target;
    float a = std::exp(-dt / tau_s);
    return target + (current - target) * a;
}

} // namespace ptv
