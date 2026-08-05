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
