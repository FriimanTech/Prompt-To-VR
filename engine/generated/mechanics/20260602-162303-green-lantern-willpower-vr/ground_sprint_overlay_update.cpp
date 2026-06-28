#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <optional>
#include <algorithm>
#include <cstdint>
#include <cmath>

// This file intentionally left minimal; the main implementation is in ground_sprint_overlay_register.cpp
// Splitting keeps compile units small and aligns with suggested split in the task.
// Additional per-scene tuning hooks or debug draws could live here in future.

namespace ptv {
// No additional symbols for now.
}
