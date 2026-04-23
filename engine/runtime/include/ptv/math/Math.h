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

}  // namespace ptv
