#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"
#include <entt/entt.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
// [ptv-bounds] removed disallowed include: limits
// [ptv-bounds] removed disallowed include: utility_constructs_shared.h

using namespace utility_internal;

namespace {
entt::entity spawn_hand(ptv::World& w, const ptv::Transform& at) {
    auto& reg = w.registry();
    entt::entity e = reg.create();
    std::uint32_t u = static_cast<std::uint32_t>(e);
    w.emplace_or_replace<ptv::CTransform>(u, ptv::CTransform{at});
    ptv::CTag tag; tag.tags = {"utility","grabber_hand"};
    w.emplace_or_replace<ptv::CTag>(u, std::move(tag));
    return e;
}

std::pair<entt::entity,float> find_nearest_grabbable(ptv::World& w, const ptv::Vec3& from, float maxR) {
    auto& reg = w.registry();
    auto view = reg.view<ptv::CTransform, ptv::CTag>();
    float best = std::numeric_limits<float>::max();
    entt::entity bestE = entt::null;
    const float r2 = maxR*maxR;
    for (auto e : view) {
        std::uint32_t u = static_cast<std::uint32_t>(e);
        if (!w.has<ptv::CTag>(u) || !w.has<ptv::CTransform>(u)) continue;
        auto& tags = w.get<ptv::CTag>(u).tags;
        if (std::find(tags.begin(), tags.end(), "grabbable") == tags.end()) continue;
        const auto& tr = w.get<ptv::CTransform>(u).t;
        const ptv::Vec3 d = tr.position - from;
        const float d2 = dot(d,d);
        if (d2 <= r2 && d2 < best) { best = d2; bestE = e; }
    }
    return {bestE, best};
}
}

namespace ptv {

void UtilityConstructs::tick_grabber(World& w, std::uint32_t ring_entity, const nlohmann::json& live, float dt) {
    const bool activate = utility_internal::jflag(live, "activate_grabber", false);
    const bool latch = utility_internal::jflag(live, "right_trigger_pull", false);

    if (activate && grabber_.hand == entt::null) {
        if (!w.has<ptv::CTransform>(ring_entity)) return;
        auto at = w.get<ptv::CTransform>(ring_entity).t;
        at.position += ptv::Vec3{0.0f, 0.25f, 0.5f};
        at.scale = 0.7f;
        grabber_.hand = spawn_hand(w, at);
        utility_internal::add_tag(w, ring_entity, "utility:grabber:spawned");
    }

    if (grabber_.hand != entt::null) {
        std::uint32_t h = static_cast<std::uint32_t>(grabber_.hand);
        if (!w.has<ptv::CTransform>(h)) return;
        auto& handTr = w.get<ptv::CTransform>(h).t;

        // Keep hand near ring
        if (w.has<ptv::CTransform>(ring_entity)) {
            const auto& rt = w.get<ptv::CTransform>(ring_entity).t;
            ptv::Vec3 targetPos = rt.position + ptv::Vec3{0,0.2f,0.6f};
            handTr.position = handTr.position + (targetPos - handTr.position) * std::min(1.0f, 6.0f*dt);
        }

        if (grabber_.latched == entt::null && latch) {
            // Acquire nearest grabbable within 8m
            auto [e, d2] = find_nearest_grabbable(w, handTr.position, 8.0f);
            if (e != entt::null) {
                grabber_.latched = e;
                utility_internal::add_tag(w, static_cast<std::uint32_t>(e), "tethered_to_ring");
            }
        }

        if (grabber_.latched != entt::null) {
            std::uint32_t u = static_cast<std::uint32_t>(grabber_.latched);
            if (w.has<ptv::CTransform>(u)) {
                auto& tr = w.get<ptv::CTransform>(u).t;
                // Pull toward hand (elastic)
                const ptv::Vec3 d = handTr.position - tr.position;
                tr.position += d * std::min(1.0f, (grabber_.pull_force*0.02f)*dt);
                // Visual hint via tag for core VFX to draw tether line
                utility_internal::add_tag(w, u, "vfx:tether_line");
            }
        }
    }
}

} // namespace ptv
