#include "utility_constructs_shared.h"

#include <algorithm>

namespace ptv { namespace utility {

static std::optional<entt::entity> find_repairable_in_front(World& w, std::uint32_t ring_e, float max_dist_m) {
    if (!w.has<CTransform>(ring_e)) return std::nullopt;
    const auto& hand = w.get<CTransform>(ring_e).t;
    // Approximate forward from ring rotation (0,0,1) rotated by hand.rotation is not provided; use +Z in local: assume facing scene forward.
    Vec3 fwd{0.0f, 0.0f, 1.0f};
    auto& reg = w.registry();
    auto view = reg.view<CTag, CTransform>();
    entt::entity best = entt::null; float best_d = max_dist_m;
    for (auto e : view) {
        if (static_cast<std::uint32_t>(e) == ring_e) continue;
        const auto& tags = view.get<CTag>(e).tags;
        bool is_rep=false; for (const auto& s: tags){ if (s=="repairable") { is_rep=true; break; } }
        if (!is_rep) continue;
        const auto& tr = view.get<CTransform>(e).t;
        Vec3 d = tr.position - hand.position;
        float dist = std::sqrt(d.x*d.x + d.y*d.y + d.z*d.z);
        if (dist > max_dist_m) continue;
        // simple facing check
        float facing = (d.x*fwd.x + d.y*fwd.y + d.z*fwd.z) / std::max(1e-4f, dist);
        if (facing < 0.5f) continue;
        if (dist < best_d) { best_d = dist; best = e; }
    }
    if (best==entt::null) return std::nullopt; else return best;
}

void update_repair(World& w, std::uint32_t ring_e, UtilityConstructs& uc, const nlohmann::json& p, float dt) {
    const bool begin = p.value("spawn_repair", false) || (p.value("selected", std::string("")) == std::string("construct_repair_tool_arm") && p.value("rt_confirm", false));
    if (begin && !uc.repair_.channel) {
        // No separate entity needed; start channel on target
        auto target = find_repairable_in_front(w, ring_e, 4.0f);
        if (target) {
            uc.repair_.channel = true;
            uc.repair_.target = *target;
            uc.repair_.progress01 = 0.0f;
            add_tag(w, static_cast<entt::entity>(ring_e), "haptics.publish:{\"pattern_id\":\"hover_light\",\"hand\":\"right\",\"intensity_scale\":0.7,\"duration_s\":0.05}");
        }
    }

    if (!uc.repair_.channel) return;

    // If player stops aiming (very rough), gently decay channel
    if (!find_repairable_in_front(w, ring_e, 4.5f).has_value() || find_repairable_in_front(w, ring_e, 4.5f).value()!=uc.repair_.target) {
        uc.repair_.progress01 = std::max(0.0f, uc.repair_.progress01 - dt*0.2f);
        return;
    }

    uc.repair_.progress01 = std::min(1.0f, uc.repair_.progress01 + dt * std::max(0.05f, uc.repair_.rate_per_s));
    add_tag(w, uc.repair_.target, "hud.progress:update:{\"channel\":\"repair\"}");
    add_tag(w, static_cast<entt::entity>(ring_e), "ring.energy:consume:{\"amount\":0.3}");

    if (uc.repair_.progress01 >= 1.0f) {
        // Fire completion event the door/panel logic can consume
        add_tag(w, uc.repair_.target, "event.repair:complete");
        add_tag(w, uc.repair_.target, "door.open");
        uc.repair_ = RepairState{};
    }
}

}} // namespace ptv::utility
