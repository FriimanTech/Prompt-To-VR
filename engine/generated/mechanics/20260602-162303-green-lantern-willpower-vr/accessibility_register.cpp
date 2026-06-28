#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <optional>
#include <algorithm>

// Forward declarations to depend-on exports (no extra headers allowed)
namespace ptv { namespace haptics { float get_user_intensity(); void set_user_intensity(float); } }
namespace ptv { namespace hud_helpers { void hud_get_settings_json(std::string& out); } }
namespace ptv { namespace persistence_sections { void merge_shallow(const std::string& section, const std::string& json_blob); std::string tiny_digest(const std::string& json_blob); } }

// Helpers implemented in accessibility_runtime.cpp
namespace ptv { namespace accessibility_internal {
    // Applies key=value tags to CTag and touches other systems (haptics)
    void apply_to_world(World& w, std::uint32_t e, const nlohmann::json& st);
    // Pull latest overlay from Wrist Menu/HUD; returns true if found any settings blob
    bool fetch_overlay(nlohmann::json& out);
    // Persist to SavePersistence section
    void persist(const nlohmann::json& st);
} }

namespace ptv {

// Mechanic: reads params and wrist-menu overlay; pushes comfort/accessibility tags; persists.
class AccessibilitySettings final : public IMechanic {
public:
    void on_attach(World& world, std::uint32_t entity, const nlohmann::json& params) override {
        (void)world; (void)entity;
        state_ = defaults_from_params(params);
        // Try overlay from HUD (user's previous save restored by HUD or SavePersistence)
        nlohmann::json overlay;
        if (accessibility_internal::fetch_overlay(overlay) && overlay.is_object()) {
            merge_into(state_, overlay);
        }
        // Cache digest to avoid redundant work, then apply immediately and persist
        last_digest_ = digest(state_);
        accessibility_internal::apply_to_world(world, entity, state_);
        accessibility_internal::persist(state_);
    }

    void on_update(World& world, std::uint32_t entity, float /*dt*/) override {
        // Poll HUD/Wrist settings json each frame; apply only when changed
        nlohmann::json overlay;
        if (accessibility_internal::fetch_overlay(overlay) && overlay.is_object()) {
            nlohmann::json merged = state_;
            merge_into(merged, overlay);
            std::string d = digest(merged);
            if (d != last_digest_) {
                state_ = merged;
                last_digest_ = d;
                accessibility_internal::apply_to_world(world, entity, state_);
                accessibility_internal::persist(state_);
            }
        }
    }

    void on_detach(World&, std::uint32_t) override {}

    std::string id() const override { return "generated.accessibility_settings"; }

private:
    static nlohmann::json defaults_from_params(const nlohmann::json& p) {
        nlohmann::json j = nlohmann::json::object();
        j["vignette_intensity"]        = p.value("vignette_intensity", 0.45f);
        j["turn_mode"]                 = p.value("turn_mode", std::string("snap"));               // snap | smooth
        j["smooth_turn_speed_degps"]   = p.value("smooth_turn_speed_degps", 120.0f);
        j["snap_angle_deg"]            = p.value("snap_angle_deg", 45.0f);
        j["flight_assist_strength"]    = p.value("flight_assist_strength", 0.5f);
        j["haptics_user_intensity"]    = p.value("haptics_user_intensity", 0.8f);
        j["subtitle_scale"]            = p.value("subtitle_scale", 1.0f);                         // 0.75..1.5
        j["colorblind_mode"]           = p.value("colorblind_mode", std::string("none"));        // none|protan|deutan|tritan|mono
        j["seated_y_offset_m"]         = p.value("seated_y_offset_m", 0.0f);
        j["motion_cue_opacity"]        = p.value("motion_cue_opacity", 0.6f);
        return j;
    }

    static void merge_into(nlohmann::json& base, const nlohmann::json& overlay) {
        for (auto it = overlay.begin(); it != overlay.end(); ++it) {
            // Only accept known keys
            const std::string& k = it.key();
            if (base.contains(k)) base[k] = it.value();
        }
    }

    static std::string digest(const nlohmann::json& j) {
        return persistence_sections::tiny_digest(j.dump());
    }

private:
    nlohmann::json state_{};
    std::string last_digest_{};
};

} // namespace ptv

PTV_REGISTER_MECHANIC("generated.accessibility_settings", ptv::AccessibilitySettings)
