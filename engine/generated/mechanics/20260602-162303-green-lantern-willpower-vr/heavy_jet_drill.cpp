#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"
#include <entt/entt.hpp>
#include <algorithm>

namespace ptv { namespace heavy_internal {

enum class Form : std::uint8_t { None=0, Turret, MechArm, JetDrill, FistSwarm };
struct LiveConstructRef { entt::entity e{entt::null}; Form form{Form::None}; float t{0.0f}; };

void tick_jet_drill(World& w, std::uint32_t /*source*/, LiveConstructRef& ref, float dt){
    if(ref.e==entt::null || !w.registry().valid(ref.e)) return;
    auto u = static_cast<std::uint32_t>(ref.e);
    if(!w.has<CTransform>(u)) return;
    auto& tr = w.get<CTransform>(u).t;

    // Rooted cone, armor-break hint via tags. Sustains ~ 4s then despawns.
    ref.t += dt;
    if(w.has<CTag>(u)){
        auto tg = w.get<CTag>(u);
        if(std::find(tg.tags.begin(), tg.tags.end(), std::string("armor_break_x2"))==tg.tags.end()) tg.tags.push_back("armor_break_x2");
        if(std::find(tg.tags.begin(), tg.tags.end(), std::string("continuous_beam"))==tg.tags.end()) tg.tags.push_back("continuous_beam");
        w.emplace_or_replace<CTag>(u, std::move(tg));
    }

    // Visual trembling to sell energy output, but keep camera-safe (world anchored)
    tr.position.x += 0.01f * std::sin(ref.t*37.0f) * dt;

    if(ref.t>4.0f){ if(w.registry().valid(ref.e)) w.registry().destroy(ref.e); ref.e=entt::null; }
}

}} // namespace ptv::heavy_internal
