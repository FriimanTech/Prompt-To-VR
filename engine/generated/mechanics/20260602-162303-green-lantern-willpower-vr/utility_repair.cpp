#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"
#include <entt/entt.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <unordered_map>
// [ptv-bounds] removed disallowed include: utility_constructs_shared.h

using namespace utility_internal;

namespace {
std::optional<std::pair<std::uint32_t, float>> find_repair_target(ptv::World& w, std::uint32_t ring_entity) {
    if (!w.has<ptv::CTransform>(ring_entity)) return std::nullopt;
    const auto& rt = w.get<ptv::CTransform>(ring_entity).t;
    const ptv::Vec3 fwd = rotate(rt.rotation, ptv::Vec3{0,0,1});
    auto& reg = w.registry();
    auto view = reg.view<ptv::CTransform, ptv::CTag>();
    float bestDot = 0.0f;
    std::uint32_t bestU = 0;
    bool found = false;
    for (auto e : view) {
        std::uint32_t u = static_cast<std::uint32_t>(e);
        if (!w.has<ptv::CTag>(u)) continue;
        auto& tags = w.get<ptv::CTag>(u).tags;
        if (std::find(tags.begin(), tags.end(), "repairable") == tags.end()) continue;
        const auto& tr = w.get<ptv::CTransform>(u).t;
        if (!utility_internal::in_forward_cone(rt, fwd, tr.position, 12.0f, std::cos(0.35f))) continue;
        const ptv::Vec3 dir = normalize(tr.position - rt.position);
        float d = dot(dir, fwd);
        if (!found || d > bestDot) { found = true; bestDot = d; bestU = u; }
    }
    if (!found) return std::nullopt;
    return std::make_optional(std::make_pair(bestU, bestDot));
}
}

namespace ptv {

void UtilityConstructs::tick_repair(World& w, std::uint32_t ring_entity, const nlohmann::json& live, float dt) {
    const bool activate = utility_internal::jflag(live, "activate_repair_tool", false);
    const bool channel = utility_internal::jflag(live, "right_trigger_channel", false);

    if (!activate) { repair_.current = entt::null; return; }

    // Pick target in a forward cone
    auto cand = find_repair_target(w, ring_entity);
    if (!cand.has_value()) { repair_.current = entt::null; return; }

    std::uint32_t target = cand->first;
    if (channel) {
        float& p = repair_.progress[static_cast<entt::entity>(target)];
        p += repair_.rate * dt;
        utility_internal::add_tag(w, target, "ui:repair_in_progress");
        if (p >= 1.0f) {
            p = 1.0f;
            utility_internal::remove_tag(w, target, "ui:repair_in_progress");
            utility_internal::add_tag(w, target, "repaired");
            utility_internal::add_tag(w, target, "opened"); // door/panel open hook for tests
        }
    } else {
        // Show aim hint
        utility_internal::add_tag(w, target, "ui:repair_hint");
    }
}

} // namespace ptv
