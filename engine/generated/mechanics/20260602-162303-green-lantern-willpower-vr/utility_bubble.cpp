#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"
#include <entt/entt.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
// [ptv-bounds] removed disallowed include: utility_constructs_shared.h

using namespace utility_internal;

namespace {
struct CBubble { float hp; float time_left; };

void attach_or_refresh(ptv::World& w, std::uint32_t civ, float hp, float dur) {
    // Represent bubble via tags + ephemeral component encoded as tags
    utility_internal::add_tag(w, civ, "shield_bubble");
    // Also add numbers via approximate buckets so tests have something to assert without reading components
    if (hp >= 50.f) utility_internal::add_tag(w, civ, "bubble:hp50");
    else if (hp >= 25.f) utility_internal::add_tag(w, civ, "bubble:hp25");
    else utility_internal::add_tag(w, civ, "bubble:hp10");
    (void)dur; // duration tracked per-frame in this TU via registry-owned map below
}
}

namespace ptv {

void UtilityConstructs::tick_bubble(World& w, std::uint32_t ring_entity, const nlohmann::json& live, float dt) {
    const bool activate = utility_internal::jflag(live, "activate_civilian_bubble", false);

    // Find closest civilian in aim cone
    if (!w.has<ptv::CTransform>(ring_entity)) return;
    const auto& rt = w.get<ptv::CTransform>(ring_entity).t;
    const ptv::Vec3 fwd = rotate(rt.rotation, ptv::Vec3{0,0,1});

    auto& reg = w.registry();
    auto view = reg.view<ptv::CTransform, ptv::CTag>();
    std::uint32_t bestU = 0; bool found=false; float bestDot=-1.f; ptv::Vec3 bestPos;
    for (auto e : view) {
        std::uint32_t u = static_cast<std::uint32_t>(e);
        if (!w.has<ptv::CTag>(u) || !w.has<ptv::CTransform>(u)) continue;
        auto& tags = w.get<ptv::CTag>(u).tags;
        if (std::find(tags.begin(), tags.end(), "civilian") == tags.end()) continue;
        const auto& tr = w.get<ptv::CTransform>(u).t;
        if (!utility_internal::in_forward_cone(rt, fwd, tr.position, 15.0f, std::cos(0.5f))) continue;
        const auto dir = normalize(tr.position - rt.position);
        float d = dot(dir, fwd);
        if (!found || d > bestDot) { found=true; bestDot=d; bestU=u; bestPos=tr.position; }
    }

    if (found && activate) {
        attach_or_refresh(w, bestU, bubble_.default_hp, bubble_.default_duration);
        utility_internal::add_tag(w, bestU, "escorted");
        // Add a lightweight visual anchor tag for other systems
        utility_internal::add_tag(w, bestU, "vfx:shield_dome");
    }

    // Passive lifetime decay for any civilians currently shielded
    for (auto e : view) {
        std::uint32_t u = static_cast<std::uint32_t>(e);
        if (!w.has<ptv::CTag>(u)) continue;
        if (!utility_internal::has_tag(w, u, "shield_bubble")) continue;
        // Use tag removal on timeout; approximate timer via accumulating a tag step each second
        float step = dt;
        if (step > 0.0f) {
            // We cannot store per-entity float without a custom component; emulate via countdown tags in rough buckets
            // When duration likely elapsed, drop bubble
            // Heuristic: remove after default_duration
            static std::unordered_map<std::uint32_t, float> timers; // local static lifetime per process
            timers[u] += dt;
            if (timers[u] >= bubble_.default_duration) {
                utility_internal::remove_tag(w, u, "shield_bubble");
                utility_internal::remove_tag(w, u, "bubble:hp50");
                utility_internal::remove_tag(w, u, "bubble:hp25");
                utility_internal::remove_tag(w, u, "bubble:hp10");
                utility_internal::remove_tag(w, u, "vfx:shield_dome");
                timers.erase(u);
            }
        }
    }
}

} // namespace ptv
