#pragma once

// How a held item sits in one or two hands, from its socket_generation
// sockets and hold profile (``CHoldProfile``). Pure maths, so it is testable
// without an Application; ``GrabMechanic`` decides WHICH solve applies.
//
// Hand sockets (hand_grip, support_grip, support_wrap, hand_strap) are stored
// in the OpenXR GRIP frame of the hand holding that part (docs/sockets.md):
// origin on the gripped part's axis at palm height, -Z little finger ->
// thumb, +X the palm normal (into the palm for the right hand). So:
//
//   * hand-frame hold (swords, tools): the item is placed so its socket IS
//     the grip pose: ``item = grip * inverse(socket)``;
//   * aim-aligned hold (guns, launchers, wands): the palm socket still sits
//     at the grip origin, but the item turns about it so its forward / up
//     follow the controller's AIM pose -- the Touch handle is angled, so the
//     grip frame would point a barrel at the floor;
//   * line hold (rifle grip + handguard, two-handed sword, spear): the item
//     vector between its two hand sockets is laid along the line between the
//     two hands, rolled so its up stays as close as possible to the primary
//     hand's;
//   * forearm hold (strapped shield): the line from the hand strap to the
//     forearm strap is laid along the forearm, the front face toward the
//     back of the forearm.
//
// A socket with mirror rule ``flip_z180`` (both palms approach from the same
// side, e.g. palm-up under a handguard) is turned half a turn about its Z for
// the LEFT hand; ``same`` sockets (handles) serve both hands unchanged.

#include "GripHold.h"
#include "ptv/math/Math.h"

#include <glm/gtc/quaternion.hpp>

namespace ptv::grab {

/** Rotation whose +Z is ``z`` and whose +Y is as close to ``y_hint`` as
 *  possible (right-handed). */
inline Quat quat_from_zy(const Vec3& z_in, const Vec3& y_hint) {
    Vec3 z = glm::normalize(z_in);
    Vec3 y = y_hint - z * glm::dot(y_hint, z);
    if (glm::length(y) < 1e-5f) {
        const Vec3 alt = std::fabs(z.y) < 0.9f ? Vec3(0, 1, 0) : Vec3(1, 0, 0);
        y = alt - z * glm::dot(alt, z);
    }
    y = glm::normalize(y);
    const Vec3 x = glm::cross(y, z);
    return glm::normalize(glm::quat_cast(glm::mat3(x, y, z)));
}

/** A hand socket's rotation for ``left`` (hand 0), applying the mirror rule. */
inline Quat hand_socket_rotation(const Quat& socket_rot, bool left, bool flip_z180) {
    if (!left || !flip_z180) return socket_rot;
    return glm::normalize(socket_rot *
                          glm::angleAxis(3.14159265358979323846f, Vec3(0, 0, 1)));
}

/** Hand-frame hold terms for ``obj = hand_pos + hand_rot * offset`` and
 *  ``obj_rot = hand_rot * rotation``: the socket coincides with the grip. */
inline void solve_hand_frame_hold(const Vec3& socket_pos, const Quat& socket_rot,
                                  float entity_scale, Vec3* out_offset,
                                  Quat* out_rotation) {
    const Quat rot = glm::normalize(glm::inverse(socket_rot));
    *out_rotation = rot;
    *out_offset = -(rot * (socket_pos * entity_scale));
}

/** Aim-aligned hold: the palm socket at the grip origin, the item's
 *  ``forward`` (mesh space, where shots go) along the aim ray and its ``up``
 *  along the aim up. ``grip_to_aim`` is the aim pose in the grip frame. */
inline void solve_aim_hold(const Vec3& palm_pos, const Vec3& item_forward,
                           const Vec3& item_up, float entity_scale,
                           const Quat& grip_to_aim, const Quat& extra,
                           Vec3* out_offset, Quat* out_rotation) {
    const Quat item_frame = quat_from_zy(item_forward, item_up);
    solve_grip_hold(palm_pos, item_frame, entity_scale, grip_to_aim, extra,
                    out_offset, out_rotation);
}

/** World rotation laying item vector ``item_dir`` along ``world_dir`` with the
 *  item's ``item_up`` turned as close to ``world_up`` as the line allows. */
inline Quat line_rotation(const Vec3& item_dir, const Vec3& item_up,
                          const Vec3& world_dir, const Vec3& world_up) {
    const Quat item_frame = quat_from_zy(item_dir, item_up);
    const Quat world_frame = quat_from_zy(world_dir, world_up);
    return glm::normalize(world_frame * glm::inverse(item_frame));
}

/** World position that puts mesh-space point ``local`` (at ``entity_scale``)
 *  on ``world_point`` under ``rotation``. */
inline Vec3 place_point(const Vec3& local, float entity_scale, const Quat& rotation,
                        const Vec3& world_point) {
    return world_point - rotation * (local * entity_scale);
}

/** The back of the hand, in world space: grip +X is into the palm for the
 *  right hand and away from it for the left. */
inline Vec3 dorsal_direction(const Quat& grip_rot, bool left) {
    const Vec3 x = grip_rot * Vec3(1.0f, 0.0f, 0.0f);
    return left ? x : -x;
}

}  // namespace ptv::grab
