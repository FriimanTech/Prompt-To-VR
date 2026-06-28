#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"
#include <entt/entt.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
// [ptv-bounds] removed disallowed include: utility_constructs_shared.h

using namespace utility_internal;

namespace {
entt::entity spawn_beacon(ptv::World& w, const ptv::Transform& at) {
    auto& reg = w.registry();
    entt::entity e = reg.create();
    std::uint32_t u = static_cast<std::uint32_t>(e);
    w.emplace_or_replace<ptv::CTransform>(u, ptv::CTransform{at});
    ptv::CTag tag; tag.tags = {"utility","beacon","searchlight"};
    w.emplace_or_replace<ptv::CTag>(u, std::move(tag));
    return e;
}
}

namespace ptv {

void UtilityConstructs::tick_beacon(World& w, std::uint32_t ring_entity, const nlohmann::json& live, float dt) {
    const bool activate = utility_internal::jflag(live, "activate_beacon", false);

    if (activate && beacon_.beacon == entt::null) {
        if (!w.has<ptv::CTransform>(ring_entity)) return;
        auto at = w.get<ptv::CTransform>(ring_entity).t;
        at.position += ptv::Vec3{0.0f, 0.0f, 2.0f};
        at.scale = 1.0f;
        beacon_.beacon = spawn_beacon(w, at);
        utility_internal::add_tag(w, ring_entity, "utility:beacon:spawned");
    }

    if (beacon_.beacon != entt::null) {
        std::uint32_t b = static_cast<std::uint32_t>(beacon_.beacon);
        if (!w.has<ptv::CTransform>(b)) return;
        auto& tr = w.get<ptv::CTransform>(b).t;
        beacon_.head_rot_t += dt;
        // Simple rotating head emulation via tag that encodes intensity
        utility_internal::add_tag(w, b, "light:active");
        // Affect nearby enemies: add perception debuff tag within cone in front of beacon
        const ptv::Vec3 fwd = rotate(tr.rotation, ptv::Vec3{0,0,1});
        auto& reg = w.registry();
        auto view = reg.view<ptv::CTransform, ptv::CTag>();
        for (auto e : view) {
            std::uint32_t u = static_cast<std::uint32_t>(e);
            if (!w.has<ptv::CTag>(u)) continue;
            auto& tags = w.get<ptv::CTag>(u).tags;
            if (std::find(tags.begin(), tags.end(), "enemy") == tags.end()) continue;
            const auto& etr = w.get<ptv::CTransform>(u).t;
            if (utility_internal::in_forward_cone(tr, fwd, etr.position, 20.0f, std::cos(0.45f))) {
                utility_internal::add_tag(w, u, "ai:ambush_reduced");
                utility_internal::add_tag(w, u, "revealed");
            }
        }
    }
}

} // namespace ptv
