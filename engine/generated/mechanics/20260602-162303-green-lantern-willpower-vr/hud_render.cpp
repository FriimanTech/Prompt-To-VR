#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"
#include <nlohmann/json.hpp>
#include <entt/entt.hpp>
#include <string>
#include <vector>
#include <algorithm>
#include "hud_wrist_shared.h"

namespace ptv {

static std::string mode_name_from_index(int i) {
    switch (i % 3) {
        case 0: return "full";
        case 1: return "minimal";
        default: return "targets";
    }
}

void HudAndWristMenu::publish_hud_mode(World& world, std::uint32_t entity) {
    push_tag(world, entity, std::string(TAG_UI_HUD_MODE_PREFIX) + nlohmann::json({{"mode", mode_name_from_index(hud_mode_index_)}}).dump());
}

void HudAndWristMenu::publish_hud_style(World& world, std::uint32_t entity) {
    push_tag(world, entity, std::string(TAG_UI_HUD_STYLE_PREFIX) + nlohmann::json({{"high_contrast", high_contrast_}, {"opacity", ui_opacity_}}).dump());
}

void HudAndWristMenu::publish_menu_open(World& world, std::uint32_t entity) {
    // Ensure pages are up-to-date before opening
    scan_and_register_pages(world);
    // Build pages array
    nlohmann::json arr = nlohmann::json::array();
    for (auto& p : pages_) arr.push_back({{"id", p.id}, {"title", p.title}});
    nlohmann::json j = {{"pages", arr}, {"opacity", ui_opacity_}};
    push_tag(world, entity, std::string(TAG_UI_MENU_OPEN_PREFIX) + j.dump());
}

void HudAndWristMenu::publish_menu_close(World& world, std::uint32_t entity) {
    push_tag(world, entity, std::string(TAG_UI_MENU_CLOSE_PREFIX) + nlohmann::json::object().dump());
}

void HudAndWristMenu::persist_settings_now(World& world, std::uint32_t entity) {
    // Compose a single settings blob under our section key
    nlohmann::json data = {
        {"hud_on", hud_on_},
        {"hud_mode", mode_name_from_index(hud_mode_index_)},
        {"high_contrast", high_contrast_},
        {"ui_opacity", ui_opacity_}
    };
    nlohmann::json push = {{"section", "settings.hud"}, {"data", data}};
    push_tag(world, entity, std::string(TAG_SAVE_SECTION_PUSH_PREFIX) + push.dump());
    // Request an immediate save flush (manager may debounce internally)
    nlohmann::json req = {{"immediate", true}};
    push_tag(world, entity, std::string(TAG_SAVE_REQUEST_PREFIX) + req.dump());
}

void HudAndWristMenu::scan_and_register_pages(World& world) {
    auto& reg = world.registry();
    auto view = reg.view<CTag>();
    for (auto e : view) {
        auto& tags = view.get<CTag>(e).tags;
        for (auto& s : tags) {
            auto jOpt = parse_tag_json(s, TAG_UI_PAGE_REGISTER_PREFIX);
            if (!jOpt) continue;
            const auto& j = *jOpt;
            if (!j.is_object()) continue;
            WristPageDef def;
            def.id = j.value("id", "");
            def.title = j.value("title", def.id);
            def.order = j.value("order", 100);
            if (def.id.empty()) continue;
            auto it = std::find_if(pages_.begin(), pages_.end(), [&](const WristPageDef& p){return p.id == def.id;});
            if (it == pages_.end()) {
                pages_.push_back(def);
            } else {
                // Update title/order if re-registered
                it->title = def.title;
                it->order = def.order;
            }
        }
    }
    std::sort(pages_.begin(), pages_.end(), [](const WristPageDef& a, const WristPageDef& b){
        if (a.order != b.order) return a.order < b.order;
        return a.title < b.title;
    });
}

void HudAndWristMenu::handle_binding_inputs(World& world, std::uint32_t entity, const nlohmann::json& j) {
    // Resource meters (optional inputs)
    hud_data_.health01 = std::clamp(j.value("health01", hud_data_.health01), 0.0f, 1.0f);
    hud_data_.ring_energy = std::max(0.0f, j.value("ring_energy", hud_data_.ring_energy));
    hud_data_.will_focus = std::clamp(j.value("will_focus", hud_data_.will_focus), 0.0f, 1000.0f);

    const bool x_tap = j.value("x_tap", false);
    const bool x_hold = j.value("x_hold", false);
    const bool x_pressed = j.value("x_pressed", false);

    // Open/close menu on press/hold; cycle HUD mode on tap if menu closed
    if ((x_hold || x_pressed) && !menu_open_) {
        menu_open_ = true;
        publish_menu_open(world, entity);
        return;
    }
    if ((x_hold || x_pressed) && menu_open_) {
        // Toggle off on re-press
        menu_open_ = false;
        publish_menu_close(world, entity);
        persist_settings_now(world, entity);
        return;
    }
    if (x_tap && !menu_open_) {
        hud_mode_index_ = (hud_mode_index_ + 1) % 3;
        publish_hud_mode(world, entity);
    }

    // Optional direct style tweaks from input
    if (j.contains("toggle_high_contrast") && j.at("toggle_high_contrast").get<bool>()) {
        high_contrast_ = !high_contrast_;
        publish_hud_style(world, entity);
    }
    if (j.contains("opacity_delta")) {
        float d = j.at("opacity_delta").get<float>();
        ui_opacity_ = std::clamp(ui_opacity_ + d, 0.2f, 1.0f);
        publish_hud_style(world, entity);
    }
}

void HudAndWristMenu::on_update(World& world, std::uint32_t entity, float dt) {
    t_since_attach_ += dt;

    // Pull inputs from our own binding entry (params_json)
    if (world.has<CMechanicBinding>(entity)) {
        const auto& mb = world.get<CMechanicBinding>(entity);
        for (const auto& entry : mb.entries) {
            // Match either exact id or builtin route (id field is authoritative here)
            if (entry.id == id() || entry.builtin_id == id()) {
                auto j = nlohmann::json::parse(entry.params_json, nullptr, false);
                if (j.is_object()) {
                    handle_binding_inputs(world, entity, j);
                }
            }
        }
    }

    // Keep HUD enable tag alive if it was toggled externally
    if (hud_on_) {
        push_tag(world, entity, std::string(TAG_UI_HUD_ENABLE_PREFIX) + nlohmann::json({{"on", true}}).dump());
    }
}

} // namespace ptv
