#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <string>

#include "locomotion_options_ui_shared.h"

namespace ptv {

static inline bool starts_with(const std::string& s, const char* pfx) {
    return s.rfind(pfx, 0) == 0;
}

static constexpr const char* kTagUIPageRegister = "ui.page.register:";
static constexpr const char* kTagUIToast        = "ui.toast:";
static constexpr const char* kTagMenuClose      = "ui.menu.close:";
static constexpr const char* kTagSaveSection    = "save.section.push:";
static constexpr const char* kTagSaveRequest    = "save.request:";

static constexpr const char* kTagComfortVignette = "comfort.vignette:";
static constexpr const char* kTagTurnState       = "locomotion.turn:";
static constexpr const char* kTagLocoEnable      = "locomotion.enable:";
static constexpr const char* kTagLocoDisable     = "locomotion.disable:";
static constexpr const char* kTagActiveProfile   = "locomotion.active_profile:";
static constexpr const char* kTagHeightOffset    = "player.height_offset:";

static std::string settings_section_json(const LocomotionSettingsState& s) {
    nlohmann::json j{
        {"section","locomotion"},
        {"enable_teleport", s.enable_teleport},
        {"enable_slide_head", s.enable_slide_head},
        {"enable_slide_controller", s.enable_slide_controller},
        {"turn_mode", s.turn_mode},
        {"snap_angle_deg", s.snap_angle_deg},
        {"smooth_turn_speed_degps", s.smooth_turn_speed_degps},
        {"vignette_intensity", s.vignette_intensity},
        {"height_offset_m", s.height_offset_m}
    };
    return j.dump();
}

static void publish_changes(World& w, std::uint32_t e, const LocomotionSettingsState& cur, LocomotionSettingsState& last) {
    if (cur.turn_mode != last.turn_mode || cur.snap_angle_deg != last.snap_angle_deg || cur.smooth_turn_speed_degps != last.smooth_turn_speed_degps) {
        nlohmann::json j{{"mode", cur.turn_mode}, {"snap_deg", cur.snap_angle_deg}, {"smooth_degps", cur.smooth_turn_speed_degps}};
        add_tag(w, e, std::string(kTagTurnState) + j.dump());
    }
    if (cur.vignette_intensity != last.vignette_intensity) {
        nlohmann::json j{{"intensity", cur.vignette_intensity}};
        add_tag(w, e, std::string(kTagComfortVignette) + j.dump());
    }

    auto push_enable = [&](const char* id, bool en, bool prev){
        if (en != prev) {
            nlohmann::json j{{"id", id}};
            add_tag(w, e, std::string(en ? kTagLocoEnable : kTagLocoDisable) + j.dump());
        }
    };
    push_enable("builtin.locomotion.teleport", cur.enable_teleport, last.enable_teleport);
    push_enable("builtin.locomotion.slide.head_relative", cur.enable_slide_head, last.enable_slide_head);
    push_enable("builtin.locomotion.slide.controller_relative", cur.enable_slide_controller, last.enable_slide_controller);

    std::string profile = cur.enable_slide_controller ? "slide_controller" : (cur.enable_slide_head ? "slide_head" : "teleport");
    if (profile != (last.enable_slide_controller ? "slide_controller" : (last.enable_slide_head ? "slide_head" : "teleport"))) {
        nlohmann::json j{{"profile", profile}};
        add_tag(w, e, std::string(kTagActiveProfile) + j.dump());
    }

    last = cur;
}

static void maybe_persist_on_menu_close(World& w, std::uint32_t e, const LocomotionSettingsState& s) {
    if (!w.has<CTag>(e)) return;
    const auto& t = w.get<CTag>(e);
    bool saw_close = false;
    for (const auto& tag : t.tags) {
        if (starts_with(tag, kTagMenuClose)) { saw_close = true; break; }
    }
    if (saw_close) {
        add_tag(w, e, std::string(kTagSaveSection) + settings_section_json(s));
        add_tag(w, e, std::string(kTagSaveRequest) + std::string("{\"reason\":\"locomotion_options_changed\"}"));
    }
}

class LocomotionOptionsUI : public IMechanic {
public:
    void on_attach(World& w, std::uint32_t e, const nlohmann::json& params) override {
        entity_ = e;
        // Register Wrist Menu page via HUD/Wrist dependency contract
        add_tag(w, e, std::string("ui.page.register:{\"id\":\"locomotion\",\"title\":\"Locomotion\",\"order\":10}"));

        // Optional: seed from provided params on attach
        read_settings_from_params(params, target_);
        current_ = target_;
        height_current_ = current_.height_offset_m;

        // First-time hint toast (harmless if shown globally once)
        if (!hint_done_) {
            add_tag(w, e, std::string("ui.toast:{\"text\":\"Open Wrist Menu (X) to choose your locomotion\",\"ttl_s\":4.0}"));
            hint_done_ = true;
        }
    }

    void on_update(World& w, std::uint32_t e, float dt) override {
        if (w.has<CMechanicBinding>(e)) {
            const auto& mb = w.get<CMechanicBinding>(e);
            auto pj = params_for_mechanic_from_binding(mb, id());
            LocomotionSettingsState newTarget = target_;
            read_settings_from_params(pj, newTarget);
            target_ = newTarget;
        }

        if (!target_.enable_teleport && !target_.enable_slide_head && !target_.enable_slide_controller) {
            target_.enable_teleport = true;
        }

        float target_h = std::clamp(target_.height_offset_m, -0.3f, 0.3f);
        float lerp_rate = 8.0f;
        float alpha = std::clamp(dt * lerp_rate, 0.0f, 1.0f);
        height_current_ = height_current_ + (target_h - height_current_) * alpha;
        if (std::abs(height_current_ - current_.height_offset_m) > 1e-4f) {
            current_.height_offset_m = height_current_;
            nlohmann::json j{{"meters", current_.height_offset_m}};
            add_tag(w, e, std::string(kTagHeightOffset) + j.dump());
        }

        current_.enable_teleport = target_.enable_teleport;
        current_.enable_slide_head = target_.enable_slide_head;
        current_.enable_slide_controller = target_.enable_slide_controller;
        current_.turn_mode = target_.turn_mode;
        current_.snap_angle_deg = target_.snap_angle_deg;
        current_.smooth_turn_speed_degps = target_.smooth_turn_speed_degps;
        current_.vignette_intensity = target_.vignette_intensity;

        publish_changes(w, e, current_, last_published_);

        maybe_persist_on_menu_close(w, e, current_);
    }

    void on_detach(World&, std::uint32_t) override {}

    std::string id() const override { return "generated.locomotion_options_ui"; }

    // Expose state to sibling files
    const LocomotionSettingsState& current() const { return current_; }

private:
    std::uint32_t entity_{0};
    LocomotionSettingsState current_{};
    LocomotionSettingsState target_{};
    LocomotionSettingsState last_published_{};

    float height_current_{0.0f};
    bool hint_done_{false};

    friend void apply_runtime(World& w, std::uint32_t e, float dt, LocomotionOptionsUI& self);
};

} // namespace ptv

PTV_REGISTER_MECHANIC("generated.locomotion_options_ui", ptv::LocomotionOptionsUI)
