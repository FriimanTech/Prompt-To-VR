#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <string>

#include "locomotion_settings_menu_shared.h"

namespace ptv {

class LocomotionSettingsMenu : public IMechanic {
public:
    void on_attach(World& w, std::uint32_t e, const nlohmann::json& params) override {
        // Ensure service tag so other systems can find this owner as the settings source.
        ensure_service_tag(w, e);

        // Seed defaults from binding params first (if present), fall back to provided params, then hard defaults.
        CLocomotionSettings s{};
        s.mode = CLocomotionSettings::Mode::SlideHead;
        s.snap_degrees = 45.0f;
        s.snap_cooldown_s = 0.30f;
        s.vignette_enabled = true;
        s.vignette_intensity = 0.5f;
        s.smooth_turn_enabled = false;
        s.smooth_turn_rate_dps = 120.0f;
        s.dirty = true;

        // Read this mechanic's binding JSON (if any) for defaults.
        if (auto jb = find_own_binding_params(w, e, id()); jb.has_value()) {
            const auto& j = *jb;
            const std::string def_mode = j.value("default_mode", params.value("default_mode", std::string("slide_head")));
            if (def_mode == "teleport") s.mode = CLocomotionSettings::Mode::Teleport;
            else if (def_mode == "slide_controller") s.mode = CLocomotionSettings::Mode::SlideController;
            else s.mode = CLocomotionSettings::Mode::SlideHead;

            s.snap_degrees = j.value("default_snap_deg", params.value("default_snap_deg", 45.0f));
            s.snap_cooldown_s = j.value("default_snap_cooldown_s", params.value("default_snap_cooldown_s", 0.30f));
            s.vignette_enabled = j.value("default_vignette", params.value("default_vignette", true));
            // Smooth turn is chosen via HUD; start disabled unless a binding asks otherwise.
            s.smooth_turn_enabled = j.value("default_smooth_turn", params.value("default_smooth_turn", false));
            s.smooth_turn_rate_dps = j.value("default_smooth_turn_rate_dps", params.value("default_smooth_turn_rate_dps", 120.0f));
        } else {
            const std::string def_mode = params.value("default_mode", std::string("slide_head"));
            if (def_mode == "teleport") s.mode = CLocomotionSettings::Mode::Teleport;
            else if (def_mode == "slide_controller") s.mode = CLocomotionSettings::Mode::SlideController;
            else s.mode = CLocomotionSettings::Mode::SlideHead;

            s.snap_degrees = params.value("default_snap_deg", 45.0f);
            s.snap_cooldown_s = params.value("default_snap_cooldown_s", 0.30f);
            s.vignette_enabled = params.value("default_vignette", true);
            s.smooth_turn_enabled = params.value("default_smooth_turn", false);
            s.smooth_turn_rate_dps = params.value("default_smooth_turn_rate_dps", 120.0f);
        }

        // Load overrides from persistence if present.
        if (!w.has<CSavedSettings>(e)) {
            // Create an empty blob to allow immediate write-through persistence.
            w.emplace_or_replace<CSavedSettings>(e, CSavedSettings{});
        }
        load_settings_if_present(w, e, s);

        // Publish the singleton component.
        w.emplace_or_replace<CLocomotionSettings>(e, s);

        // Apply immediately to world so locomotion starts in the chosen mode.
        apply_settings_globally(w, s);
        persist_settings_if_present(w, e, s);
    }

    void on_update(World& w, std::uint32_t e, float /*dt*/) override {
        if (!w.has<CLocomotionSettings>(e)) return;
        auto& s = w.get<CLocomotionSettings>(e);
        if (!s.dirty) return;
        s.dirty = false;
        // Enforce exclusivity and snap/smooth policy.
        apply_settings_globally(w, s);
        persist_settings_if_present(w, e, s);
        // Ensure the service tag remains present (other mechanics may replace CTag).
        ensure_service_tag(w, e);
    }

    void on_detach(World& w, std::uint32_t e) override {
        // Keep saved settings; drop live singleton.
        if (w.has<CLocomotionSettings>(e)) w.remove<CLocomotionSettings>(e);
        // Leave CTag and CSavedSettings in place so re-binding restores last values.
    }

    std::string id() const override { return "generated.locomotion_settings_menu"; }
};

PTV_REGISTER_MECHANIC("generated.locomotion_settings_menu", LocomotionSettingsMenu)

} // namespace ptv
