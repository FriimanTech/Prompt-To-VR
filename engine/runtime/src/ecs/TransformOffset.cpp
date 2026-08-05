#include "ptv/ecs/TransformOffset.h"

#include <algorithm>
#include <cmath>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace ptv {
namespace {

// Below these thresholds an offset is visually indistinguishable from rest,
// so we snap it onto the target and stop ticking it. 0.1 mm and ~0.06 degrees.
constexpr float kSettlePositionEpsilon = 1e-4f;
constexpr float kSettleRotationEpsilon = 1e-3f;

}  // namespace

void tick_transform_offsets(World& world, float dt) {
    if (dt <= 0.0f) return;

    world.registry().view<CTransformOffset>().each(
        [dt](CTransformOffset& off) {
            if (!off.active) return;

            const float tau = std::max(1e-3f, off.recover_tau_s);
            const float k = 1.0f - std::exp(-dt / tau);

            off.position += (off.target_position - off.position) * k;
            // slerp needs same-hemisphere inputs or it takes the long way
            // around; glm::slerp does not normalise the sign for us.
            Quat target = off.target_rotation;
            if (glm::dot(off.rotation, target) < 0.0f) {
                target = -target;
            }
            off.rotation = glm::normalize(glm::slerp(off.rotation, target, k));

            const bool position_settled =
                glm::length(off.target_position - off.position) <
                kSettlePositionEpsilon;
            // 1 - |dot| is a cheap angular distance that avoids an acos.
            const bool rotation_settled =
                (1.0f - std::fabs(glm::dot(off.rotation, off.target_rotation))) <
                kSettleRotationEpsilon;

            if (position_settled && rotation_settled) {
                off.position = off.target_position;
                off.rotation = off.target_rotation;
                off.active = false;
            }
        });
}

Mat4 entity_visual_matrix(const World& world, std::uint32_t entity,
                          const Transform& t) {
    const Mat4 base = t.matrix();
    if (!world.has<CTransformOffset>(entity)) return base;

    const auto& off = world.get<CTransformOffset>(entity);
    if (!off.active) return base;

    // Local-space offset: applied after the entity's own transform so a kick
    // is expressed along the object's own axes (back along its barrel, not
    // along world -Z) regardless of how the player is holding it.
    const Mat4 local = glm::translate(Mat4(1.0f), off.position) *
                       glm::mat4_cast(off.rotation);
    return base * local;
}

}  // namespace ptv
