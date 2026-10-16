#pragma once

// How far a locomotion clip has carried the avatar's neck away from where it
// stands in the bind pose, smoothed.
//
// builtin.avatar_body places the body so the BIND-pose eye point sits under
// the headset. Clips move the upper body -- an idle can settle the neck 10 cm
// forward, a run leans it -- so without this the camera ends up behind the
// avatar's own shoulders. Horizontal only: the eye height belongs to the
// player's real head.

#include <algorithm>
#include <cmath>

#include "ptv/math/Math.h"

namespace ptv::avatar_body {

/** Furthest the body follows the clip (metres, model space), and how quickly
 *  the correction follows it. */
constexpr float kMaxNeckDrift = 0.15f;
constexpr float kNeckDriftTau = 0.15f;

class NeckDrift {
public:
    /** ``animated`` / ``bind``: the neck in model space this pose and in the
     *  bind pose. Returns the smoothed, clamped horizontal drift. */
    Vec3 update(const Vec3& animated, const Vec3& bind, float dt) {
        Vec3 d = animated - bind;
        d.y = 0.0f;
        if (!std::isfinite(d.x) || !std::isfinite(d.z)) d = Vec3(0.0f);
        const float len = glm::length(d);
        if (len > kMaxNeckDrift) d *= kMaxNeckDrift / len;
        value_ += (d - value_) *
                  (1.0f - std::exp(-std::max(0.0f, dt) / kNeckDriftTau));
        return value_;
    }

    const Vec3& value() const { return value_; }
    void reset() { value_ = Vec3(0.0f); }

private:
    Vec3 value_{0.0f};
};

}  // namespace ptv::avatar_body
