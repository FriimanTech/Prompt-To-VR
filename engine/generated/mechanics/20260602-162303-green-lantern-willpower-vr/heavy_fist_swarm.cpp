#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"
#include <entt/entt.hpp>
#include <vector>
#include <cmath>
#include <string>
#include <optional>
#include <cfloat>
#include <algorithm>

namespace ptv { namespace heavy_internal {

enum class Form : std::uint8_t { None=0, Turret, MechArm, JetDrill, FistSwarm };
struct LiveConstructRef { entt::entity e{entt::null}; Form form{Form::None}; float t{0.0f}; };

static entt::entity spawn_basic(World& w, const std::string& id_hint, const std::string& asset_id, const std::string& glb,
                                const Transform& t, float target_scale=1.0f){
    auto& reg = w.registry();
    entt::entity e = reg.create();
    std::uint32_t u = static_cast<std::uint32_t>(e);
    w.emplace_or_replace<CTransform>(u, CTransform{t});
    CMesh m{}; m.asset_id=asset_id; m.glb_path=glb; m.target_scale_meters=target_scale; w.emplace_or_replace<CMesh>(u, std::move(m));
    CPhysics p{}; p.kind="dynamic"; p.mass_kg=1.0f; p.restitution=0.1f; p.friction=0.6f; p.collision_shape="convex"; w.emplace_or_replace<CPhysics>(u, std::move(p));
    CTag tag{}; tag.tags = {"construct", id_hint}; w.emplace_or_replace<CTag>(u, std::move(tag));
    return e;
}

static std::optional<Vec3> nearest_enemy_pos(World& w, const Vec3& from){
    auto& reg = w.registry();
    float best=FLT_MAX; Vec3 pos{}; bool found=false;
    auto view = reg.view<CTransform, CTag>();
    for(auto ent : view){
        auto& tr = view.get<CTransform>(ent).t;
        const CTag& tg = view.get<CTag>(ent);
        if(std::find(tg.tags.begin(), tg.tags.end(), std::string("enemy"))==tg.tags.end()) continue;
        Vec3 d{tr.position.x-from.x, tr.position.y-from.y, tr.position.z-from.z};
        float ds = d.x*d.x+d.y*d.y+d.z*d.z;
        if(ds < best){ best=ds; pos = tr.position; found=true; }
    }
    if(found) return pos; else return std::nullopt;
}

static void ensure_children(World& w, LiveConstructRef& ref, int count){
    // Spawn child fists around anchor as separate entities tagged with parent id
    auto anchor_u = static_cast<std::uint32_t>(ref.e);
    if(!w.has<CTransform>(anchor_u)) return;
    auto& atr = w.get<CTransform>(anchor_u).t;
    auto& reg = w.registry();
    // Count existing children by tag match
    int existing = 0; auto view = reg.view<CTag>();
    for(auto ent : view){ const CTag& tg = view.get<CTag>(ent); for(auto& s: tg.tags){ if(s=="fist_swarm_child") { existing++; break; } } }
    for(int i=existing;i<count;i++){
        float ang = (6.2831853f * i) / std::max(1,count);
        Transform t = atr; t.position.x += 0.6f*std::cos(ang); t.position.z += 0.6f*std::sin(ang);
        auto child = spawn_basic(w, "fist_swarm_child", "construct_fist_swarm", "assets/construct_fist_swarm.glb", t, 0.4f);
        auto u = static_cast<std::uint32_t>(child);
        if(w.has<CTag>(u)){
            auto tg = w.get<CTag>(u);
            tg.tags.push_back("fist_swarm_child");
            w.emplace_or_replace<CTag>(u, std::move(tg));
        }
    }
}

void tick_fist_swarm(World& w, std::uint32_t /*source*/, LiveConstructRef& ref, float dt){
    if(ref.e==entt::null || !w.registry().valid(ref.e)) return;
    auto u = static_cast<std::uint32_t>(ref.e);
    if(!w.has<CTransform>(u)) return;
    auto& tr = w.get<CTransform>(u).t;

    ref.t += dt;
    const float lifetime = 8.0f;
    ensure_children(w, ref, 6);

    // Orbiting motion for children
    auto& reg = w.registry();
    auto view = reg.view<CTransform, CTag>();
    int idx=0;
    for(auto ent : view){
        auto& tg = view.get<CTag>(ent);
        bool is_child=false; for(auto& s: tg.tags){ if(s=="fist_swarm_child"){ is_child=true; break; } }
        if(!is_child) continue;
        auto& ct = view.get<CTransform>(ent).t;
        float ang = (6.2831853f * idx)/6 + ref.t*1.8f;
        ct.position.x = tr.position.x + 0.8f*std::cos(ang);
        ct.position.y = tr.position.y + 0.15f*std::sin(ref.t*3.0f + idx);
        ct.position.z = tr.position.z + 0.8f*std::sin(ang);
        idx++;
        // Dash burst toward nearest enemy every 1.5s window start
        if(std::fmod(ref.t + 0.1f*idx, 1.5f) < dt){
            auto near = nearest_enemy_pos(w, ct.position);
            if(near){ ct.position.x = 0.5f*ct.position.x + 0.5f*near->x; ct.position.y = 0.5f*ct.position.y + 0.5f*near->y; ct.position.z = 0.5f*ct.position.z + 0.5f*near->z; }
        }
    }

    if(ref.t>lifetime){
        // Destroy children and anchor
        for(auto ent : view){ auto& tg = view.get<CTag>(ent); for(auto& s: tg.tags){ if(s=="fist_swarm_child"){ reg.destroy(ent); break; } } }
        if(reg.valid(ref.e)) reg.destroy(ref.e); ref.e=entt::null;
    }
}

}} // namespace ptv::heavy_internal
