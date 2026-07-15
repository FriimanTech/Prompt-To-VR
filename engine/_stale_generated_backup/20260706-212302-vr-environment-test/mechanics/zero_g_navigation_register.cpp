#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"
#include <nlohmann/json.hpp>
#include <string>
#include "zero_g_navigation_shared.h"

namespace ptv {

// ZeroGNavigation is defined in zero_g_navigation_shared.h so both this
// registration TU and zero_g_navigation_simulation.cpp (which defines
// on_update) share one class definition.
PTV_REGISTER_MECHANIC("generated.zero_g_navigation", ZeroGNavigation)

} // namespace ptv
