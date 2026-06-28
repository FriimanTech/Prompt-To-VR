#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"
#include <nlohmann/json.hpp>
#include <entt/entt.hpp>

// Yellow zone suppression logic implemented in EnvironmentalHazards::tick_yellow.
// HUD marker tags set on player: "yellow_suppression_active", "ring_drain_x1.5", "control_tether_unstable".
