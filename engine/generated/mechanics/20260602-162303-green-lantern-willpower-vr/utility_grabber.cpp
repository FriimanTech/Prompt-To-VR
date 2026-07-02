#include "utility_constructs_shared.h"

#include <algorithm>

namespace ptv { namespace utility {

static entt::entity ensure_grabber(World& w, std::uint32_t ring_e, UtilityConstructs& uc) {
    if (uc.grabber_.active && uc.grabber_.e != entt::null && w.registry().valid(uc.grabber_.e)) return uc.grabber_.e;
    Transform t{}; if (w.has<CTransform>(ring_e)) t = w.get<CTransform>(ring_e).t;
    t.position += Vec3{0.0f, 0.05f, 0.25f};
    uc.grabber_.e = make_child(w, "construct.utility:grabber", t);
    uc.grabber_.active = true;
    add_tag(w, static_cast<entt::entity>(ring_e), "haptics.publish:{\"pattern_id\":\"construct_form_heavy\",\"hand\":\"right\",\"intensity_scale\":0.8,\"duration_s\":0.06}");
    return uc.grabber_.e;
}

static void pull_nearby(World& w, std::uint32_t ring_e, entt::entity grabber, float pull_force, float dt) {
    Transform* gt = get_tr(w, grabber);
    if (!gt) return;
    auto& reg = w.registry();
    auto view = reg.view<CTag, CTransform>();
    for (auto e : view) {
        if (e == grabber) continue;
        const auto& tags = view.get<CTag>(e).tags;
        bool is_small=false;
        for (const auto& s : tags) { if (s=="grabbable") { is_small=true; break; } }
        if (!is_small) continue;
        auto& tr = view.get<CTransform>(e).t;
        Vec3 d = gt->position - tr.position;
        float dist2 = d.x*d.x + d.y*d.y + d.z*d.z;
        if (dist2 > 36.0f) continue; // within 6m
        // Elastic-like pull (no physics body assumption):
        float dist = std::sqrt(std::max(1e-4f, dist2));
        Vec3 dir = (1.0f/dist) * d;
        float step = std::min(2.0f, pull_force * dt / std::max(0.5f, dist));
        tr.position = tr.position + dir * step;
        // Tether VFX hint
        add_tag(w, e, "vfx.spawn:elastic_tether");
    }
    // light drain per second when active pull requested
    add_tag(w, static_cast<entt::entity>(ring_e), "ring.energy:consume:{\"amount\":0.2}");
}

void update_grabber(World& w, std::uint32_t ring_e, UtilityConstructs& uc, const nlohmann::json& p, float dt) {
    const bool spawn = p.value("spawn_grabber", false) || (p.value("selected", std::string("")) == std::string("construct_grabber_hand") && p.value("rt_confirm", false));
    if (spawn) ensure_grabber(w, ring_e, uc);
    if (!uc.grabber_.active) return;

    const bool pulling = p.value("right_trigger_hold", false) || p.value("pull_hold", false);
    if (pulling) pull_nearby(w, ring_e, uc.grabber_.e, uc.grabber_.pull_force, dt);

    // Follow ring hand softly
    if (w.has<CTransform>(ring_e)) {
        Transform* gt = get_tr(w, uc.grabber_.e);
        const auto& hand = w.get<CTransform>(ring_e).t;
        if (gt) gt->position = gt->position + (hand.position + Vec3{0,0.05f,0.25f} - gt->position) * std::clamp(dt*8.0f, 0.0f, 1.0f);
    }
}

}} // namespace ptv::utility
