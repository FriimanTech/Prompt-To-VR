#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"
#include <nlohmann/json.hpp>
#include <string>
#include "heavy_shared.h"

namespace ptv {

// Fire-and-forget tags to describe turret intent
inline void spawn_turret(World& w, std::uint32_t hand_e) {
    // Spawn autonomous emitter with target prioritization; drained by ring system
    broadcast_spawn(w, hand_e, "turret", "construct_turret");
    // Gentle kick haptics on both hands (heavy form)
    publish_haptics(w, hand_e, "construct_form_heavy", "both", 1.0f, 0.22f);
}

} // namespace ptv
