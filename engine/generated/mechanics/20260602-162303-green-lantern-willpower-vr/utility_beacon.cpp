#include "utility_constructs_shared.h"

#include <algorithm>

namespace ptv { namespace utility {

static entt::entity ensure_beacon(World& w, std::uint32_t ring_e, UtilityConstructs& uc) {
    if (uc.beacon_.active && uc.beacon_.e != entt::null && w.registry().valid(uc.beacon_.e)) return uc.beacon_.e;
    Transform t{}; if (w.has<CTransform>(ring_e)) t = w.get<CTransform>(ring_e).t;
    t.position += Vec3{0.0f, -0.05f, 0.8f};
    uc.beacon_.e = make_child(w, "construct.utility:beacon", t);
    uc.beacon_.active = true;
    add_tag(w, static_cast<entt::entity>(ring_e), "haptics.publish:{\"pattern_id\":\"construct_form_heavy\",\"hand\":\"right\",\"intensity_scale\":0.7,\"duration_s\":0.06}");
    return uc.beacon_.e;
}

void update_beacon(World& w, std::uint32_t ring_e, UtilityConstructs& uc, const nlohmann::json& p, float dt) {
    const bool spawn = p.value("spawn_beacon", false) || (p.value("selected", std::string("")) == std::string("construct_beacon_searchlight") && p.value("rt_confirm", false));
    if (spawn) ensure_beacon(w, ring_e, uc);
    if (!uc.beacon_.active) return;

    // Idle rotate head
    uc.beacon_.yaw_rad += dt * 0.7f;
    Transform* bt = get_tr(w, uc.beacon_.e);
    if (bt) {
        // Simple circular drift for visual interest
        bt->position += Vec3{std::sin(uc.beacon_.yaw_rad)*0.01f, 0.0f, 0.0f};
    }

    // Publish AI perception modifier cone each tick (consumed by AI pack if present)
    add_tag(w, uc.beacon_.e, std::string("ai.perception:modifier:{\"kind\":\"illum_cone\",\"radius_m\":6.0,\"intensity\":" ) + std::to_string(std::max(0.1f, uc.beacon_.intensity)) + "}");
    add_tag(w, uc.beacon_.e, "vfx.spawn:searchlight_cone");
}

}} // namespace ptv::utility
