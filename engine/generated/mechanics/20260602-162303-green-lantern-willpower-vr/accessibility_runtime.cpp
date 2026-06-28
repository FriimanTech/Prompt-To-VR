#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <optional>
#include <algorithm>
#include <cstdint>

// Forward declarations to depend-on exports (no extra headers allowed)
namespace ptv { namespace haptics { float get_user_intensity(); void set_user_intensity(float); } }
namespace ptv { namespace hud_helpers { void hud_get_settings_json(std::string& out); } }
namespace ptv { namespace persistence_sections { void merge_shallow(const std::string& section, const std::string& json_blob); } }

namespace ptv { namespace accessibility_runtime_detail {

static void set_tag_kv(World& w, std::uint32_t e, const std::string& key, const std::string& value) {
    if (!w.valid(e)) return;
    if (!w.has<CTag>(e)) {
        w.emplace_or_replace<CTag>(e, CTag{});
    }
    auto& tags = w.get<CTag>(e).tags;
    const std::string prefix = key + "=";
    // overwrite existing key if present
    bool replaced = false;
    for (auto& t : tags) {
        if (t.rfind(prefix, 0) == 0) { t = prefix + value; replaced = true; break; }
    }
    if (!replaced) tags.push_back(prefix + value);
}

static std::string f2(const float v) {
    char buf[32];
    // compact formatting
    std::snprintf(buf, sizeof(buf), "%.3g", v);
    return std::string(buf);
}

// Clamp helper
static float clamp(float v, float a, float b){ return std::max(a, std::min(v, b)); }

} } // namespaces

namespace ptv { namespace accessibility_internal {

void apply_to_world(World& w, std::uint32_t e, const nlohmann::json& st) {
    using namespace accessibility_runtime_detail;
    // Comfort/vignette bus (shared with locomotion_options_ui and sprint overlay)
    float vignette = clamp(st.value("vignette_intensity", 0.45f), 0.0f, 1.0f);
    set_tag_kv(w, e, "comfort_vignette", f2(vignette));

    // Turn mode and parameters (UI and locomotion systems read these tags)
    std::string turn_mode = st.value("turn_mode", std::string("snap"));
    if (turn_mode != "snap" && turn_mode != "smooth") turn_mode = "snap";
    set_tag_kv(w, e, "turn_mode", turn_mode);

    float snap_angle = clamp(st.value("snap_angle_deg", 45.0f), 5.0f, 120.0f);
    set_tag_kv(w, e, "snap_angle", f2(snap_angle));

    float smooth_speed = clamp(st.value("smooth_turn_speed_degps", 120.0f), 30.0f, 360.0f);
    set_tag_kv(w, e, "smooth_turn_speed_degps", f2(smooth_speed));

    // Flight assist (flight_system can read if present)
    float assist = clamp(st.value("flight_assist_strength", 0.5f), 0.0f, 1.0f);
    set_tag_kv(w, e, "flight_assist_strength", f2(assist));

    // Haptics user intensity (call global service and mirror on tag for UI reflection)
    float hap = clamp(st.value("haptics_user_intensity", 0.8f), 0.0f, 1.0f);
    ptv::haptics::set_user_intensity(hap);
    set_tag_kv(w, e, "haptics_user_intensity", f2(hap));

    // Subtitles and colorblind mode (read by HUD)
    float sub_scale = clamp(st.value("subtitle_scale", 1.0f), 0.5f, 2.0f);
    set_tag_kv(w, e, "subtitle_scale", f2(sub_scale));

    std::string cbm = st.value("colorblind_mode", std::string("none"));
    // normalize a few common aliases
    if (cbm == "deuteranopia") cbm = "deutan"; else if (cbm == "protanopia") cbm = "protan"; else if (cbm == "tritanopia") cbm = "tritan";
    set_tag_kv(w, e, "colorblind_mode", cbm);

    // Seated height offset (player rig/hud can apply this)
    float yoff = clamp(st.value("seated_y_offset_m", 0.0f), -0.5f, 0.5f);
    set_tag_kv(w, e, "seated_y_offset_m", f2(yoff));

    // Motion cue opacity (screen-space/construct cues)
    float cue = clamp(st.value("motion_cue_opacity", 0.6f), 0.0f, 1.0f);
    set_tag_kv(w, e, "motion_cue_opacity", f2(cue));
}

bool fetch_overlay(nlohmann::json& out) {
    std::string blob; blob.reserve(512);
    ptv::hud_helpers::hud_get_settings_json(blob);
    if (blob.empty()) return false;
    auto j = nlohmann::json::parse(blob, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) return false;
    // Expect either nested under "accessibility" or flat keys
    if (j.contains("accessibility") && j["accessibility"].is_object()) {
        out = j["accessibility"];
    } else {
        out = j; // tolerate flat layout in tests
    }
    return out.is_object();
}

void persist(const nlohmann::json& st) {
    // Persist under a dedicated section name. Shallow merge enables incremental writes.
    nlohmann::json root = nlohmann::json::object();
    root["accessibility"] = st;
    ptv::persistence_sections::merge_shallow("user_settings", root.dump());
}

} } // namespace ptv::accessibility_internal
