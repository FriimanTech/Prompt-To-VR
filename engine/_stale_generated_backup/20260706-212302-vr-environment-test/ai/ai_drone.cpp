#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>

#include "enemy_ai_shared.h"

namespace ptv {

static ptv::Vec3 estimate_forward_from_rotation(const ptv::Quat& q) {
    // Assuming engine's Quat is w,x,y,z; forward = rotate (0,0,1)
    // Minimal placeholder: face towards +Z ignoring rotation specifics
    (void)q; return ptv::Vec3{0.0f, 0.0f, 1.0f};
}

void ptv_ai_tick_drone(World& w, std::uint32_t e, EnemyStateLocal& s, float dt) {
    if (!w.has<CTransform>(e)) return;
    auto& tr = w.get<CTransform>(e).t;

    // Desired ring: 6-12m
    const float minR = 6.0f;
    const float maxR = 12.0f;
    ptv::Vec3 center = s.last_player_pos;

    // Maintain altitude around player's head height + offset
    float desiredY = std::max(center.y + 1.0f, 2.0f + s.desired_altitude);

    // Orbit progression and strafing
    s.orbit_angle = ptv_ai_angle_wrap(s.orbit_angle + dt * 0.6f);
    float radius = minR + 0.5f * (maxR - minR) * (1.0f + std::sin(0.7f * s.orbit_angle));

    // Separation: repel from nearby drones (simple)
    ptv::Vec3 sep{0.0f, 0.0f, 0.0f};
    int sep_count = 0;
    auto& reg = w.registry();
    auto v = reg.view<ptv::CTransform, ptv::CTag>();
    for (auto other : v) {
      auto oid = static_cast<std::uint32_t>(other);
      if (oid == e) continue;
      if (!w.has<ptv::CTag>(oid)) continue;
      const auto& tg = w.get<ptv::CTag>(oid);
      if (!ptv_ai_has_tag(tg, "enemy")) continue;
      if (!ptv_ai_has_tag(tg, "flying")) continue;
      const auto& otr = w.get<ptv::CTransform>(oid).t;
      ptv::Vec3 d = tr.position - otr.position;
      float d2 = glm::dot(d, d);
      if (d2 < 4.0f) { // within 2m
        float inv = 1.0f / std::max(std::sqrt(std::max(d2, 1e-4f)), 0.2f);
        sep += inv * d;
        sep_count++;
      }
    }
    if (sep_count > 0) sep = (1.0f / static_cast<float>(sep_count)) * sep;

    // Dodge when recently tagged by beam effect
    if (w.has<CTag>(e)) {
        const auto& tg = w.get<CTag>(e);
        if (ptv_ai_has_tag(tg, "beam_sustained_on_enemy") && s.t.dodge_cooldown <= 0.0f) {
            s.orbit_angle += 1.57f; // quarter turn burst
            s.t.dodge_cooldown = 1.0f;
        }
    }

    // Target orbital point plus separation
    ptv::Vec3 orbit_pos = ptv_ai_orbit_point(center, radius, s.orbit_angle, desiredY);
    orbit_pos += 0.6f * sep; // bias away from neighbors

    // Step towards point with hover speed
    float speed = 6.0f; // m/s readable
    ptv::Vec3 next = ptv_ai_seek(tr.position, orbit_pos, speed * dt);
    tr.position = next;

    // Fire bursts if within frontal cone (readability: tracers handled by VFX systems)
    ptv::Vec3 fwd = estimate_forward_from_rotation(tr.rotation);
    if (ptv_ai_in_cone(tr.position, fwd, center, 25.0f, 20.0f)) {
        if (s.t.fire_cooldown <= 0.0f) {
            ptv_ai_add_tag(w, e, "ai.fire_burst");
            s.t.fire_cooldown = 1.2f; // short bursts cadence
        }
    } else {
        ptv_ai_remove_tag(w, e, "ai.fire_burst");
    }
}

} // namespace ptv
