#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace ptv {

using Vec2 = glm::vec2;
using Vec3 = glm::vec3;
using Vec4 = glm::vec4;
using Quat = glm::quat;
using Mat3 = glm::mat3;
using Mat4 = glm::mat4;

struct Transform {
    Vec3 position{0.0f};
    Quat rotation{1.0f, 0.0f, 0.0f, 0.0f};  // w, x, y, z
    float scale{1.0f};

    Mat4 matrix() const {
        Mat4 t = glm::translate(Mat4(1.0f), position);
        Mat4 r = glm::mat4_cast(rotation);
        Mat4 s = glm::scale(Mat4(1.0f), Vec3(scale));
        return t * r * s;
    }
};

/** Position and orientation of ``m`` with its scale (and shear) removed.
 *
 *  A joint's matrix carries the rig's own unit scale -- Uthana's root joint
 *  is scaled 0.01 because the rig is authored in centimetres -- and an
 *  offset composed through it would shrink a hundredfold. */
inline Mat4 rigid_part(const Mat4& m) {
    Vec3 x(m[0]), y(m[1]);
    const float lx = glm::length(x);
    if (lx < 1e-8f) return glm::translate(Mat4(1.0f), Vec3(m[3]));
    x /= lx;
    y -= x * glm::dot(x, y);
    const float ly = glm::length(y);
    if (ly < 1e-8f) return glm::translate(Mat4(1.0f), Vec3(m[3]));
    y /= ly;
    Mat4 out(1.0f);
    out[0] = Vec4(x, 0.0f);
    out[1] = Vec4(y, 0.0f);
    out[2] = Vec4(glm::cross(x, y), 0.0f);
    out[3] = m[3];
    return out;
}

}  // namespace ptv
