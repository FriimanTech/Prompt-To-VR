#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <utility>

#include "accessibility_shared.h"

namespace ptv {

static inline void push_tag(World& w, std::uint32_t e, const std::string& s) {
    if (!w.has<CTag>(e)) { w.emplace_or_replace<CTag>(e, CTag{}); }
    auto& tag = w.get<CTag>(e);
    tag.tags.emplace_back(s);
}

static nlohmann::json get_live_params(World& w, std::uint32_t e, const std::string& mech_id) {
    if (!w.has<CMechanicBinding>(e)) return nlohmann::json{};
    auto& mb = w.get<CMechanicBinding>(e);
    for (const auto& entry : mb.entries) {
        if (entry.id == mech_id || entry.builtin_id == mech_id) {
            auto j = nlohmann::json::parse(entry.params_json, nullptr, false);
            if (j.is_discarded()) return nlohmann::json{};
            return j;
        }
    }
    return nlohmann::json{};
}

class AccessibilitySettings : public IMechanic {
public:
    void on_attach(World& w, std::uint32_t e, const nlohmann::json& params) override {
        state_ = AccessibilityState::from_defaults_json(params);
        entity_ = e;
        // Register Wrist Menu page (via HUD contract)
        nlohmann::json jpage; jpage["id"] = "accessibility"; jpage["title"] = "Accessibility & Comfort"; jpage["order"] = 20;
        push_tag(w, e, std::string("ui.page.register:") + jpage.dump());
        // Apply defaults immediately
        publish_all(w, e, /*with_toast=*/true);
        // Optionally persist immediately if params request it
        if (params.value("persist_on_attach", false)) {
            persist_now(w, e, "init");
        }
    }

    void on_update(World& w, std::uint32_t e, float) override {
        auto j = get_live_params(w, e, id());

        bool request_save = false;
        bool menu_closed_persist = false;

        if (j.is_object()) {
            state_.any_changed = false;
            apply_updates_from_params(state_, j);

            request_save = j.value("request_save", false);
            menu_closed_persist = j.value("menu_closed", false);

            if (state_.any_changed) {
                publish_all(w, e, /*with_toast=*/false);
            }
        }

        if (request_save || menu_closed_persist) {
            persist_now(w, e, request_save ? std::string("explicit_request") : std::string("menu_close"));
            nlohmann::json jt; jt["text"] = std::string("Accessibility settings saved"); jt["ttl"] = 1.0f;
            push_tag(w, e, std::string("ui.toast:") + jt.dump());
        }
    }

    void on_detach(World&, std::uint32_t) override {}

    std::string id() const override { return "generated.accessibility_settings"; }

private:
    void publish_all(World& w, std::uint32_t e, bool with_toast) {
        // Comfort vignette (shared prefix from locomotion_options_ui)
        {
            nlohmann::json j; j["intensity01"] = state_.vignette01; j["ttl"] = 0.2f;
            push_tag(w, e, std::string("comfort.vignette:") + j.dump());
        }
        // Turn settings (shared with locomotion systems)
        {
            nlohmann::json j; j["mode"] = state_.turn_mode; j["speed_dps"] = state_.smooth_turn_speed_dps; j["snap_degrees"] = state_.snap_turn_degrees;
            push_tag(w, e, std::string("locomotion.turn:") + j.dump());
        }
        // Flight assist broadcast (for flight/hover comfort modules)
        {
            nlohmann::json j; j["assist01"] = state_.flight_assist01;
            push_tag(w, e, std::string("flight.assist:") + j.dump());
        }
        // Haptics overall amplitude setting (consumed by haptics manager or mechanics honoring it)
        {
            nlohmann::json j; j["amp01"] = state_.haptics_amp01;
            push_tag(w, e, std::string("haptics.settings:") + j.dump());
        }
        // Subtitles size + colorblind mode (HUD may consume)
        {
            nlohmann::json j; j["size"] = state_.subtitles_size; push_tag(w, e, std::string("ui.subtitles:") + j.dump());
        }
        {
            nlohmann::json j; j["mode"] = state_.colorblind_mode; push_tag(w, e, std::string("ui.colorblind:") + j.dump());
        }
        // Seated height calibration
        {
            nlohmann::json j; j["meters"] = state_.seated_height_offset_m;
            push_tag(w, e, std::string("player.height_offset:") + j.dump());
        }
        // Motion cues opacity (for overlays, speed lines, etc.)
        {
            nlohmann::json j; j["opacity01"] = state_.motion_cue_opacity01;
            push_tag(w, e, std::string("ui.motion_cue:") + j.dump());
        }
        if (with_toast) {
            nlohmann::json j; j["text"] = std::string("Applied Accessibility & Comfort"); j["ttl"] = 1.0f;
            push_tag(w, e, std::string("ui.toast:") + j.dump());
        }
    }

    void persist_now(World& w, std::uint32_t e, const std::string& reason) {
        nlohmann::json sec; sec["section"] = "accessibility"; sec["data"] = state_.to_json();
        push_tag(w, e, std::string("save.section.push:") + sec.dump());
        nlohmann::json req; req["reason"] = reason; req["who"] = "generated.accessibility_settings";
        push_tag(w, e, std::string("save.request:") + req.dump());
    }

private:
    AccessibilityState state_{};
    std::uint32_t entity_{0};
};

} // namespace ptv

PTV_REGISTER_MECHANIC("generated.accessibility_settings", ptv::AccessibilitySettings)
