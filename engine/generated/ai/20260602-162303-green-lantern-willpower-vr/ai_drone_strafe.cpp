#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"
#include <entt/entt.hpp>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>

namespace {
inline float dot(const ptv::Vec3& a, const ptv::Vec3& b){ return a.x*b.x + a.y*b.y + a.z*b.z; }
inline float len(const ptv::Vec3& v){ return std::sqrt(dot(v,v)); }
inline ptv::Vec3 sub(const ptv::Vec3& a, const ptv::Vec3& b){ return ptv::Vec3{a.x-b.x,a.y-b.y,a.z-b.z}; }
inline ptv::Vec3 add(const ptv::Vec3& a, const ptv::Vec3& b){ return ptv::Vec3{a.x+b.x,a.y+b.y,a.z+b.z}; }
inline ptv::Vec3 mul(const ptv::Vec3& v, float s){ return ptv::Vec3{v.x*s,v.y*s,v.z*s}; }
inline ptv::Vec3 norm(const ptv::Vec3& v){ float l=len(v); return (l>1e-5f)? ptv::Vec3{v.x/l,v.y/l,v.z/l} : ptv::Vec3{0,0,0}; }
inline float clampf(float v, float a, float b){ return std::max(a, std::min(v,b)); }

static void tag_set(ptv::World& w, std::uint32_t e, const std::string& key, const std::string& value){
    if (!w.has<ptv::CTag>(e)) { w.emplace_or_replace<ptv::CTag>(e, ptv::CTag{}); }
    auto& tags = w.get<ptv::CTag>(e).tags;
    const std::string prefix = key + ":";
    bool replaced=false;
    for (auto& t : tags){ if (t.rfind(prefix,0)==0){ t = prefix + value; replaced=true; break; } }
    if (!replaced) tags.push_back(prefix + value);
}

static bool inside_dome(ptv::World& w, const ptv::Vec3& p){
    auto& reg = w.registry();
    auto view = reg.view<ptv::CTransform, ptv::CMesh>();
    for (auto ent : view){
        const auto& m = view.get<ptv::CMesh>(ent);
        if (m.asset_id != "construct_dome_shield") continue;
        const auto& c = view.get<ptv::CTransform>(ent).t.position;
        if (len(sub(p,c)) < 3.5f) return true;
    }
    return false;
}
}

namespace enemy_ai {

void drone_tick(ptv::World& w, std::uint32_t self, const ptv::Vec3& player_pos,
                const ptv::Vec3& player_vel, float dt, int tier, float& local_t){
    (void)player_vel; (void)dt;
    if (!w.has<ptv::CTransform>(self)) return;
    auto& t = w.get<ptv::CTransform>(self).t;

    float near_d = 7.0f - 0.5f*(tier-1); // tighter on higher tiers
    near_d = clampf(near_d, 5.5f, 7.0f);
    float far_d = near_d + 3.0f;

    ptv::Vec3 to_p = sub(player_pos, t.position);
    float d = len(to_p);

    // Desired altitude offset and lateral juke
    float alt = 2.5f + 0.4f*(tier-1);
    ptv::Vec3 f = norm(to_p);
    ptv::Vec3 right{f.z, 0.0f, -f.x};
    float juke = std::sin(local_t*2.3f) * (1.0f + 0.3f*(tier-1));
    ptv::Vec3 desired = add(player_pos, add(mul(f, -clampf(d<near_d? near_d : (d>far_d? near_d : d), near_d, far_d)), add(mul(right, juke*2.0f), ptv::Vec3{0,alt,0})));

    // Avoid dome interiors: if target would be inside, push outward
    if (inside_dome(w, desired)){
        desired = add(desired, mul(f, 4.0f));
    }

    tag_set(w, self, "ai_state", "strafe");
    tag_set(w, self, "ai_move_target", std::to_string(desired.x)+","+std::to_string(desired.y)+","+std::to_string(desired.z));
    tag_set(w, self, "ai_speed_mps", "6.0");

    // Fire: short bursts while outside domes
    if (!inside_dome(w, t.position)){
        float rps = 4.0f + 0.5f*(tier-1);
        tag_set(w, self, "ai_fire", std::string("burst_rps:")+std::to_string(rps));
    } else {
        tag_set(w, self, "ai_fire", "hold"); // hold fire if sheltered inside
    }
}

} // namespace enemy_ai
