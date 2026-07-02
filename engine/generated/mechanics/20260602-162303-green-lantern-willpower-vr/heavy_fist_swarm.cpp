#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"
#include <nlohmann/json.hpp>
#include <string>
#include "heavy_shared.h"

namespace ptv {

inline void spawn_fist_swarm(World& w, std::uint32_t hand_e) {
    // Orbiting fists that dash to marked targets
    broadcast_spawn(w, hand_e, "fist_swarm", "construct_fist_swarm");
    // Swarm trail VFX ping
    nlohmann::json j{{"asset","construct_energy_vfx_core"},{"mode","swarm_burst"},{"count",6}};
    add_tag(w, hand_e, std::string("vfx.spawn:") + j.dump());
    publish_haptics(w, hand_e, "bolt_fire_sharp", "right", 0.9f, 0.12f);
}

} // namespace ptv
