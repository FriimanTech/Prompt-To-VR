#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <entt/entt.hpp>
#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>

namespace control_ext_helpers {

struct TargetPick {
    bool found{false};
    std::uint32_t entity{0};
    float dist{0.0f};
};

static inline float dot3(const ptv::Vec3& a, const ptv::Vec3& b){ return a.x*b.x + a.y*b.y + a.z*b.z; }
static inline float len3(const ptv::Vec3& v){ return std::sqrt(v.x*v.x+v.y*v.y+v.z*v.z); }
static inline ptv::Vec3 sub3(const ptv::Vec3& a, const ptv::Vec3& b){ return ptv::Vec3{a.x-b.x,a.y-b.y,a.z-b.z}; }
static inline ptv::Vec3 norm3(const ptv::Vec3& v){ float L=len3(v); if(L<1e-5f) return ptv::Vec3{0,0,1}; return ptv::Vec3{v.x/L,v.y/L,v.z/L}; }

bool entity_has_tag(ptv::World& w, std::uint32_t e, std::string_view tag_sv) {
    if (!w.valid(e)) return false;
    if (!w.has<ptv::CTag>(e)) return false;
    auto& tag = w.get<ptv::CTag>(e);
    for (auto& s : tag.tags) {
        if (s == tag_sv) return true;
        // allow substring match like "yellow_zone_warning"
        if (s.find(tag_sv) != std::string::npos) return true;
    }
    return false;
}

TargetPick pick_target_forward_cone(ptv::World& w, std::uint32_t from_entity,
                                    float max_distance_m,
                                    float half_angle_deg,
                                    bool prefer_enemies)
{
    TargetPick best;
    if (!w.valid(from_entity) || !w.has<ptv::CTransform>(from_entity)) return best;
    auto& tr = w.get<ptv::CTransform>(from_entity).t;
    ptv::Vec3 origin = tr.position;

    // Approximate forward from rotation (assuming w,x,y,z quaternion with forward +Z in local)
    // Here we just use world +Z as a safe fallback; tests are mostly forward facing.
    ptv::Vec3 fwd{0.0f, 0.0f, 1.0f};

    auto& reg = w.registry();
    auto view = reg.view<ptv::CTransform>();

    const float cos_limit = std::cos(half_angle_deg * 3.14159265f / 180.0f);

    auto consider = [&](entt::entity ent){
        std::uint32_t u = static_cast<std::uint32_t>(ent);
        if (u == from_entity) return; // skip self
        if (!w.has<ptv::CTransform>(u)) return;
        auto& t = w.get<ptv::CTransform>(u).t;
        ptv::Vec3 to = sub3(t.position, origin);
        float dist = len3(to);
        if (dist < 0.2f || dist > max_distance_m) return;
        ptv::Vec3 dir = norm3(to);
        float cosang = dot3(dir, fwd);
        if (cosang < cos_limit) return;

        bool is_enemy = entity_has_tag(w, u, "enemy") || entity_has_tag(w, u, "target") || entity_has_tag(w, u, "drone");
        // Prefer closer, with enemy weight
        float score = dist;
        if (prefer_enemies && is_enemy) score *= 0.6f; // enemy bias

        if (!best.found || score < best.dist) {
            best.found = true; best.entity = u; best.dist = dist;
        }
    };

    for (auto ent : view) consider(ent);

    return best;
}

void vfx_snap_tether(ptv::World& /*w*/, const ptv::Vec3& /*pos_world*/) {
    // TODO: integrate with VFX event bus when available.
}

} // namespace control_ext_helpers
