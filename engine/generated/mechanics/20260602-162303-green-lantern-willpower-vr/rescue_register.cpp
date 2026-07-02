#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"
#include <nlohmann/json.hpp>
#include <string>

#include "rescue_shared.h"

namespace ptv {
// All methods are defined in rescue_runtime.cpp
}

PTV_REGISTER_MECHANIC("generated.civilian_rescue_interactions", ptv::CivilianRescueInteractions)
