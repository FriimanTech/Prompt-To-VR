#pragma once

// The engine's single definition of "which way is this facing".
//
// Forward is -Z (glTF / OpenXR), up is +Y, and yaw is a rotation about +Y.
// A rotation by ``yaw`` turns -Z into ``(-sin yaw, 0, -cos yaw)``, so the yaw
// of a forward vector f is ``atan2(-f.x, -f.z)``.
//
// This header exists because that formula was written out by hand in several
// places and two of them got the sign of x wrong (``atan2(f.x, -f.z)``), which
// mirrors every heading about the Z axis: travel along +X turned the body to
// face -X. A character chasing straight down -Z looked right, and everything
// else was off by up to 180 degrees. Use these instead of writing atan2.

#include <cmath>

#include "ptv/math/Math.h"

namespace ptv {

/** Yaw (radians, about +Y) of a forward direction. Only x and z are read. */
inline float yaw_from_forward(const Vec3& forward) {
    return std::atan2(-forward.x, -forward.z);
}

/** Unit horizontal forward for a yaw. Inverse of ``yaw_from_forward``. */
inline Vec3 forward_from_yaw(float yaw) {
    return Vec3(-std::sin(yaw), 0.0f, -std::cos(yaw));
}

/** Rotation about +Y by ``yaw``. */
inline Quat yaw_rotation(float yaw) {
    return glm::angleAxis(yaw, Vec3(0.0f, 1.0f, 0.0f));
}

/** Rotation that faces ``forward`` (horizontal part only). */
inline Quat rotation_facing(const Vec3& forward) {
    return yaw_rotation(yaw_from_forward(forward));
}

/** Horizontal forward of a rotation: where its -Z points, flattened. */
inline Vec3 facing_of(const Quat& rotation) {
    Vec3 f = rotation * Vec3(0.0f, 0.0f, -1.0f);
    f.y = 0.0f;
    const float len = std::sqrt(f.x * f.x + f.z * f.z);
    return len > 1.0e-6f ? f / len : Vec3(0.0f, 0.0f, -1.0f);
}

/** Wrap an angle into (-pi, pi]. */
inline float wrap_angle(float a) {
    constexpr float kPi = 3.14159265358979323846f;
    a = std::fmod(a + kPi, 2.0f * kPi);
    if (a < 0.0f) a += 2.0f * kPi;
    return a - kPi;
}

/** Unsigned angle (radians, 0..pi) between two horizontal directions. */
inline float heading_error(const Vec3& a, const Vec3& b) {
    return std::abs(wrap_angle(yaw_from_forward(a) - yaw_from_forward(b)));
}

}  // namespace ptv
