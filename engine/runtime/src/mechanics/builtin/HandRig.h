#pragma once

// ---------------------------------------------------------------------------
// HandRig -- measure a rigged hand once, so any rig can be posed correctly.
//
// Posing a finger means rotating each of its joints about the axis that
// closes it into the palm. That axis is NOT a constant: it depends on how the
// rig's author oriented every joint, which differs between Mixamo, Unity,
// Blender exports, Tripo and every hand-authored model. A hard-coded "curl
// about local X" only works for the one rig it was written against and bends
// every other rig's fingers sideways or backwards.
//
// So the axes are derived from the rig's own rest pose instead:
//
//   * the fingers are found by BodySlots names first and, when a rig uses
//     names nobody predicted, by topology (five chains hanging off the
//     wrist; the thumb is the one rooted closest to it);
//   * the palm normal comes from the wrist and the index/pinky knuckles,
//     with the handedness deciding its sign;
//   * each joint's curl axis is ``cross(bone_direction, palm_normal)``,
//     expressed in that joint's own local frame, which rotates the bone
//     toward the palm by construction;
//   * the wrist-to-grip alignment maps the hand's anatomical frame onto the
//     OpenXR grip frame (spec: +X normal to the palm -- away from it on the
//     left hand, into it on the right; -Z along the tube the closed fingers
//     make, little finger toward thumb).
//
// Nothing here writes to the rig. Callers apply curls with
// ``ISkeleton::set_joint_rotation_from_bind`` so the rest rotation is kept.
// ---------------------------------------------------------------------------

#include <array>
#include <cstdint>

#include "ptv/api/MechanicContext.h"

namespace ptv::hand_rig {

inline constexpr int kFingers = 5;         // thumb, index, middle, ring, pinky
inline constexpr int kSegments = 3;        // proximal, intermediate, distal

enum Finger : int { Thumb = 0, Index = 1, Middle = 2, Ring = 3, Pinky = 4 };

struct Segment {
    int joint{-1};
    /** Unit axis in the joint's LOCAL bind frame that curls it toward the
     *  palm with a positive angle. */
    Vec3 curl_axis_local{1.0f, 0.0f, 0.0f};
};

struct FingerChain {
    std::array<Segment, kSegments> segments{};
    bool present() const { return segments[0].joint >= 0; }
};

struct Rig {
    bool valid{false};
    int hand{1};  ///< 0 = left, 1 = right
    int wrist{-1};
    std::array<FingerChain, kFingers> fingers{};
    /** For the thumb only: local axis that swings it toward the index
     *  finger's base, which is the "thumb resting on the stick" pose. */
    Vec3 thumb_adduct_axis_local{0.0f, 1.0f, 0.0f};

    /** Rest-pose hand frame in the entity's space: origin at the palm
     *  centre, columns laid out like the OpenXR grip frame. */
    Mat4 hand_frame_model{1.0f};
    /** Wrist joint's bind pose in the entity's space. */
    Mat4 wrist_bind_model{1.0f};

    /** Transform taking the OpenXR GRIP frame to the ENTITY frame, for a rig
     *  that IS a hand (the whole entity follows the controller):
     *  ``entity_world = grip_world * grip_to_entity``. */
    Mat4 grip_to_entity{1.0f};

    /** Wrist world rotation that puts this hand in the grip frame, for a
     *  rig where the hand is one joint of a body:
     *  ``wrist_world_rot = grip_world_rot * grip_to_wrist_rot``. */
    Quat grip_to_wrist_rot{1.0f, 0.0f, 0.0f, 0.0f};
    /** Palm centre in the wrist joint's local frame (metres). */
    Vec3 palm_offset_wrist_local{0.0f};

    /** How many finger joints were resolved. */
    int joint_count() const;
};

/** How the fingers were found, for logging. */
enum class Resolution { None, Names, Topology };

/** Measure one hand on ``entity``'s rig. ``hand`` is 0 = left, 1 = right.
 *
 *  Returns an invalid rig when the skeleton has no recognisable hand for
 *  that side -- a Tripo body whose arm ends in one rigid wrist bone, say.
 *  That is a legitimate answer, not an error. */
Rig analyze(const ISkeleton& skeleton, std::uint32_t entity, int hand,
            Resolution* how = nullptr);

}  // namespace ptv::hand_rig
