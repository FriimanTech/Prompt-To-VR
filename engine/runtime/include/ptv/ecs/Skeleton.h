#pragma once

// ---------------------------------------------------------------------------
// ptv::tick_animators -- per-frame pose evaluation for CSkeleton / CAnimator.
//
// Two things can drive a joint, and they meet here:
//
//   * A glTF animation clip, advanced by a time cursor. This is what Tripo's
//     animation-retarget output produces, and what a walk cycle is.
//   * A procedural override written by a mechanic during Phase::Simulate,
//     e.g. index-finger curl computed from input.hands[h].trigger. There is
//     no clip for that: the pose is a function of the current input, not of
//     time.
//
// Overrides win per joint, so a character can play its idle clip while code
// steers only its head or its trigger finger.
//
// Call this once per frame AFTER the Simulate-phase mechanics have run (so an
// override requested this frame is visible this frame) and BEFORE the
// renderer builds its draw list -- the same slot, and for the same reason, as
// tick_transform_offsets.
// ---------------------------------------------------------------------------

#include "ptv/ecs/World.h"

namespace ptv {

/** Advance every ``CAnimator`` and rebuild every ``CSkeleton`` palette.
 *
 * Per entity: reset the pose to bind, sample the active clip into every joint
 * that was not overridden this frame, compose world matrices parent-first,
 * and write ``jointWorld * inverseBind`` into the palette. Entities with a
 * ``CSkeleton`` but no ``CAnimator`` still get their palette rebuilt, which
 * is what makes a purely procedurally-posed rig work. */
void tick_animators(World& world, float dt);

/** Distance bands controlling how often a character's pose is recomputed.
 *
 * Posing a skeleton and rebuilding its palette is per-joint CPU work that
 * competes with the render thread, and a 65-joint character costs the same
 * whether it fills the view or is a speck on the horizon. Meta's own guidance
 * puts a Quest 3 "busy simulation" -- their term for an app with many
 * animated NPCs -- at 200-300 draw calls, and calls out animation and
 * skinning as work that delays the render thread. So distant characters are
 * sampled less often.
 *
 * Crucially the clip still advances by the FULL elapsed time when it is
 * sampled, so a distant character walks at the correct speed; only the
 * smoothness drops, which is what nobody can see at 20 metres. */
struct AnimationLod {
    /** Nearer than this: every frame. */
    float full_rate_m{8.0f};
    /** Nearer than this: every other frame. Beyond: every fourth. */
    float half_rate_m{15.0f};
    /** Beyond this: not posed at all. The mesh holds its last pose. */
    float stop_m{40.0f};
    /** Set false to pose everything every frame (tests, tiny scenes). */
    bool enabled{true};
};

/** Pose evaluation with distance-based throttling.
 *
 * ``viewer`` is the headset position. Entities without a ``CTransform``
 * cannot be measured and are always posed at full rate. */
void tick_animators(World& world, float dt, const Vec3& viewer,
                    const AnimationLod& lod);

/** Sample one clip into ``out_pose`` at ``time_s``.
 *
 * Exposed separately from the tick so tests can evaluate a clip at an exact
 * time without running a frame. Joints flagged in ``overridden`` are left
 * untouched; pass an empty span to sample everything. */
void sample_animation_clip(const gltf_mesh_load::AnimationClip& clip,
                           float time_s,
                           const std::vector<std::uint8_t>& overridden,
                           std::vector<gltf_mesh_load::JointPose>& out_pose);

}  // namespace ptv
