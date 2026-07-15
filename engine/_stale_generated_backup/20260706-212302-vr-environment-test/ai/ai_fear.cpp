#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>

#include "enemy_ai_shared.h"

namespace ptv {

void ptv_ai_tick_fear(World& w, std::uint32_t e, EnemyStateLocal& s, float dt) {
    if (!w.has<CTransform>(e)) return;
    auto& tr = w.get<CTransform>(e).t;

    // Aggressive rush
    ptv::Vec3 player = s.last_player_pos;
    ptv::Vec3 d = player - tr.position;
    float dist = std::sqrt(std::max(glm::dot(d, d), 1e-6f));
    ptv::Vec3 toP = (1.0f / dist) * d;

    float speed = 5.0f; // fast but readable
    tr.position = ptv_ai_seek(tr.position, player - toP * 1.8f, speed * dt);

    // Ring-drain aura when close
    float aura_range = 4.0f;
    if (dist < aura_range) {
        ptv_ai_add_tag(w, e, "ai.fear_aura_active");
        // We do not have CRingEnergy here; a gameplay layer can read this tag and apply
        // params.value("fear_aura_drain_per_s", 2.0) externally. This AI announces intent.
    } else {
        ptv_ai_remove_tag(w, e, "ai.fear_aura_active");
    }

    // Vulnerable to reinforced cages: if a reinforcement tag exists, reduce local stagger
    if (w.has<CTag>(e)) {
        const auto& tg = w.get<CTag>(e);
        if (ptv_ai_has_tag(tg, "caged_reinforced")) {
            // shorter breakout: emulate by longer stagger when broken
            s.t.stagger_time = std::max(s.t.stagger_time, 0.4f);
        }
    }
}

} // namespace ptv
