#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"
#include <nlohmann/json.hpp>
#include <string>

#include "locomotion_settings_menu_shared.h"

namespace ptv {

// Convenience API other systems (e.g., HUD) can call by editing the singleton then setting dirty=true.
// To help tests, we expose small helpers via weakly-linked C-style names (not required by engine).

// Change locomotion mode.
static void set_mode(CLocomotionSettings& s, const std::string& mode) {
    if (mode == "teleport") s.mode = CLocomotionSettings::Mode::Teleport;
    else if (mode == "slide_controller") s.mode = CLocomotionSettings::Mode::SlideController;
    else s.mode = CLocomotionSettings::Mode::SlideHead;
}

// Internal utility to sanitize settings before apply.
static void sanitize(CLocomotionSettings& s) {
    if (s.snap_degrees < 5.0f) s.snap_degrees = 5.0f;
    if (s.snap_degrees > 90.0f) s.snap_degrees = 90.0f;
    if (s.snap_cooldown_s < 0.0f) s.snap_cooldown_s = 0.0f;
    if (s.vignette_intensity < 0.0f) s.vignette_intensity = 0.0f;
    if (s.vignette_intensity > 1.0f) s.vignette_intensity = 1.0f;
    if (s.smooth_turn_rate_dps < 30.0f) s.smooth_turn_rate_dps = 30.0f;
    if (s.smooth_turn_rate_dps > 360.0f) s.smooth_turn_rate_dps = 360.0f;
}

// Public helper callable from tests via symbol lookup if needed.
extern "C" void ptv_locomotion_apply_settings(World* world, std::uint32_t owner_entity,
                                              const char* json_settings_blob) {
    if (!world || !world->valid(owner_entity)) return;
    auto& w = *world;
    if (!w.has<CLocomotionSettings>(owner_entity)) return;
    auto& s = w.get<CLocomotionSettings>(owner_entity);

    auto j = nlohmann::json::parse(json_settings_blob ? json_settings_blob : "{}", nullptr, false);
    if (!j.is_object()) j = nlohmann::json::object();

    if (j.contains("mode")) set_mode(s, j["mode"].get<std::string>());
    if (j.contains("snap_degrees")) s.snap_degrees = j["snap_degrees"].get<float>();
    if (j.contains("snap_cooldown_s")) s.snap_cooldown_s = j["snap_cooldown_s"].get<float>();
    if (j.contains("vignette_enabled")) s.vignette_enabled = j["vignette_enabled"].get<bool>();
    if (j.contains("vignette_intensity")) s.vignette_intensity = j["vignette_intensity"].get<float>();

    if (j.contains("turn")) {
        const auto turn = j["turn"].get<std::string>();
        if (turn == "smooth") s.smooth_turn_enabled = true; else if (turn == "snap") s.smooth_turn_enabled = false;
    }
    if (j.contains("smooth_turn_rate_dps")) s.smooth_turn_rate_dps = j["smooth_turn_rate_dps"].get<float>();

    sanitize(s);
    s.dirty = true; // Let the registered mechanic apply/persist next frame.
}

// Getter for HUD/tests.
extern "C" int ptv_locomotion_get_settings(World* world, std::uint32_t owner_entity, char* out_json, int out_cap) {
    if (!world || !world->valid(owner_entity) || out_cap <= 2) return 0;
    auto& w = *world;
    if (!w.has<CLocomotionSettings>(owner_entity)) return 0;
    const auto& s = w.get<CLocomotionSettings>(owner_entity);

    nlohmann::json j = nlohmann::json::object();
    std::string mode = (s.mode == CLocomotionSettings::Mode::Teleport) ? "teleport" : (s.mode == CLocomotionSettings::Mode::SlideController ? "slide_controller" : "slide_head");
    j["mode"] = mode;
    j["snap_degrees"] = s.snap_degrees;
    j["snap_cooldown_s"] = s.snap_cooldown_s;
    j["vignette_enabled"] = s.vignette_enabled;
    j["vignette_intensity"] = s.vignette_intensity;
    j["smooth_turn_enabled"] = s.smooth_turn_enabled;
    j["smooth_turn_rate_dps"] = s.smooth_turn_rate_dps;

    const std::string dump = j.dump();
    const int n = static_cast<int>(dump.size());
    const int to_copy = (n < out_cap - 1) ? n : (out_cap - 1);
    std::memcpy(out_json, dump.data(), static_cast<size_t>(to_copy));
    out_json[to_copy] = '\0';
    return to_copy;
}

} // namespace ptv
