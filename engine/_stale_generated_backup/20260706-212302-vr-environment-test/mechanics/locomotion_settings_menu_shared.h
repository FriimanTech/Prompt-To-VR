#pragma once

#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <string>
#include <optional>
#include <vector>

namespace ptv {

// Local component holding current locomotion prefs for global consumption.
struct CLocomotionSettings {
    enum class Mode : std::uint8_t { Teleport = 0, SlideHead = 1, SlideController = 2 };

    Mode   mode{Mode::SlideHead};
    float  snap_degrees{45.0f};
    float  snap_cooldown_s{0.30f};

    bool   vignette_enabled{true};
    float  vignette_intensity{0.5f}; // 0..1; HUD may adjust; engine does not render here.

    bool   smooth_turn_enabled{false};
    float  smooth_turn_rate_dps{120.0f}; // When smooth turn is chosen.

    bool   dirty{true}; // Set true when something changed and needs applying/persisting.
};

// Lightweight saved-settings blob (shim). Real system should replace this.
struct CSavedSettings {
    // Keys: locomotion.mode, locomotion.snap_deg, locomotion.snap_cd,
    //       locomotion.smooth_turn_rate, locomotion.vignette
    nlohmann::json kv{nlohmann::json::object()};
};

// Helpers
inline bool tag_list_has(const std::vector<std::string>& tags, const std::string& t) {
    for (const auto& s : tags) if (s == t) return true; return false;
}

inline void ensure_service_tag(World& w, std::uint32_t e) {
    if (w.has<CTag>(e)) {
        auto& tag = w.get<CTag>(e);
        if (!tag_list_has(tag.tags, "locomotion_settings")) tag.tags.push_back("locomotion_settings");
    } else {
        CTag t; t.tags.push_back("locomotion_settings");
        w.emplace_or_replace<CTag>(e, std::move(t));
    }
}

inline std::optional<nlohmann::json> find_own_binding_params(World& w, std::uint32_t e, const std::string& my_id) {
    if (!w.has<CMechanicBinding>(e)) return std::nullopt;
    auto& mb = w.get<CMechanicBinding>(e);
    for (const auto& ent : mb.entries) {
        if (ent.id == my_id || ent.builtin_id == my_id) {
            auto j = nlohmann::json::parse(ent.params_json, nullptr, false);
            if (j.is_discarded()) return nlohmann::json::object();
            if (!j.is_object()) return nlohmann::json::object();
            return j;
        }
    }
    return std::nullopt;
}

// Apply enable flag onto a binding entry's params_json (non-destructive merge).
inline void set_binding_enabled_flag(nlohmann::json& params, bool enabled) {
    // Write/overwrite a conventional runtime key.
    params["enabled"] = enabled;
}

inline void write_turn_params(nlohmann::json& params, float degrees, float cooldown_s) {
    params["degrees"] = degrees;
    params["cooldown_s"] = cooldown_s;
}

// Find and mutate well-known builtin locomotion binding params in-place.
inline void apply_enable_matrix_to_bindings(World& w, bool en_tele, bool en_head, bool en_ctrl, bool en_snap,
                                            float snap_deg, float snap_cd) {
    auto& reg = w.registry();
    auto view = reg.view<CMechanicBinding>();
    for (auto ent : view) {
        auto& mb = view.get<CMechanicBinding>(ent);
        for (auto& entry : mb.entries) {
            const std::string& id = !entry.id.empty() ? entry.id : entry.builtin_id;
            if (id.empty()) continue;
            if (id == "builtin.locomotion.teleport" || id == "locomotion_teleport") {
                auto j = nlohmann::json::parse(entry.params_json, nullptr, false);
                if (!j.is_object()) j = nlohmann::json::object();
                set_binding_enabled_flag(j, en_tele);
                entry.params_json = j.dump();
            } else if (id == "builtin.locomotion.slide.head_relative" || id == "locomotion_slide_head") {
                auto j = nlohmann::json::parse(entry.params_json, nullptr, false);
                if (!j.is_object()) j = nlohmann::json::object();
                set_binding_enabled_flag(j, en_head);
                entry.params_json = j.dump();
            } else if (id == "builtin.locomotion.slide.controller_relative" || id == "locomotion_slide_controller") {
                auto j = nlohmann::json::parse(entry.params_json, nullptr, false);
                if (!j.is_object()) j = nlohmann::json::object();
                set_binding_enabled_flag(j, en_ctrl);
                entry.params_json = j.dump();
            } else if (id == "builtin.locomotion.snap_turn" || id == "locomotion_snap_turn") {
                auto j = nlohmann::json::parse(entry.params_json, nullptr, false);
                if (!j.is_object()) j = nlohmann::json::object();
                set_binding_enabled_flag(j, en_snap);
                write_turn_params(j, snap_deg, snap_cd);
                entry.params_json = j.dump();
            }
        }
    }
}

// Persistence shim.
inline void persist_settings_if_present(World& w, std::uint32_t e, const CLocomotionSettings& s) {
    if (!w.has<CSavedSettings>(e)) return; // If save system binds later, this will start working automatically.
    auto& save = w.get<CSavedSettings>(e);
    save.kv["locomotion.mode"] = static_cast<int>(s.mode);
    save.kv["locomotion.snap_deg"] = s.snap_degrees;
    save.kv["locomotion.snap_cd"] = s.snap_cooldown_s;
    save.kv["locomotion.smooth_turn_rate"] = s.smooth_turn_rate_dps;
    save.kv["locomotion.vignette"] = s.vignette_enabled;
}

inline void load_settings_if_present(World& w, std::uint32_t e, CLocomotionSettings& s) {
    if (!w.has<CSavedSettings>(e)) return;
    auto& save = w.get<CSavedSettings>(e);
    if (save.kv.contains("locomotion.mode")) s.mode = static_cast<CLocomotionSettings::Mode>(save.kv["locomotion.mode"].get<int>());
    if (save.kv.contains("locomotion.snap_deg")) s.snap_degrees = save.kv["locomotion.snap_deg"].get<float>();
    if (save.kv.contains("locomotion.snap_cd")) s.snap_cooldown_s = save.kv["locomotion.snap_cd"].get<float>();
    if (save.kv.contains("locomotion.smooth_turn_rate")) s.smooth_turn_rate_dps = save.kv["locomotion.smooth_turn_rate"].get<float>();
    if (save.kv.contains("locomotion.vignette")) s.vignette_enabled = save.kv["locomotion.vignette"].get<bool>();
}

// Compute and push enable matrix based on settings.
inline void apply_settings_globally(World& w, const CLocomotionSettings& s) {
    const bool en_tele = (s.mode == CLocomotionSettings::Mode::Teleport);
    const bool en_head = (s.mode == CLocomotionSettings::Mode::SlideHead);
    const bool en_ctrl = (s.mode == CLocomotionSettings::Mode::SlideController);
    const bool en_snap = !s.smooth_turn_enabled; // Snap on unless smooth selected.
    apply_enable_matrix_to_bindings(w, en_tele, en_head, en_ctrl, en_snap, s.snap_degrees, s.snap_cooldown_s);
}

} // namespace ptv
