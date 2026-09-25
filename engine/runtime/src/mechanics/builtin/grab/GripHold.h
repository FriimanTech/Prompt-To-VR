#pragma once

// Where a grabbed object sits in the hand, from its ``grip`` socket.
//
// The convention, and why it needs converting:
//
//   * A socket (asset_contract / glb_sockets) faces along its +Z: for a
//     weapon's grip, +Z points down the barrel toward the muzzle, and +Y is
//     the weapon's up (sights on top, magazine and handle below).
//   * The OpenXR GRIP pose looks down -Z, along the controller's angled
//     handle; the AIM pose looks down -Z along the controller body -- where
//     the player is pointing.
//
// Mapping the socket frame straight onto the grip frame -- the old solve --
// therefore pointed the barrel BACKWARD along the handle, tilted by the
// handle's angle, which is the "gun is upside down / sideways in my hand"
// bug. The hold now maps socket +Z onto where the controller AIMS and socket
// +Y onto the controller's up, with the grip socket sitting at the grip
// pose's position, so a gun held with the controller upright points forward
// and sits upright.
//
// Pure maths so it is testable without an Application.

#include "ptv/math/Math.h"

#include <glm/gtc/quaternion.hpp>

namespace ptv::grab {

/** Rotation from socket convention (+Z forward) to OpenXR (-Z forward):
 *  half a turn about Y, which also keeps +Y up. */
inline Quat socket_to_openxr() {
    return glm::angleAxis(3.14159265358979323846f, Vec3(0.0f, 1.0f, 0.0f));
}

/** Hold terms for ``obj_pos = hand_pos + hand_rot * offset`` and
 *  ``obj_rot = hand_rot * rotation``.
 *
 *  ``socket_pos`` / ``socket_rot``: the grip socket in the object's frame
 *  (metres at authored scale); ``entity_scale``: the object's CTransform
 *  scale; ``grip_to_aim``: the controller's aim pose expressed in its grip
 *  frame (identity if unknown); ``extra``: a per-asset correction, applied in
 *  the socket's own frame. */
inline void solve_grip_hold(const Vec3& socket_pos, const Quat& socket_rot,
                            float entity_scale, const Quat& grip_to_aim,
                            const Quat& extra, Vec3* out_offset,
                            Quat* out_rotation) {
    const Quat hold_frame =
        glm::normalize(grip_to_aim * socket_to_openxr() * extra);
    const Quat rot = glm::normalize(hold_frame * glm::inverse(socket_rot));
    *out_rotation = rot;
    *out_offset = -(rot * (socket_pos * entity_scale));
}

}  // namespace ptv::grab
