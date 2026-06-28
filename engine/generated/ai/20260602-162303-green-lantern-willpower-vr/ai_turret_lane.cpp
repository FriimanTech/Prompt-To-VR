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

static bool has_barrier_between(ptv::World& w, const ptv::Vec3& a, const ptv::Vec3& b){
    auto& reg = w.registry();
    auto view = reg.view<ptv::CTransform, ptv::CMesh>();
    const float block_r = 1.5f;
    const ptv::Vec3 ab = sub(b,a);
    const float ab2 = dot(ab,ab) + 1e-6f;
    for (auto ent : view){
        const auto& m = view.get<ptv::CMesh>(ent);
        if (m.asset_id != "construct_barrier_wall") continue;
        const auto& p = view.get<ptv::CTransform>(ent).t.position;
        const ptv::Vec3 ap = sub(p,a);
        float t = clampf(dot(ap,ab)/ab2, 0.0f, 1.0f);
        ptv::Vec3 proj = add(a, mul(ab, t));
        if (len(sub(p, proj)) < block_r) return true;
    }
    return false;
}
}

namespace enemy_ai {

void turret_tick(ptv::World& w, std::uint32_t self, const ptv::Vec3& player_pos,
                 const ptv::Vec3&, float dt, int tier, float& local_t){
    (void)dt; (void)local_t;
    if (!w.has<ptv::CTransform>(self)) return;
    const auto self_p = w.get<ptv::CTransform>(self).t.position;

    // If flanked (player close), switch to burst mode and disable melee if exists
    const float flank_d = 5.0f;
    if (len(sub(player_pos, self_p)) < flank_d){
        tag_set(w, self, "ai_state", "flanked_burst");
        tag_set(w, self, "ai_fire", std::string("burst_rps:")+std::to_string(6.0f + 0.5f*(tier-1)));
        tag_set(w, self, "turret_melee_disable", "true");
        tag_set(w, self, "ai_laser", "off");
        return;
    }

    // Establish lane: if barrier blocks LOS, strafe aim horizontally to reacquire
    bool blocked = has_barrier_between(w, self_p, player_pos);
    ptv::Vec3 aim_point = player_pos;
    if (blocked){
        ptv::Vec3 f = norm(sub(player_pos, self_p));
        ptv::Vec3 right{f.z, 0.0f, -f.x};
        float offset = (std::fmod(local_t*0.7f, 2.0f) < 1.0f)? 2.5f : -2.5f;
        aim_point = add(player_pos, mul(right, offset));
        tag_set(w, self, "ai_state", "lane_reacquire");
        tag_set(w, self, "ai_laser", "sweep");
    } else {
        tag_set(w, self, "ai_state", "lane_hold");
        tag_set(w, self, "ai_laser", "on");
    }

    // Write fire directive (single powerful shot cadence)
    float shot_period = 1.6f - 0.15f*(tier-1);
    shot_period = clampf(shot_period, 0.9f, 1.6f);
    tag_set(w, self, "ai_fire", std::string("snipe_period_s:")+std::to_string(shot_period));
    // Publish aim target so VFX/emitters can use it
    tag_set(w, self, "ai_aim_target", std::to_string(aim_point.x)+","+std::to_string(aim_point.y)+","+std::to_string(aim_point.z));
}

} // namespace enemy_ai
