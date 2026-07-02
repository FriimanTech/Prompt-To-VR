#include "ptv/ecs/World.h"
#include <entt/entt.hpp>
#include <string>

namespace ptv {

// Emits a tag that a generic spawner (builtin or game-specific) could consume.
// We keep it lightweight and idempotent-ish via a short TTL concept embedded in tag.
void maybe_emit_practice_spawn(World& world, std::uint32_t self, int max_alive, float& t_since_spawn, float cooldown_s) {
    t_since_spawn += world.delta_time();
    if (t_since_spawn < cooldown_s) return;

    // Count existing practice drones by scanning for CTag entries that mark them.
    int alive = 0;
    auto& reg = world.registry();
    auto view = reg.view<CTag>();
    for (auto e : view) {
        auto& tags = reg.get<CTag>(e).tags;
        for (auto& s : tags) {
            if (s.rfind("practice.drone:", 0) == 0) { alive++; break; }
        }
    }
    if (alive >= max_alive) return;

    // Publish a request to spawn one more drone target in corridor.
    // Consumers: a generic spawner can watch for this prefix and create an enemy dummy.
    if (world.valid(self)) {
        if (!world.has<CTag>(self)) world.emplace_or_replace<CTag>(self, CTag{});
        auto& tags = world.get<CTag>(self).tags;
        // Minimal payload: type id and a short suggested TTL for the consumer
        tags.push_back("spawner.request:{\"profile\":\"practice_drone\",\"ttl_s\":30.0}");
        t_since_spawn = 0.0f;
    }
}

} // namespace ptv
