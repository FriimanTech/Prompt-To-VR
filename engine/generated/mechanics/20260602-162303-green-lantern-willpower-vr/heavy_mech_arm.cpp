#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"
#include <nlohmann/json.hpp>
#include <string>
#include "heavy_shared.h"

namespace ptv {

inline void spawn_mech_arm(World& w, std::uint32_t hand_e) {
    // Oversized arm that will perform sweeping punch / ground pound via downstream systems
    broadcast_spawn(w, hand_e, "mech_arm", "construct_mech_arm");
    // Shockwave VFX hint
    nlohmann::json j{{"asset","construct_energy_vfx_core"},{"mode","shockwave"},{"radius_m",3.0f}};
    add_tag(w, hand_e, std::string("vfx.spawn:") + j.dump());
    // Heavy impact haptics
    publish_haptics(w, hand_e, "block_impact_strong", "both", 1.0f, 0.18f);
}

} // namespace ptv
