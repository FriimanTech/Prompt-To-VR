#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>

#include "enemy_ai_shared.h"

namespace ptv {

void ptv_ai_tick_mech(World& w, std::uint32_t e, EnemyStateLocal& s, float dt) {
    if (!w.has<CTransform>(e)) return;
    auto& tr = w.get<CTransform>(e).t;

    // Slow patrol toward a firing position ~15-20m, then anchor
    ptv::Vec3 player = s.last_player_pos;
    ptv::Vec3 dir = player - tr.position;
    float dist = std::sqrt(std::max(glm::dot(dir, dir), 1e-6f));
    ptv::Vec3 toP = (1.0f / dist) * dir;

    float hold_min = 15.0f, hold_max = 20.0f;
    float desired = 0.5f * (hold_min + hold_max);

    if (dist > hold_max) {
        tr.position = ptv_ai_seek(tr.position, player - toP * desired, 2.0f * dt); // heavy slow
        ptv_ai_remove_tag(w, e, "ai.anchor");
    } else if (dist < hold_min) {
        tr.position = ptv_ai_seek(tr.position, player - toP * desired, 2.0f * dt);
        ptv_ai_remove_tag(w, e, "ai.anchor");
    } else {
        // Anchor and volley
        ptv_ai_add_tag(w, e, "ai.anchor");
        if (s.t.fire_cooldown <= 0.0f) {
            ptv_ai_add_tag(w, e, "ai.volley_cannon"); // AOE tell handled by VFX
            s.t.fire_cooldown = 6.0f;
        } else {
            ptv_ai_remove_tag(w, e, "ai.volley_cannon");
        }
    }

    // Ground pound if very close and roughly ground-level
    if (dist < 5.0f && std::abs(tr.position.y) < 1.0f) {
        ptv_ai_add_tag(w, e, "ai.ground_pound");
    } else {
        ptv_ai_remove_tag(w, e, "ai.ground_pound");
    }

    // React to heavy constructs: if tagged, open stagger window
    if (w.has<CTag>(e)) {
        const auto& tg = w.get<CTag>(e);
        if (ptv_ai_has_tag(tg, "hit_by_battering_ram")) {
            s.t.stagger_time = 1.2f; // long stagger for readability
        }
    }
}

} // namespace ptv
