#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <glm/glm.hpp>
#include <entt/entt.hpp>
#include <algorithm>
#include <cmath>
#include <string>

#include "ground_sprint_collision_guard_shared.h"

// This TU intentionally contains only helper usage demonstrations if needed in the future.
// The actual mechanic is implemented in ground_sprint.cpp. Keeping this file to match the
// suggested split and to host any future guard-specific utilities (none required yet).

namespace ptv {

// Example utility (currently unused by runtime but available for future tuning via linkage).
static glm::vec3 clamp_horizontal(const glm::vec3& v) {
    return glm::vec3(v.x, 0.0f, v.z);
}

} // namespace ptv
