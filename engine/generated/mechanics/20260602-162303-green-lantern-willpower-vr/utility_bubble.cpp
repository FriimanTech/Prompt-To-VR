#include "utility_constructs_shared.h"

#include <algorithm>

namespace ptv { namespace utility {

static std::optional<entt::entity> find_near_civilian(World& w, std::uint32_t ring_e, float max_dist_m) {
    if (!w.has<CTransform>(ring_e)) return std::nullopt;
    const auto& hand = w.get<CTransform>(ring_e).t;
    auto& reg = w.registry();
    auto view = reg.view<CTag, CTransform>();
    entt::entity best = entt::null; float best_d = max_dist_m;
    for (auto e : view) {
        const auto& tags = view.get<CTag>(e).tags;
        bool civ=false; for (const auto& s: tags){ if (s=="civilian") { civ=true; break; } }
        if (!civ) continue;
        const auto& tr = view.get<CTransform>(e).t;
        Vec3 d = tr.position - hand.position;
        float dist = std::sqrt(d.x*d.x + d.y*d.y + d.z*d.z);
        if (dist < best_d) { best_d = dist; best = e; }
    }
    if (best==entt::null) return std::nullopt; else return best;
}

static entt::entity spawn_bubble(World& w, entt::entity target, UtilityConstructs& uc) {
    // Spawn at target
    Transform t{}; auto* tt = get_tr(w, target); if (tt) t = *tt;
    entt::entity b = make_child(w, "construct.utility:bubble", t);
    uc.bubble_.e = b; uc.bubble_.active = true; uc.bubble_.protected_target = target;
    add_tag(w, b, "shield:enable");
    add_tag(w, target, "hud.marker:add:{\"pack\":\"ring_hud_ui_pack\",\"style\":\"escort_protect\",\"duration_s\":2.0}");
    return b;
}

void update_bubble(World& w, std::uint32_t ring_e, UtilityConstructs& uc, const nlohmann::json& p, float dt) {
    const bool spawn = p.value("spawn_bubble", false) || (p.value("selected", std::string("")) == std::string("civilian_shield_bubble") && p.value("rt_confirm", false));
    if (spawn && !uc.bubble_.active) {
        auto civ = find_near_civilian(w, ring_e, 6.0f);
        if (civ) {
            spawn_bubble(w, *civ, uc);
            add_tag(w, static_cast<entt::entity>(ring_e), "haptics.publish:{\"pattern_id\":\"construct_form_heavy\",\"hand\":\"right\",\"intensity_scale\":1.0,\"duration_s\":0.08}");
            add_tag(w, static_cast<entt::entity>(ring_e), "ring.energy:consume:{\"amount\":2.0}");
        }
    }

    if (!uc.bubble_.active) return;

    // Follow protected target
    if (uc.bubble_.protected_target != entt::null) {
        Transform* bt = get_tr(w, uc.bubble_.e);
        Transform* tt = get_tr(w, uc.bubble_.protected_target);
        if (bt && tt) bt->position = tt->position;
    }

    // Lifetime & simple damage simulation hook: external systems can decrement via tag
    uc.bubble_.ttl_s -= dt;
    if (uc.bubble_.ttl_s <= 0.0f || uc.bubble_.hp <= 0.0f) {
        if (w.registry().valid(uc.bubble_.e)) w.registry().destroy(uc.bubble_.e);
        uc.bubble_ = BubbleState{};
        return;
    }

    // Expose current hp via tag for HUD
    add_tag(w, uc.bubble_.e, std::string("shield.hp:update:{\"hp\":") + std::to_string(uc.bubble_.hp) + ",\"ttl_s\":" + std::to_string(std::max(0.0f, uc.bubble_.ttl_s)) + "}");
}

}} // namespace ptv::utility
