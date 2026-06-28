#include "ptv/math/Math.h"
#include <glm/glm.hpp>
#include <cmath>

namespace traversal_helpers {

// Build a yaw-only quaternion that faces the horizontal projection of fwd
ptv::Quat yaw_facing_from_forward(const ptv::Vec3& fwd_ws) {
    glm::vec3 f(fwd_ws.x, fwd_ws.y, fwd_ws.z);
    if (!std::isfinite(f.x) || !std::isfinite(f.y) || !std::isfinite(f.z) || glm::length2(f) < 1e-8f)
        f = glm::vec3(0,0,1);
    glm::vec3 h = glm::normalize(glm::vec3(f.x, 0.0f, f.z));
    float yaw = std::atan2(h.x, h.z); // note: z-forward; adjust if engine uses different convention
    float half = 0.5f * yaw;
    float c = std::cos(half), s = std::sin(half);
    // Yaw around +Y: w, x, y, z
    return ptv::Quat{ c, 0.0f, s, 0.0f };
}

} // namespace traversal_helpers
