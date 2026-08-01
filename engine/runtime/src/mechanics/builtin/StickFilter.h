#pragma once

// Thumbstick conditioning shared by every stick-driven built-in.
//
// Raw OpenXR thumbstick values are noisy and, on well-used Touch Plus
// controllers, have noticeable cross-axis bleed. Every locomotion mechanic
// needs the same treatment, and they must agree: teleport's aim threshold and
// slide's speed ramp are both expressed in terms of the FILTERED magnitude,
// so if one of them skipped the filter its thresholds would silently mean
// something different.

#include <algorithm>
#include <cmath>

#include "ptv/math/Math.h"

namespace ptv {
namespace stick {

// RADIAL dead zone, not per-axis. A per-axis dead zone leaves a square dead
// region, which lets a near-rest diagonal push produce pure strafe -- exactly
// the comfort problem Meta calls out. 0.18 kills drift on Quest 3 Touch Plus
// without eating intentional motion.
constexpr float kDeadRadius = 0.18f;
// Cardinal snap: when one axis clearly dominates and the other is small, zero
// the small one. "Push pure right" then feels like pure right instead of a
// slow diagonal. Intentional diagonals survive because BOTH axes have to be
// small for the snap to fire.
constexpr float kAxisDominant = 0.50f;
constexpr float kAxisCrossSnap = 0.18f;

struct Filtered {
    Vec2 axes{0.0f, 0.0f};
    /** Magnitude AFTER the dead zone and snap, clamped to 1. Thresholds in
     *  mechanics are all expressed against this. */
    float magnitude{0.0f};
};

/** Apply the radial dead zone and cardinal snap to a raw thumbstick value. */
inline Filtered filter(const Vec2& raw) {
    Filtered out;
    const float raw_mag = std::sqrt(raw.x * raw.x + raw.y * raw.y);
    if (raw_mag <= kDeadRadius) return out;

    // Re-scale the surviving range to [0,1] so travel stays linear past the
    // dead zone. Matches Unity XR Toolkit / Meta's first-party samples.
    const float scaled = (raw_mag - kDeadRadius) / (1.0f - kDeadRadius);
    const float s = std::min(scaled, 1.0f) / raw_mag;
    float x = raw.x * s;
    float y = raw.y * s;

    if (std::abs(x) > kAxisDominant && std::abs(y) < kAxisCrossSnap) {
        y = 0.0f;
    } else if (std::abs(y) > kAxisDominant && std::abs(x) < kAxisCrossSnap) {
        x = 0.0f;
    }

    out.axes = Vec2(x, y);
    // Recompute after the snap so speed ramps use the corrected length.
    out.magnitude = std::min(std::sqrt(x * x + y * y), 1.0f);
    return out;
}

}  // namespace stick
}  // namespace ptv
