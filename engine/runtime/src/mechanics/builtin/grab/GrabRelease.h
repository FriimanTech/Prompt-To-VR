#pragma once

// The hand-off between grab and throw.
//
// Grab owns everything about holding an object: what is held, the hold
// offset, the kinematic follow, and letting go. Throw is a separate
// per-entity mechanic that only decides how hard a released object flies.
// Keeping them split matters because scenes bind them independently -- the
// starter room has a cube that is grabbable but NOT throwable -- so releasing
// must work with no throw mechanic present at all.
//
// Grab publishes a release here; a throw mechanic bound to that entity picks
// it up in the same Input phase and applies the impulse before the physics
// step. The ``serial`` makes the handshake order-independent: if a throw
// instance happens to tick before grab on the frame of the release, it simply
// sees the new serial on the following frame instead of missing the event.

#include <cstdint>

#include "ptv/math/Math.h"

namespace ptv {
namespace grab {

struct ReleaseEvent {
    std::uint32_t entity{0xFFFFFFFFu};
    /** Smoothed world velocity of the releasing hand, m/s. */
    Vec3 hand_velocity{0.0f, 0.0f, 0.0f};
    /** Increments on every publish; 0 means "nothing has been released". */
    std::uint64_t serial{0};
};

/** Most recent release, or a serial of 0 if there has not been one. */
const ReleaseEvent& last_release();

/** Called by the grab mechanic when a hand lets go of ``entity``. */
void publish_release(std::uint32_t entity, const Vec3& hand_velocity);

}  // namespace grab
}  // namespace ptv
