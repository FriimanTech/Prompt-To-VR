#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"
#include <entt/entt.hpp>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <algorithm>
// [ptv-bounds] removed disallowed include: utility_constructs_shared.h

namespace ptv {
class UtilityConstructs; // fwd
}

using namespace utility_internal;

namespace {
// Local helpers
entt::entity spawn_drone(ptv::World& w, const ptv::Transform& at) {
    auto& reg = w.registry();
    entt::entity e = reg.create();
    std::uint32_t u = static_cast<std::uint32_t>(e);
    w.emplace_or_replace<ptv::CTransform>(u, ptv::CTransform{at});
    ptv::CTag tag; tag.tags = {"drone","scanner_drone","utility"};
    w.emplace_or_replace<ptv::CTag>(u, std::move(tag));
    return e;
}

void mark_targets(ptv::World& w, const ptv::Vec3& pos, float range) {
    auto& reg = w.registry();
    auto view = reg.view<ptv::CTransform, ptv::CTag>();
    const float r2 = range*range;
    for (auto e : view) {
        std::uint32_t u = static_cast<std::uint32_t>(e);
        if (!w.has<ptv::CTransform>(u) || !w.has<ptv::CTag>(u)) continue;
        auto& tr = w.get<ptv::CTransform>(u).t;
        auto& tags = w.get<ptv::CTag>(u).tags;
        const bool is_target = (std::find(tags.begin(), tags.end(), "enemy")!=tags.end()) ||
                               (std::find(tags.begin(), tags.end(), "objective")!=tags.end());
        if (!is_target) continue;
        const ptv::Vec3 d = tr.position - pos;
        if (dot(d,d) <= r2) {
            utility_internal::add_tag(w, u, "hud:scanner_mark");
            utility_internal::add_tag(w, u, "revealed");
        }
    }
}
}

namespace ptv {

void UtilityConstructs::tick_scanner(World& w, std::uint32_t ring_entity, const nlohmann::json& live, float dt) {
    const bool activate = utility_internal::jflag(live, "activate_scanner", false);
    const bool toggle = utility_internal::jflag(live, "left_trigger_toggle_scan", false);
    const ptv::Vec3 guide = utility_internal::jvec3(live, "left_grip_guide_dir", {0,0,0});

    if (activate && scanner_.drone == entt::null) {
        if (!w.has<ptv::CTransform>(ring_entity)) return;
        auto at = w.get<ptv::CTransform>(ring_entity).t;
        at.position += ptv::Vec3{0.0f, 0.35f, 0.6f};
        scanner_.drone = spawn_drone(w, at);
        scanner_.enabled = true;
        scanner_.t_accum = 0.0f;
        utility_internal::add_tag(w, ring_entity, "utility:scanner:spawned");
    }

    if (scanner_.drone != entt::null) {
        std::uint32_t d = static_cast<std::uint32_t>(scanner_.drone);
        if (!w.has<ptv::CTransform>(d)) return;
        auto& tr = w.get<ptv::CTransform>(d).t;
        // Gentle guidance by left grip vector (world-space nudge)
        tr.position += guide * (0.6f * dt);
        tr.scale = 0.6f;

        if (toggle) scanner_.enabled = !scanner_.enabled;
        if (scanner_.enabled) {
            scanner_.t_accum += dt;
            if (scanner_.t_accum >= std::max(0.1f, scanner_.scan_interval)) {
                scanner_.t_accum = 0.0f;
                mark_targets(w, tr.position, scanner_.range);
                // Tag drone so HUD/VFX systems can pick up a pulse
                utility_internal::add_tag(w, d, "scanner:pulse");
            }
        } else {
            utility_internal::remove_tag(w, d, "scanner:pulse");
        }
    }
}

} // namespace ptv
