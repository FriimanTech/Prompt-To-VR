#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"
#include <nlohmann/json.hpp>
#include <string>
#include "hud_wrist_shared.h"

namespace ptv {

void HudAndWristMenu::on_attach(World& world, std::uint32_t entity, const nlohmann::json& params) {
    // Read optional defaults
    ui_opacity_ = std::clamp(params.value("ui_opacity", ui_opacity_), 0.2f, 1.0f);
    high_contrast_ = params.value("high_contrast", high_contrast_);

    // Enable HUD immediately
    push_tag(world, entity, std::string(TAG_UI_HUD_ENABLE_PREFIX) + nlohmann::json({{"on", true}}).dump());
    publish_hud_style(world, entity);
    publish_hud_mode(world, entity);

    // Intro toast to guide player ("on spawn" behavior). Shown once per attach.
    const float ttl = 4.0f;
    const char* tip = "Open Wrist Menu (X) to choose locomotion and comfort.";
    push_tag(world, entity, std::string(TAG_UI_TOAST_PREFIX) + nlohmann::json({{"text", tip}, {"ttl_s", ttl}}).dump());
    toast_prompt_sent_ = true;

    // Pre-scan for externally registered pages already present in scene
    scan_and_register_pages(world);
}

void HudAndWristMenu::on_detach(World& world, std::uint32_t entity) {
    // Close any open menu and persist settings defensively
    if (menu_open_) {
        publish_menu_close(world, entity);
        persist_settings_now(world, entity);
    }
}

} // namespace ptv

PTV_REGISTER_MECHANIC("generated.hud_and_wrist_menu", ptv::HudAndWristMenu)
