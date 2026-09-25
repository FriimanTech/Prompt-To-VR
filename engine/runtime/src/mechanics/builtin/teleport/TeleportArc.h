#pragma once

// Parabolic teleport-arc solver, split out from the mechanic so it can be
// tested and re-used (a "teleport but in slow motion" variant only needs to
// change the launch speed it passes in).

#include <cstdint>
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
    /** Bodies the arc passes through: the player's own capsule and whatever
     *  is in their hands. An arc that starts inside the pistol the player is
     *  holding must not "land" on it. */
    std::vector<std::uint32_t> ignore_entities;
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

/** How far the arc should reach for a given controller pitch.
 *
 *  Monotonic by construction: tilting the controller up ALWAYS reaches
 *  farther and tilting it down always lands closer. A plain fixed-speed
 *  projectile does not have that property -- its range peaks at 45 degrees
 *  and falls again above it, so raising the controller past that point
 *  pulls the landing spot back toward the player, which reads as broken. */
float distance_for_pitch(float pitch_rad, float min_distance_m,
                         float max_distance_m);

/** Clamp ``distance_m`` to what an arc fired at ``pitch_rad`` from
 *  ``height_m`` can reach. Aiming down, a projectile can land no further
 *  than roughly where the aim line meets the ground; asking for more has no
 *  ballistic solution. Monotonic in pitch, like ``distance_for_pitch``. */
float reachable_distance(float pitch_rad, float distance_m, float height_m);

/** Launch speed that lands a projectile fired at ``pitch_rad`` from
 *  ``height_m`` above the landing plane at ``distance_m`` horizontally.
 *  Returns a small positive speed when the geometry has no ballistic
 *  solution (aiming steeply down at a point behind the drop). */
float launch_speed_for(float pitch_rad, float distance_m, float height_m,
                       float gravity_mps2);

/** Pitch range the aim is clamped to, radians. */
inline constexpr float kMinAimPitch = -1.047f;  // -60 degrees
inline constexpr float kMaxAimPitch = 0.611f;   // +35 degrees

}  // namespace teleport
}  // namespace ptv
