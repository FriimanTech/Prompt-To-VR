#pragma once

#include <string>
#include <optional>
#include <cstdint>
#include <nlohmann/json.hpp>

namespace ptv {

// Local state machine for portal travel
enum class PortalPhase : std::uint8_t {
    IdleAtLaunch = 0,
    ArmedAwaitConfirm,   // mission selected, waiting for A to confirm
    FadeOut,             // fade/transition masking
    LoadingCorridor,     // issuing load request to corridor scene
    InCorridor,          // corridor mini-scene, optional target practice
    Cancelling,          // B pressed in corridor, return to launch
    HandoffToMission     // loading destination mission scene
};

struct PortalTarget {
    std::string mission_id;     // e.g., "alien_city_attack_m01"
    std::string scene_id;       // e.g., "alien_city_attack"
    std::string mission_name;   // display name (for HUD toast)
};

struct PortalConfig {
    // Read from params on attach / per-frame
    bool practice_drones = true;
    int practice_max_alive = 3;
    float practice_spawn_cooldown_s = 2.0f;
    // Button inputs (driven by test via params_json on our binding entry)
    bool a_pressed = false;
    bool b_pressed = false;
};

struct PortalRuntime {
    PortalPhase phase = PortalPhase::IdleAtLaunch;
    std::optional<PortalTarget> target; // set by portal.travel.request: tag

    // simple edges
    bool a_was_held = false;
    bool b_was_held = false;

    // timers
    float t_fade = 0.0f;
    float t_since_spawn = 0.0f;

    // book-keeping to avoid re-consuming same tag payload every frame
    std::string last_request_payload; // raw json of last portal.travel.request consumed

    // cached strings from dependencies (kept here to avoid hardcoding in multiple places)
    std::string tag_ui_page_register = "ui.page.register:"; // from hud_and_wrist_menu
    std::string tag_ui_toast = "ui.toast:";                 // from hud_and_wrist_menu
    std::string tag_portal_request = "portal.travel.request:"; // from mission_system outputs
    std::string tag_portal_activated = "portal.activated:";     // from mission_system inputs
};

// Utility: safe parse of JSON embedded in CTag string after a known prefix.
inline nlohmann::json parse_payload_after_prefix(const std::string& s, const std::string& prefix) {
    if (s.rfind(prefix, 0) != 0) return nlohmann::json{};
    auto payload = s.substr(prefix.size());
    auto j = nlohmann::json::parse(payload, nullptr, false);
    return j.is_discarded() ? nlohmann::json{} : j;
}

} // namespace ptv
