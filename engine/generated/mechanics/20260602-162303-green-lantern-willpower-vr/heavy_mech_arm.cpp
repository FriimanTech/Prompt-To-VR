#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"
#include <entt/entt.hpp>
#include <cmath>

namespace ptv { namespace heavy_internal {

enum class Form : std::uint8_t { None=0, Turret, MechArm, JetDrill, FistSwarm };
struct LiveConstructRef { entt::entity e{entt::null}; Form form{Form::None}; float t{0.0f}; };

void tick_mech_arm(World& w, std::uint32_t /*source*/, LiveConstructRef& ref, float dt){
    if(ref.e==entt::null || !w.registry().valid(ref.e)) return;
    auto u = static_cast<std::uint32_t>(ref.e);
    if(!w.has<CTransform>(u)) return;
    auto& tr = w.get<CTransform>(u).t;

    // Timeline: 0..0.5s wind-up (raise), 0.5..0.8s punch forward, 0.8..1.1s ground pound down, 1.1..2.0s settle, 2.0s despawn
    ref.t += dt;
    const float T0=0.0f, T1=0.5f, T2=0.8f, T3=1.1f, T4=2.0f;

    if(ref.t<T1){
        // Wind-up: slight back and up
        tr.position.y += 0.4f*dt; tr.position.z -= 0.3f*dt;
    } else if(ref.t<T2){
        // Punch forward fast
        tr.position.z += 2.6f*dt;
    } else if(ref.t<T3){
        // Slam down
        tr.position.y -= 2.8f*dt;
    } else if(ref.t<T4){
        // Settle: small oscillation
        tr.position.y += 0.06f*std::sin((ref.t - T3)*18.0f)*dt;
    } else {
        if(w.registry().valid(ref.e)) w.registry().destroy(ref.e); ref.e=entt::null;
        return;
    }

    // Shockwave tag at punch and pound markers (for VFX systems to pick up)
    if(std::fabs(ref.t-0.8f)<0.016f || std::fabs(ref.t-1.0f)<0.016f){
        if(w.has<CTag>(u)){
            auto tg = w.get<CTag>(u);
            tg.tags.push_back("shockwave");
            w.emplace_or_replace<CTag>(u, std::move(tg));
        }
    }
}

}} // namespace ptv::heavy_internal
