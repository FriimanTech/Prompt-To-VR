#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"
#include <nlohmann/json.hpp>
#include <entt/entt.hpp>

// Vent logic implemented in EnvironmentalHazards::tick_vent (see hazards_register.cpp).
// This TU exists to respect the suggested split and ease future expansion.
