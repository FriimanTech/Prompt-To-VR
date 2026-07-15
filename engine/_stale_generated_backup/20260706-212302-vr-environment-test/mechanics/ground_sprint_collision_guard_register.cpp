#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <string>

// Registration is emitted in ground_sprint.cpp with the class definition to satisfy
// the single-macro-per-mechanic rule. This file intentionally left minimal to satisfy
// the planner's suggested file naming without duplicating registration.

// Note: Do not add another PTV_REGISTER_MECHANIC here.
