#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"
#include <entt/entt.hpp>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <algorithm>

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

static std::optional<std::uint32_t> find_player_entity(ptv::World& w){
    auto& reg = w.registry();
    auto view = reg.view<ptv::CTag>();
    for (auto ent : view){
        const auto& tag = view.get<ptv::CTag>(ent);
        if (std::find(tag.tags.begin(), tag.tags.end(), std::string("player_avatar")) != tag.tags.end()){
            return static_cast<std::uint32_t>(ent);
        }
    }
    return std::nullopt;
}
}

namespace enemy_ai {

void brute_tick(ptv::World& w, std::uint32_t self, const ptv::Vec3& player_pos,
                const ptv::Vec3&, float dt, int tier, float& local_t){
    if (!w.has<ptv::CTransform>(self)) return;
    auto& t = w.get<ptv::CTransform>(self).t;

    // Suppression aura (yellow zone) — publish on self and notify player via tag multiplier
    float aura_r = 6.0f + 0.5f*(tier-1);
    float aura_mult = 1.25f + 0.1f*(tier-1);
    tag_set(w, self, "aura_suppression", std::string("r:")+std::to_string(aura_r)+",mult:"+std::to_string(aura_mult));

    if (auto pe = find_player_entity(w)){
        tag_set(w, *pe, "willpower_drain_multiplier", std::to_string(aura_mult)); // Consumed by promoted.ring_charge_and_recharge
    }

    // Rush behaviour with tether resistance implied (no external tether system here)
    ptv::Vec3 to_p = sub(player_pos, t.position);
    float d = len(to_p);

    // Telegraph when in melee range
    float swing_range = 2.4f;
    if (d < swing_range){
        // Windup for heavy swing; alternate left/right
        bool left = (static_cast<int>(local_t*1.3f) % 2)==0;
        tag_set(w, self, "ai_state", left? "windup_left" : "windup_right");
        tag_set(w, self, "ai_melee", "windup:0.6,heavy:true");
        // Small anchor move to keep pressure
        ptv::Vec3 desired = add(t.position, mul(norm(to_p), clampf(d-1.4f, 0.0f, 0.6f)));
        tag_set(w, self, "ai_move_target", std::to_string(desired.x)+","+std::to_string(desired.y)+","+std::to_string(desired.z));
        tag_set(w, self, "ai_speed_mps", "3.2");
        return;
    }

    // Otherwise rush in, roaring (telegraph via state tag for VFX)
    tag_set(w, self, "ai_state", "rush");
    float rush_speed = 4.5f + 0.4f*(tier-1);
    rush_speed = clampf(rush_speed, 4.5f, 6.0f);
    ptv::Vec3 desired = add(player_pos, mul(norm(to_p), -1.8f));
    tag_set(w, self, "ai_move_target", std::to_string(desired.x)+","+std::to_string(desired.y)+","+std::to_string(desired.z));
    tag_set(w, self, "ai_speed_mps", std::to_string(rush_speed));
}

} // namespace enemy_ai
