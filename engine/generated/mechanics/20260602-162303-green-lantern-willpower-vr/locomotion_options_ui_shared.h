#pragma once

#include "ptv/ecs/World.h"
#include <nlohmann/json.hpp>
#include <string>
#include <algorithm>

namespace ptv {

struct LocomotionSettingsState {
    // toggles
    bool enable_teleport{true};
    bool enable_slide_head{false};
    bool enable_slide_controller{false};

    // turn
    std::string turn_mode{"snap"}; // "snap" | "smooth"
    float snap_angle_deg{45.0f};     // 30/45/60
    float smooth_turn_speed_degps{120.0f}; // 90..240

    // comfort + calibration
    float vignette_intensity{0.0f};  // 0..1
    float height_offset_m{0.0f};     // -0.3 .. +0.3
};

// Safe add-tag helper: ensures CTag exists and pushes a string tag.
inline void add_tag(World& w, std::uint32_t e, const std::string& s) {
    if (!w.has<CTag>(e)) {
        CTag t; t.tags.reserve(8); t.tags.push_back(s);
        w.emplace_or_replace<CTag>(e, std::move(t));
    } else {
        auto& t = w.get<CTag>(e);
        // avoid unbounded duplication of identical consecutive tags
        if (t.tags.empty() || t.tags.back() != s) {
            t.tags.push_back(s);
        }
    }
}

// Reads params for this mechanic (from its own CMechanicBinding entry) and fills state.
// Missing keys keep defaults; out-of-range values are clamped to spec.
inline void read_settings_from_params(const nlohmann::json& j, LocomotionSettingsState& out) {
    if (!j.is_object()) return;
    if (j.contains("enable_teleport")) out.enable_teleport = j.at("enable_teleport").get<bool>();
    if (j.contains("enable_slide_head")) out.enable_slide_head = j.at("enable_slide_head").get<bool>();
    if (j.contains("enable_slide_controller")) out.enable_slide_controller = j.at("enable_slide_controller").get<bool>();

    if (j.contains("turn_mode")) out.turn_mode = j.at("turn_mode").get<std::string>();
    if (out.turn_mode != "snap" && out.turn_mode != "smooth") out.turn_mode = "snap";

    if (j.contains("snap_angle_deg")) out.snap_angle_deg = j.at("snap_angle_deg").get<float>();
    // snap allowed: 30/45/60; snap to nearest
    {
        float cand = out.snap_angle_deg;
        float opts[3] = {30.0f, 45.0f, 60.0f};
        float best = 45.0f; float bd = 1e9f;
        for (float o: opts) { float d = std::abs(o - cand); if (d < bd) { bd = d; best = o; } }
        out.snap_angle_deg = best;
    }

    if (j.contains("smooth_turn_speed_degps")) out.smooth_turn_speed_degps = j.at("smooth_turn_speed_degps").get<float>();
    out.smooth_turn_speed_degps = std::clamp(out.smooth_turn_speed_degps, 90.0f, 240.0f);

    if (j.contains("vignette_intensity")) out.vignette_intensity = j.at("vignette_intensity").get<float>();
    out.vignette_intensity = std::clamp(out.vignette_intensity, 0.0f, 1.0f);

    if (j.contains("height_offset_m")) out.height_offset_m = j.at("height_offset_m").get<float>();
    out.height_offset_m = std::clamp(out.height_offset_m, -0.3f, 0.3f);

    // Safety: ensure at least one locomotion is enabled
    if (!out.enable_teleport && !out.enable_slide_head && !out.enable_slide_controller) {
        out.enable_teleport = true; // comfort-safe default
    }
}

// Extract this mechanic's params json (if present) from the binding entry list and parse.
inline nlohmann::json params_for_mechanic_from_binding(const CMechanicBinding& mb, const std::string& mech_id) {
    for (const auto& e : mb.entries) {
        // prefer explicit id match; fall back to builtin_id if used by tools
        if (e.id == mech_id || e.builtin_id == mech_id) {
            nlohmann::json j = nlohmann::json::parse(e.params_json, nullptr, false);
            if (j.is_discarded()) return nlohmann::json::object();
            return j;
        }
    }
    return nlohmann::json::object();
}

} // namespace ptv
