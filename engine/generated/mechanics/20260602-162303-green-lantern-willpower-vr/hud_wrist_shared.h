#pragma once

#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <optional>
#include <algorithm>

namespace ptv {

// Dependency (save_persistence) tag protocol — copied from ledger exports for loose coupling
// Inputs we may publish:
//   "save.request:", "load.request:", "mission.complete:", "recharge.complete:",
//   "save.register:", "save.section.push:"
// Outputs we may listen to (not used here):
//   "save.collect.start:", "save.completed:", "load.section:", "load.completed:"
static constexpr const char* TAG_SAVE_REQUEST_PREFIX = "save.request:";
static constexpr const char* TAG_SAVE_SECTION_PUSH_PREFIX = "save.section.push:";

// HUD & Wrist Menu interop prefixes defined by this mechanic
static constexpr const char* TAG_UI_TOAST_PREFIX = "ui.toast:";                  // {"text":"...","ttl_s":2.0}
static constexpr const char* TAG_UI_MENU_OPEN_PREFIX = "ui.menu.open:";          // {"pages":[{id,title}],"opacity":0..1}
static constexpr const char* TAG_UI_MENU_CLOSE_PREFIX = "ui.menu.close:";        // {}
static constexpr const char* TAG_UI_PAGE_REGISTER_PREFIX = "ui.page.register:";  // {"id":"locomotion","title":"Locomotion","order":10}
static constexpr const char* TAG_UI_HUD_ENABLE_PREFIX = "hud.enable:";           // {"on":true}
static constexpr const char* TAG_UI_HUD_MODE_PREFIX = "hud.mode:";               // {"mode":"full|minimal|targets"}
static constexpr const char* TAG_UI_HUD_STYLE_PREFIX = "hud.style:";             // {"high_contrast":bool,"opacity":0..1}

// Inputs expected on our own CMechanicBinding entry (driven by test harness)
//  { x_tap:bool, x_hold:bool, x_pressed:bool, health01:float, ring_energy:float, will_focus:float }

struct WristPageDef {
    std::string id;
    std::string title;
    int order{100};
};

struct HudStateData {
    float health01{1.0f};
    float ring_energy{100.0f};
    float will_focus{0.0f};
};

// Helper to push a tag string onto an entity's CTag vector (create if missing)
inline void push_tag(World& w, std::uint32_t e, const std::string& tag) {
    if (!w.valid(e)) return;
    if (w.has<CTag>(e)) {
        auto& v = w.get<CTag>(e).tags;
        v.emplace_back(tag);
    } else {
        CTag t; t.tags.emplace_back(tag);
        w.emplace_or_replace<CTag>(e, std::move(t));
    }
}

inline std::optional<nlohmann::json> parse_tag_json(const std::string& s, const char* prefix) {
    if (s.rfind(prefix, 0) != 0) return std::nullopt;
    auto j = nlohmann::json::parse(s.c_str() + std::char_traits<char>::length(prefix), nullptr, false);
    if (!j.is_discarded()) return j;
    return std::nullopt;
}

// Forward declaration of the mechanic class
class HudAndWristMenu : public IMechanic {
public:
    void on_attach(World& world, std::uint32_t entity, const nlohmann::json& params) override;
    void on_update(World& world, std::uint32_t entity, float dt) override;
    void on_detach(World& world, std::uint32_t entity) override;
    std::string id() const override { return "generated.hud_and_wrist_menu"; }

private:
    // Internal helpers
    void handle_binding_inputs(World& world, std::uint32_t entity, const nlohmann::json& j);
    void scan_and_register_pages(World& world);
    void publish_menu_open(World& world, std::uint32_t entity);
    void publish_menu_close(World& world, std::uint32_t entity);
    void persist_settings_now(World& world, std::uint32_t entity);
    void publish_hud_mode(World& world, std::uint32_t entity);
    void publish_hud_style(World& world, std::uint32_t entity);

private:
    // Session state
    bool hud_on_{true};
    int hud_mode_index_{0}; // 0=full,1=minimal,2=targets
    bool menu_open_{false};
    float ui_opacity_{0.9f};
    bool high_contrast_{false};
    float t_since_attach_{0.0f};
    bool toast_prompt_sent_{false};

    // Registry of external pages
    std::vector<WristPageDef> pages_{}; // kept sorted by order then title

    // Cached HUD data (purely for annunciators if a visual layer listens)
    HudStateData hud_data_{};
};

} // namespace ptv
