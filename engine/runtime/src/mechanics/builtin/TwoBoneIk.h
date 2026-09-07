#pragma once

// ---------------------------------------------------------------------------
// Two-bone analytic IK -- shoulder -> elbow -> wrist, solved with the law of
// cosines.
//
// This is what lets an avatar's arm follow a controller. The alternative,
// an iterative solver (FABRIK, CCD), costs more CPU for no benefit on a
// two-bone chain where a closed-form answer exists.
//
// Three rules come from shipped VR avatar work rather than from the maths,
// and all three matter more than the solve itself:
//
//   1. A POLE HINT is mandatory. Two known points (shoulder and wrist) leave
//      the elbow free on a circle, so without a preferred direction the elbow
//      can flip behind the arm. The hint is a bend-plane preference, biased
//      down and back from the torso.
//   2. NEVER distance-clamp the wrist. If the player reaches beyond the
//      avatar's arm length the hand stays at the controller and the chain
//      simply straightens. Pulling the hand back to arm's length is far more
//      disturbing than a slightly-too-short arm, because the virtual hand
//      stops agreeing with the real one.
//   3. CLAMP MAXIMUM EXTENSION just short of straight. At exactly 180 degrees
//      the bend plane is undefined, so an arm held straight out jitters
//      between solutions. Stopping around 170 degrees removes the
//      singularity at a cost nobody can see.
//
// Pure maths on glm types, no engine dependencies, so it is unit-testable
// without a World, a skeleton or a renderer.
// ---------------------------------------------------------------------------

#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace ptv::two_bone_ik {

/** Cosine of the maximum interior elbow angle, i.e. how straight the chain is
 *  allowed to get. cos(170 deg) is about -0.985. */
inline constexpr float kMaxExtensionCos = -0.985f;

struct Solution {
    glm::vec3 elbow{0.0f};
    glm::vec3 wrist{0.0f};
    /** False only for degenerate input (non-finite values, zero-length
     *  bones). The caller should leave the arm alone in that case rather than
     *  writing NaNs into a joint. */
    bool valid{false};
};

/** Solve for the elbow position.
 *
 *  ``shoulder`` is the chain root, ``target`` is where the wrist should be
 *  (the controller, unmodified), ``upper_len`` and ``lower_len`` are the two
 *  bone lengths, and ``pole_hint`` is the direction the elbow prefers to bend
 *  toward. All positions are in one consistent space. */
inline Solution solve(const glm::vec3& shoulder, const glm::vec3& target,
                      float upper_len, float lower_len,
                      const glm::vec3& pole_hint) {
    Solution out{};
    if (!std::isfinite(upper_len) || !std::isfinite(lower_len) ||
        upper_len <= 1.0e-5f || lower_len <= 1.0e-5f) {
        return out;
    }
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(shoulder[i]) || !std::isfinite(target[i])) {
            return out;
        }
    }

    // Rule 2: the wrist is the target, always.
    out.wrist = target;

    glm::vec3 to_target = target - shoulder;
    float reach = glm::length(to_target);
    if (reach < 1.0e-5f) {
        // Target sits on the shoulder. Any bend is as good as any other, so
        // push the elbow along the hint and keep the result finite.
        const glm::vec3 hint = glm::length(pole_hint) > 1.0e-5f
                                   ? glm::normalize(pole_hint)
                                   : glm::vec3(0.0f, -1.0f, 0.0f);
        out.elbow = shoulder + hint * upper_len;
        out.valid = true;
        return out;
    }
    const glm::vec3 axis = to_target / reach;

    const float max_reach = upper_len + lower_len;
    // Rule 3: hold the chain just short of straight even when the target is
    // further away than the arm can stretch. ``effective`` is only used to
    // place the elbow; the wrist above already went to the real target.
    const float straight_limit = max_reach * 0.999f;
    float effective = std::min(reach, straight_limit);

    // Law of cosines: distance from the shoulder to the elbow's projection
    // onto the shoulder->target axis.
    float a = (effective * effective + upper_len * upper_len -
               lower_len * lower_len) /
              (2.0f * effective);
    a = std::clamp(a, -upper_len, upper_len);

    // Enforce the extension clamp as an angle rather than a distance, so a
    // long arm and a short one both stop bending at the same straightness.
    const float cos_elbow =
        (upper_len * upper_len + lower_len * lower_len -
         effective * effective) /
        (2.0f * upper_len * lower_len);
    if (cos_elbow < kMaxExtensionCos) {
        // Recompute the projection at the clamped angle.
        const float clamped_sq = upper_len * upper_len + lower_len * lower_len -
                                 2.0f * upper_len * lower_len *
                                     kMaxExtensionCos;
        const float clamped = std::sqrt(std::max(1.0e-6f, clamped_sq));
        effective = clamped;
        a = (effective * effective + upper_len * upper_len -
             lower_len * lower_len) /
            (2.0f * effective);
        a = std::clamp(a, -upper_len, upper_len);
    }

    const float h_sq = upper_len * upper_len - a * a;
    const float h = std::sqrt(std::max(0.0f, h_sq));

    // Rule 1: build the bend plane from the hint, made perpendicular to the
    // chain axis. A hint parallel to the axis carries no information, so fall
    // back to any stable perpendicular.
    glm::vec3 bend = pole_hint - axis * glm::dot(pole_hint, axis);
    if (glm::length(bend) < 1.0e-4f) {
        glm::vec3 fallback(0.0f, -1.0f, 0.0f);
        bend = fallback - axis * glm::dot(fallback, axis);
        if (glm::length(bend) < 1.0e-4f) {
            fallback = glm::vec3(0.0f, 0.0f, -1.0f);
            bend = fallback - axis * glm::dot(fallback, axis);
        }
    }
    bend = glm::normalize(bend);

    out.elbow = shoulder + axis * a + bend * h;
    out.valid = true;
    return out;
}

/** Rotation that takes ``from`` to ``to``, both unit-ish direction vectors.
 *
 *  Used to turn a solved bone direction into a joint rotation. Handles the
 *  antiparallel case, which is otherwise a divide by zero. */
inline glm::quat rotation_between(const glm::vec3& from, const glm::vec3& to) {
    const float lf = glm::length(from);
    const float lt = glm::length(to);
    if (lf < 1.0e-6f || lt < 1.0e-6f) return glm::quat(1, 0, 0, 0);
    const glm::vec3 a = from / lf;
    const glm::vec3 b = to / lt;
    const float d = glm::dot(a, b);
    if (d > 0.9999f) return glm::quat(1, 0, 0, 0);
    if (d < -0.9999f) {
        // Opposite directions: any perpendicular axis gives a valid half turn.
        glm::vec3 axis = glm::cross(a, glm::vec3(1.0f, 0.0f, 0.0f));
        if (glm::length(axis) < 1.0e-4f) {
            axis = glm::cross(a, glm::vec3(0.0f, 1.0f, 0.0f));
        }
        return glm::angleAxis(3.14159265358979323846f, glm::normalize(axis));
    }
    const glm::vec3 axis = glm::normalize(glm::cross(a, b));
    return glm::angleAxis(std::acos(std::clamp(d, -1.0f, 1.0f)), axis);
}

}  // namespace ptv::two_bone_ik
