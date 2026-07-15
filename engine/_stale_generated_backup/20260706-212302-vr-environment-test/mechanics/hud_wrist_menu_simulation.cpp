#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"
#include <nlohmann/json.hpp>
#include <cmath>
#include <string>
#include <algorithm>

#include "hud_wrist_menu_shared.h"

namespace ptv {

static float clamp01(float v){ return std::max(0.0f, std::min(1.0f, v)); }

std::uint32_t hw_find_player_rig(World& w, HWSavedPtrs& cache) {
    if (cache.player_rig_cached && w.valid(cache.player_rig)) return cache.player_rig;
    // Try by id first
    auto ent = w.find("player_rig");
    if (ent != entt::null) {
        cache.player_rig = static_cast<std::uint32_t>(ent);
        cache.player_rig_cached = true;
        return cache.player_rig;
    }
    // Fallback: first entity that has tag 'player' or 'player_rig' or 'player_avatar'
    auto& reg = w.registry();
    std::uint32_t found = 0;
    reg.view<CTag>().each([&](entt::entity e, CTag& tag){
        if (found!=0) return; // already found
        auto has = [&](const char* s){ return std::find(tag.tags.begin(), tag.tags.end(), std::string(s))!=tag.tags.end(); };
        if (has("player_rig") || has("player") || has("player_avatar")) {
            found = static_cast<std::uint32_t>(e);
        }
    });
    if (found!=0) {
        cache.player_rig = found;
        cache.player_rig_cached = true;
        return found;
    }
    return 0;
}

static std::optional<nlohmann::json> hw_try_parse_binding_params(World& w, std::uint32_t e) {
    if (!w.has<CMechanicBinding>(e)) return std::nullopt;
    auto& mb = w.get<CMechanicBinding>(e);
    for (auto& entry : mb.entries) {
        if (entry.id == "generated.hud_wrist_menu") {
            auto j = nlohmann::json::parse(entry.params_json, nullptr, false);
            if (j.is_discarded()) return std::nullopt;
            return j;
        }
    }
    return std::nullopt;
}

void hw_parse_params(const nlohmann::json& p, HWState& s) {
    if (p.contains("open_menu_button") && p["open_menu_button"].is_string())
        s.in.open_menu_button = p["open_menu_button"].get<std::string>();
    if (p.contains("close_menu_buttons") && p["close_menu_buttons"].is_array()) {
        auto a = p["close_menu_buttons"];
        if (a.size()>0 && a[0].is_string()) s.in.close_menu_buttons[0] = a[0].get<std::string>();
        if (a.size()>1 && a[1].is_string()) s.in.close_menu_buttons[1] = a[1].get<std::string>();
    }
}

static void hw_refresh_inputs_from_bind(World& w, std::uint32_t e, HWState& s) {
    auto jp = hw_try_parse_binding_params(w, e);
    if (!jp) return;
    const auto& j = *jp;
    s.in.open_menu_pulse  = j.value("open_menu", false);
    s.in.close_menu_pulse = j.value("close_menu", false);
    s.in.dominant_ray_active = j.value("dominant_ray_active", false);
    s.in.trigger_clicked = j.value("trigger_clicked", false);
    s.in.ray_u = static_cast<float>(j.value("ray_u", 0.0));
    s.in.aiming_ranged = j.value("aiming_ranged", false);
    s.in.combat_intensity01 = static_cast<float>(j.value("combat_intensity01", 0.0));

    // HUD feeds (fallback via params since CRingEnergy/CMasteryProfile are not ECS-exposed here)
    s.hud.ring_energy_01 = clamp01(static_cast<float>(j.value("ring_energy_01", s.hud.ring_energy_01)));
    s.hud.mastery_pips   = std::max(0, std::min(6, j.value("mastery_pips", s.hud.mastery_pips)));
    // Damage flash request in seconds
    float add_flash = static_cast<float>(j.value("took_damage_flash_s", 0.0));
    s.hud.damage_flash_s = std::max(s.hud.damage_flash_s, add_flash);
}

void hw_tick_hud(HWState& s, float dt) {
    s.t_accum += dt;
    if (s.hud.damage_flash_s > 0.0f) {
        s.hud.damage_flash_s = std::max(0.0f, s.hud.damage_flash_s - dt);
    }
}

static void hw_handle_selection(HWState& s) {
    if (!s.in.dominant_ray_active || !s.in.trigger_clicked) return;
    // Map ray_u in [0,1] to 4 panels along an arc, left->right
    int idx = static_cast<int>(std::floor(clamp01(s.in.ray_u) * 4.0f));
    if (idx>=4) idx = 3;
    s.current_panel = static_cast<HWPanel>(idx);
}

void hw_tick_menu(HWState& s, float dt) {
    // Auto-hide on high combat intensity
    const float auto_hide_thresh = 0.7f;
    const float suppress_for_s = 2.0f;
    if (s.in.combat_intensity01 > auto_hide_thresh) s.suppress_timer_s = suppress_for_s;
    if (s.suppress_timer_s > 0.0f) s.suppress_timer_s = std::max(0.0f, s.suppress_timer_s - dt);

    // Input toggles
    if (s.in.open_menu_pulse && s.suppress_timer_s <= 0.0f) {
        s.menu.open = !s.menu.open;
        s.menu.fading_in = s.menu.open;
    }
    if (s.in.close_menu_pulse) {
        s.menu.open = false;
        s.menu.fading_in = false;
    }

    // Selection when open
    if (s.menu.open) hw_handle_selection(s);

    // Fade alpha
    float target = (s.menu.open && s.suppress_timer_s <= 0.0f) ? 1.0f : 0.0f;
    float tau = (target> s.menu.alpha) ? std::max(0.001f, s.menu.fade_in_s) : std::max(0.001f, s.menu.fade_out_s);
    float k = 1.0f - std::exp(-dt / tau);
    s.menu.alpha = s.menu.alpha + (target - s.menu.alpha) * k;

    s.menu.visible = s.menu.alpha > 0.02f;
}

void hw_apply_visibility(World& w, std::uint32_t e, const HWState& s) {
    auto& tr = w.get<CTransform>(e).t;
    // Use uniform scale as proxy for visibility and slight size change with alpha for readability
    const float base_scale = 1.0f;
    tr.scale = base_scale * (0.2f + 0.8f * s.menu.alpha);
}

static ptv::Vec3 rotate_by_yaw(const ptv::Vec3& v, float yaw_rad) {
    float c = std::cos(yaw_rad), s = std::sin(yaw_rad);
    return { c*v.x - s*v.z, v.y, s*v.x + c*v.z };
}

void hw_place_menu(World& w, std::uint32_t e, HWState& s, float dt) {
    (void)dt;
    auto rig = hw_find_player_rig(w, s.cache);
    if (rig==0 || !w.has<CTransform>(rig)) return;

    auto& rigTr = w.get<CTransform>(rig).t;
    auto& tr = w.get<CTransform>(e).t;

    // Extract yaw from rig rotation (face direction). Quat is (w,x,y,z)
    const auto& q = rigTr.rotation;
    // yaw from quaternion (assuming small roll/pitch typical for rig)
    float yaw = std::atan2(2.0f*(q.w*q.y + q.x*q.z), 1.0f - 2.0f*(q.y*q.y + q.x*q.x));

    // Place slightly above/forward of left wrist area relative to rig
    ptv::Vec3 local = s.place.left_wrist_offset + s.place.hud_forward_offset;
    ptv::Vec3 world_off = rotate_by_yaw(local, yaw);

    tr.position = rigTr.position + world_off;
    // Face the rig: build a yaw-only rotation back toward rig forward
    // Simple look-at by yaw: rotate so +Z faces same as rig
    tr.rotation = rigTr.rotation; // yaw-align for readability
}

} // namespace ptv
