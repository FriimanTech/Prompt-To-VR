#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"
#include <nlohmann/json.hpp>
#include <entt/entt.hpp>
#include <string>

// Intentionally empty: Laser logic is implemented inside the registered class in hazards_register.cpp
// This file exists to keep future expansion isolated if per-hazard classes are split later.
