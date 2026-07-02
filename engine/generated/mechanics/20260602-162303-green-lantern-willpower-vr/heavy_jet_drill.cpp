#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"
#include <nlohmann/json.hpp>
#include <string>
#include "heavy_shared.h"

namespace ptv {

inline void spawn_jet_drill(World& w, std::uint32_t hand_e) {
    // Sustained forward cone; rooted at spawn
    broadcast_spawn(w, hand_e, "jet_drill", "construct_energy_jet_drill");
    // Armor break emphasis
    publish_armor_break(w, hand_e, 1.75f);
    // Continuous hum haptics cue
    publish_haptics(w, hand_e, "hover_light", "right", 0.6f, 0.35f);
}

} // namespace ptv
