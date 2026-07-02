#include "flight_system_shared.h"

#include <algorithm>
#include <cmath>

namespace ptv {

void fs_publish_tag(World& w, std::uint32_t e, const std::string& tag) {
    if (w.has<CTag>(e)) {
        auto& t = w.get<CTag>(e).tags;
        t.emplace_back(tag);
    } else {
        CTag t; t.tags.emplace_back(tag);
        w.emplace_or_replace<CTag>(e, t);
    }
}

void fs_publish_locomotion_velocity(World& w, std::uint32_t e, const glm::vec3& v) {
    nlohmann::json j = {
        {"vx", v.x}, {"vy", v.y}, {"vz", v.z}
    };
    fs_publish_tag(w, e, std::string("locomotion.velocity:") + j.dump());
}

void fs_publish_hover_enable(World& w, std::uint32_t e, bool on) {
    nlohmann::json j = {{"state", on ? "hover" : "off"}};
    fs_publish_tag(w, e, std::string("locomotion.mode:") + j.dump());
}

void fs_publish_aim_stabilizer(World& w, std::uint32_t e, bool enabled, float strength01) {
    nlohmann::json j = {{"enabled", enabled}, {"strength", std::clamp(strength01, 0.0f, 1.0f)}};
    fs_publish_tag(w, e, std::string("aim.stabilizer:") + j.dump());
}

void fs_publish_vignette(World& w, std::uint32_t e, float strength01) {
    // Ground sprint overlay revealed a comfort.vignette:* tag namespace. Use the same prefix.
    nlohmann::json j = {{"strength", std::clamp(strength01, 0.0f, 1.0f)}};
    fs_publish_tag(w, e, std::string("comfort.vignette:") + j.dump());
}

void fs_publish_haptics(World& w, std::uint32_t e, const std::string& pattern_id, const std::string& hand, float amp, float dur_s) {
    // From dependency: publish via prefix haptics.publish:
    nlohmann::json j = {{"pattern_id", pattern_id}, {"hand", hand}, {"intensity_scale", amp}, {"duration_s", dur_s}};
    fs_publish_tag(w, e, std::string("haptics.publish:") + j.dump());
}

void fs_step_comfort(const FlightParams& cfg, const FlightInputs& in, float dt, World& w, std::uint32_t e, FlightStateInternal& st) {
    (void)dt; (void)in;

    // Stabilized horizon (no camera roll). We can't control camera roll directly, but enable an aim stabilizer.
    // Scale with speed for mild stabilization while in flight; off in hover.
    if (st.mode == FlightMode::Flight) {
        float spd = glm::length(st.velocity);
        float k = std::clamp(spd / (cfg.cruise_max_speed_mps * cfg.boost_multiplier), 0.0f, 1.0f);
        fs_publish_aim_stabilizer(w, e, true, 0.2f + 0.5f * k);
        if (cfg.comfort_vignette_on_boost && st.boost_held) {
            fs_publish_vignette(w, e, 0.35f * k);
        } else {
            // Small baseline vignette at higher speeds for comfort
            fs_publish_vignette(w, e, 0.18f * k);
        }
    } else {
        fs_publish_aim_stabilizer(w, e, false, 0.0f);
        fs_publish_vignette(w, e, 0.0f);
    }
}

} // namespace ptv
