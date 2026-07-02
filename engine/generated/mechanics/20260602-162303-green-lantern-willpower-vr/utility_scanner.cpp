#include "utility_constructs_shared.h"

#include <algorithm>

namespace ptv { namespace utility {

static entt::entity ensure_scanner(World& w, std::uint32_t ring_e, UtilityConstructs& uc) {
    if (uc.scanner_.active && uc.scanner_.e != entt::null && w.registry().valid(uc.scanner_.e)) return uc.scanner_.e;
    // Spawn near ring hand
    Transform t{};
    if (w.has<CTransform>(ring_e)) t = w.get<CTransform>(ring_e).t;
    t.position += Vec3{0.0f, 0.15f, 0.25f};
    uc.scanner_.e = make_child(w, "construct.utility:scanner", t);
    uc.scanner_.active = true;
    // Haptics publish (form)
    add_tag(w, static_cast<entt::entity>(ring_e), "haptics.publish:{\"pattern_id\":\"construct_form_heavy\",\"hand\":\"right\",\"intensity_scale\":0.9,\"duration_s\":0.08}");
    return uc.scanner_.e;
}

static void mark_targets(World& w, entt::entity scanner, float range_m) {
    Transform* st = get_tr(w, scanner);
    if (!st) return;
    auto& reg = w.registry();
    auto view = reg.view<CTag, CTransform>();
    for (auto e : view) {
        const std::uint32_t ue = static_cast<std::uint32_t>(e);
        if (ue == static_cast<std::uint32_t>(scanner)) continue;
        const auto& tags = view.get<CTag>(e).tags;
        bool is_enemy=false, is_objective=false;
        for (const auto& s : tags) {
            if (s == "enemy") is_enemy = true;
            if (s == "objective" || s == "objective_console") is_objective = true;
        }
        if (!(is_enemy || is_objective)) continue;
        const auto& tr = view.get<CTransform>(e).t;
        const Vec3 d = tr.position - st->position;
        const float d2 = d.x*d.x + d.y*d.y + d.z*d.z;
        if (d2 <= range_m*range_m) {
            add_tag(w, e, "hud.marker:add:{\"pack\":\"ring_hud_ui_pack\",\"style\":\"scan_highlight\",\"duration_s\":2.0}");
        }
    }
}

void update_scanner(World& w, std::uint32_t ring_e, UtilityConstructs& uc, const nlohmann::json& p, float dt) {
    // Spawn request
    const bool spawn = p.value("spawn_scanner", false) || (p.value("selected", std::string("")) == std::string("construct_scanner_drone") && p.value("rt_confirm", false));
    if (spawn) ensure_scanner(w, ring_e, uc);

    if (!uc.scanner_.active) return;

    // Toggle scanning with Left Trigger
    if (p.value("left_trigger_toggle_scan", false)) uc.scanner_.scanning = !uc.scanner_.scanning;

    // Gentle guidance toward ring when Left Grip held
    if (p.value("left_grip_guide", false)) {
        if (w.has<CTransform>(ring_e)) {
            Transform* st = get_tr(w, uc.scanner_.e);
            if (st) {
                const auto& hand = w.get<CTransform>(ring_e).t;
                Vec3 to = hand.position + Vec3{0.0f,0.15f,0.35f};
                st->position = st->position + (to - st->position) * std::clamp(dt*2.0f, 0.0f, 1.0f);
            }
        }
    }

    // Scan pulse
    if (uc.scanner_.scanning) {
        uc.scanner_.pulse_timer += dt;
        if (uc.scanner_.pulse_timer >= std::max(0.1f, uc.scanner_.interval_s)) {
            uc.scanner_.pulse_timer = 0.0f;
            add_tag(w, uc.scanner_.e, "vfx.spawn:scanner_pulse");
            mark_targets(w, uc.scanner_.e, uc.scanner_.range_m);
            // tiny energy sip
            add_tag(w, static_cast<entt::entity>(ring_e), "ring.energy:consume:{\"amount\":0.5}");
        }
    }
}

}} // namespace ptv::utility
