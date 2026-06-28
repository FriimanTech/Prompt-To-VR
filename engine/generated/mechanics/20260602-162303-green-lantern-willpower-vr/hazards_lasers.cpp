#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"
#include <nlohmann/json.hpp>
#include <entt/entt.hpp>
#include <string>
#include <cmath>

// This translation unit intentionally empty of additional symbols.
// Laser logic lives in hazards_register.cpp (tick_laser). We keep this
// file to match the suggested split for clarity/maintenance.
