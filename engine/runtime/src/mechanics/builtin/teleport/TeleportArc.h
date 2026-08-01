#pragma once

// Parabolic teleport-arc solver, split out from the mechanic so it can be
// tested and re-used (a "teleport but in slow motion" variant only needs to
// change the launch speed it passes in).

#include <vector>

#include "ptv/api/MechanicContext.h"
#include "ptv/math/Math.h"

namespace ptv {
namespace teleport {

struct ArcParams {
    float launch_speed_mps{6.0f};
    float gravity_mps2{9.8f};
    float floor_y{0.0f};
    float max_horizontal_m{12.0f};
    /** Surfaces steeper than this are walls, not landing spots.
     *  ``normal.y`` below the cutoff is rejected. */
    float min_ground_normal_y{0.4f};
};

struct ArcResult {
    /** Sampled world-space points, for drawing. Always populated, even when
     *  the arc found nowhere valid to land (so the player still sees where
     *  they are pointing). */
    std::vector<Vec3> points;
    Vec3 target{0.0f, 0.0f, 0.0f};
    bool valid{false};
};

/**
 * Trace a projectile from ``origin`` along ``forward`` until it hits
 * geometry or the floor plane.
 *
 * Prefers a real physics hit so the player can land on generated platforms
 * and terrain at any height, and falls back to the flat ``floor_y`` plane
 * when nothing is in the way.
 */
void solve_arc(const Vec3& origin, const Vec3& forward, const ArcParams& p,
               IPhysicsAccess* physics, ArcResult& out);

}  // namespace teleport
}  // namespace ptv
