#pragma once

#include <string>
#include <nlohmann/json.hpp>
#include <vector>
#include <algorithm>
#include <cstdint>

namespace ptv {

struct AccessibilityState {
    // Comfort / locomotion
    float vignette01{0.35f};
    std::string turn_mode{"snap"}; // "snap" | "smooth"
    float smooth_turn_speed_dps{120.0f};
    float snap_turn_degrees{45.0f};
    float flight_assist01{0.5f};

    // Haptics / UI
    float haptics_amp01{1.0f};
    std::string subtitles_size{"medium"}; // small|medium|large
    std::string colorblind_mode{"off"}; // off|protan|deutan|tritan
    float motion_cue_opacity01{0.6f};

    // Seated / calibration
    float seated_height_offset_m{0.0f};

    bool any_changed{false};

    static AccessibilityState from_defaults_json(const nlohmann::json& j) {
        AccessibilityState s;
        if (!j.is_object()) return s;
        s.vignette01              = std::clamp(j.value("vignette_intensity01", s.vignette01), 0.0f, 1.0f);
        s.turn_mode               = j.value("turn_type", s.turn_mode);
        if (s.turn_mode != "snap" && s.turn_mode != "smooth") s.turn_mode = "snap";
        s.smooth_turn_speed_dps   = j.value("turn_speed_deg_per_s", s.smooth_turn_speed_dps);
        s.snap_turn_degrees       = j.value("snap_turn_degrees", s.snap_turn_degrees);
        s.flight_assist01         = std::clamp(j.value("flight_assist01", s.flight_assist01), 0.0f, 1.0f);
        s.haptics_amp01           = std::clamp(j.value("haptics_amp01", s.haptics_amp01), 0.0f, 1.0f);
        s.subtitles_size          = j.value("subtitles_size", s.subtitles_size);
        s.colorblind_mode         = j.value("colorblind_mode", s.colorblind_mode);
        s.seated_height_offset_m  = j.value("seated_height_offset_m", s.seated_height_offset_m);
        s.motion_cue_opacity01    = std::clamp(j.value("motion_cue_opacity01", s.motion_cue_opacity01), 0.0f, 1.0f);
        return s;
    }

    nlohmann::json to_json() const {
        nlohmann::json j;
        j["vignette_intensity01"] = vignette01;
        j["turn_type"] = turn_mode;
        j["turn_speed_deg_per_s"] = smooth_turn_speed_dps;
        j["snap_turn_degrees"] = snap_turn_degrees;
        j["flight_assist01"] = flight_assist01;
        j["haptics_amp01"] = haptics_amp01;
        j["subtitles_size"] = subtitles_size;
        j["colorblind_mode"] = colorblind_mode;
        j["seated_height_offset_m"] = seated_height_offset_m;
        j["motion_cue_opacity01"] = motion_cue_opacity01;
        return j;
    }
};

inline void apply_updates_from_params(AccessibilityState& s, const nlohmann::json& j) {
    if (!j.is_object()) return;
    auto clamp01 = [](float v){ return std::clamp(v, 0.0f, 1.0f); };

    auto upd_f = [&](const char* key, float& dst, bool clamp=false){
        if (j.contains(key)) { float nv = j.at(key).get<float>(); if (clamp) nv = clamp01(nv); if (nv != dst) { dst = nv; s.any_changed = true; } }
    };
    auto upd_s = [&](const char* key, std::string& dst, std::vector<std::string> allowed = {}){
        if (j.contains(key)) {
            std::string nv = j.at(key).get<std::string>();
            if (!allowed.empty()) {
                if (std::find(allowed.begin(), allowed.end(), nv) == allowed.end()) return; // ignore invalid
            }
            if (nv != dst) { dst = nv; s.any_changed = true; }
        }
    };

    upd_f("set_vignette01", s.vignette01, true);
    upd_s("set_turn_type", s.turn_mode, {"snap","smooth"});
    upd_f("set_turn_speed_dps", s.smooth_turn_speed_dps, false);
    upd_f("set_snap_degrees", s.snap_turn_degrees, false);
    upd_f("set_flight_assist01", s.flight_assist01, true);
    upd_f("set_haptics_amp01", s.haptics_amp01, true);
    upd_s("set_subtitles_size", s.subtitles_size, {"small","medium","large"});
    upd_s("set_colorblind_mode", s.colorblind_mode, {"off","protan","deutan","tritan"});
    if (j.contains("nudge_seated_height_m")) { s.seated_height_offset_m += j.at("nudge_seated_height_m").get<float>(); s.any_changed = true; }
    upd_f("set_seated_height_m", s.seated_height_offset_m, false);
    upd_f("set_motion_cue_opacity01", s.motion_cue_opacity01, true);
}

} // namespace ptv
