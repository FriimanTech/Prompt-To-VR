#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>

#include "enemy_ai_shared.h"

namespace ptv {

void ptv_ai_tick_raider(World& w, std::uint32_t e, EnemyStateLocal& s, float dt) {
    if (!w.has<CTransform>(e)) return;
    auto& tr = w.get<CTransform>(e).t;

    // Prefer readable lateral flanks, never teleport behind
    ptv::Vec3 player = s.last_player_pos;
    ptv::Vec3 dir = player - tr.position;
    float d2 = glm::dot(dir, dir);
    float d = std::sqrt(std::max(d2, 1e-6f));
    ptv::Vec3 toP = (1.0f / d) * dir;

    // Choose a lateral offset target mimicking a cover hop
    ptv::Vec3 right{toP.z, 0.0f, -toP.x}; // simple right vector on ground plane
    float flank_sign = ((e & 1u) ? 1.0f : -1.0f);
    float desired_range = 9.0f; // keep some distance, peek-fire

    ptv::Vec3 cover_like = player - toP * desired_range + right * flank_sign * 2.5f;
    cover_like.y = 0.0f; // ground

    // If close (<3.2m), attempt melee charge forward
    bool charge = (d < 3.2f);

    float speed = charge ? 6.5f : 3.2f; // readable ground speeds
    tr.position = ptv_ai_seek(tr.position, charge ? (player - toP * 0.8f) : cover_like, speed * dt);

    // Grenade occasionally when mid-range and cooldown ready
    if (!charge && d > 6.0f && d < 14.0f && s.t.fire_cooldown <= 0.0f) {
        ptv_ai_add_tag(w, e, "ai.throw_grenade"); // VFX/logic elsewhere reads this
        s.t.fire_cooldown = 4.0f;
    } else {
        ptv_ai_remove_tag(w, e, "ai.throw_grenade");
    }

    // Focus-fire dome shield: if self has a tag hint set by shield system, burst
    if (w.has<CTag>(e)) {
        const auto& tg = w.get<CTag>(e);
        if (ptv_ai_has_tag(tg, "player_dome_nearby")) {
            ptv_ai_add_tag(w, e, "ai.focus_fire");
        } else {
            ptv_ai_remove_tag(w, e, "ai.focus_fire");
        }
    }
}

} // namespace ptv
