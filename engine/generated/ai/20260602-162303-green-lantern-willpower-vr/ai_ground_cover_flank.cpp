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
    using namespace std;
    if (!w.has<ptv::CTag>(e)) { w.emplace_or_replace<ptv::CTag>(e, ptv::CTag{}); }
    auto& tags = w.get<ptv::CTag>(e).tags;
    // replace or append key:value
    const std::string prefix = key + ":";
    bool replaced=false;
    for (auto& t : tags){ if (t.rfind(prefix,0)==0){ t = prefix + value; replaced=true; break; } }
    if (!replaced) tags.push_back(prefix + value);
}

static bool has_barrier_between(ptv::World& w, const ptv::Vec3& a, const ptv::Vec3& b){
    // Approximate: any construct_barrier_wall within 1.5m of AB segment blocks LOS
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

static bool is_caged(ptv::World& w, std::uint32_t self){
    auto& reg = w.registry();
    auto view = reg.view<ptv::CTransform, ptv::CMesh>();
    if (!w.has<ptv::CTransform>(self)) return false;
    const auto pos = w.get<ptv::CTransform>(self).t.position;
    for (auto ent : view){
        const auto& m = view.get<ptv::CMesh>(ent);
        if (m.asset_id != "construct_cage") continue;
        const auto& p = view.get<ptv::CTransform>(ent).t.position;
        if (len(sub(pos,p)) < 2.0f) return true; // inside / near cage
    }
    return false;
}
}

namespace enemy_ai {

void ground_tick(ptv::World& w, std::uint32_t self, const ptv::Vec3& player_pos,
                 const ptv::Vec3&, float dt, int tier, float& local_t){
    if (!w.has<ptv::CTransform>(self)) return;
    auto& t = w.get<ptv::CTransform>(self).t;

    // States via tags: cover, flank, burst, break_attempt
    const ptv::Vec3 to_player = sub(player_pos, t.position);
    const float d = len(to_player);

    // If caged, switch to break_attempt: move to nearest cage piece
    if (is_caged(w, self)){
        tag_set(w, self, "ai_state", "break_attempt");
        // Movement target = player's direction but short radius (hammer cage)
        ptv::Vec3 tgt = add(t.position, mul(norm(to_player), clampf(d-0.5f, 0.0f, 1.0f)));
        tag_set(w, self, "ai_move_target", std::to_string(tgt.x)+","+std::to_string(tgt.y)+","+std::to_string(tgt.z));
        tag_set(w, self, "ai_melee", "windup:0.4");
        return;
    }

    // Barrier-aware: if barrier blocks LOS, attempt flank by offsetting perpendicular to player line
    bool blocked = has_barrier_between(w, t.position, player_pos);
    float desired_range = 8.0f - 1.0f * (tier-1); // closer on higher tiers
    desired_range = clampf(desired_range, 4.5f, 8.0f);

    ptv::Vec3 desired = t.position;
    if (blocked){
        tag_set(w, self, "ai_state", "flank");
        ptv::Vec3 f = norm(to_player);
        ptv::Vec3 right{f.z, 0.0f, -f.x};
        float side = (std::fmod(local_t*0.5f, 2.0f) < 1.0f)? 1.0f : -1.0f;
        desired = add(player_pos, add(mul(f, -desired_range), mul(right, side*3.0f)));
    } else {
        // Take cover if too close; else strafe and burst fire
        if (d < desired_range*0.6f){
            tag_set(w, self, "ai_state", "cover_seek");
            // Move backwards and a bit lateral
            ptv::Vec3 f = norm(to_player);
            ptv::Vec3 right{f.z, 0.0f, -f.x};
            desired = add(t.position, add(mul(f, -2.0f), mul(right, std::sin(local_t*2.0f)*1.0f)));
        } else {
            tag_set(w, self, "ai_state", "burst");
            desired = add(player_pos, mul(norm(to_player), -desired_range));
            // Fire control
            float rps = 3.0f + 0.5f*(tier-1);
            tag_set(w, self, "ai_fire", std::string("burst_rps:")+std::to_string(rps));
        }
    }

    // Write movement target and simple speed hint
    tag_set(w, self, "ai_move_target", std::to_string(desired.x)+","+std::to_string(desired.y)+","+std::to_string(desired.z));
    tag_set(w, self, "ai_speed_mps", blocked? "4.5" : "3.5");
}

} // namespace enemy_ai
