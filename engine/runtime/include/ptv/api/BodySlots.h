#pragma once

// ---------------------------------------------------------------------------
// ptv::BodySlots -- one canonical vocabulary for "a place on a body".
//
// Several systems need to find the same anatomical point on a rig they did
// not author: equip has to put a gauntlet on a forearm, hand_pose has to curl
// an index finger, an avatar body has to drive a neck, and generated code has
// to attach a hat to a head. Each of those could hard-code joint names, and
// before this file existed hand_pose did exactly that -- which quietly meant
// it only worked on rigs that happened to use the engine's own hand fixture
// naming.
//
// The problem is that there is no single naming convention in the wild. This
// pipeline alone produces two: Tripo's auto-rigger emits ``spec="tripo"``
// names for characters, while the engine's own hand fixture uses a
// ``mixamorig:`` contract. Imported assets add Mixamo proper, Unity Humanoid,
// Unreal's skeleton and assorted bespoke rigs.
//
// So a slot is an ENGINE-LEVEL name ("forearm_r"), and this table maps it to
// an ordered list of joint names that different conventions use for it. The
// caller resolves a slot once and caches the index; see
// ``ISkeleton::joint_for_slot``.
//
// Two conventions worth knowing when reading the table:
//
//   * Mixamo has no separate elbow or knee bone. The elbow IS the origin of
//     ``LeftForeArm`` and the knee IS the origin of ``LeftLeg``, so
//     ``elbow_l`` and ``forearm_l`` deliberately resolve to the same joint.
//   * The engine's hand fixture is one GLB per hand, so its joints are
//     UNSIDED (``mixamorig:index_01``, not ``RightHandIndex1``). Finger slots
//     therefore list sided names first and unsided names last, so a full-body
//     rig picks the correct side and a single-hand rig still resolves.
// ---------------------------------------------------------------------------

#include <string>
#include <vector>

namespace ptv {

/** Every canonical slot name, sorted. Useful for validation and for telling
 *  an agent what vocabulary exists. */
const std::vector<std::string>& body_slot_names();

/** True when ``slot`` is part of the canonical vocabulary. */
bool is_body_slot(const std::string& slot);

/** Ordered candidate joint names for ``slot``, most specific first. Empty
 *  when the slot is not in the vocabulary.
 *
 *  Candidates are stored already normalized (see ``normalize_joint_name``),
 *  so a caller comparing against a rig should normalize the rig's names too
 *  rather than expecting a literal match. */
const std::vector<std::string>& body_slot_candidates(const std::string& slot);

/** Lower-cased, prefix-stripped, punctuation-free form of a joint name.
 *
 *  ``mixamorig:LeftForeArm``, ``Left_Fore_Arm``, ``left fore arm`` and
 *  ``leftforearm`` all reduce to ``leftforearm``, which is what lets one
 *  table serve rigs that disagree only about separators and casing. Known
 *  namespace prefixes (``mixamorig:``, ``armature|``) are dropped. */
std::string normalize_joint_name(const std::string& name);

/** The slot name for one finger segment, e.g. ``("index", 1, 1)`` ->
 *  ``"index_1_r"``. ``segment`` is 1-based (1 = proximal, 3 = distal);
 *  ``hand`` follows ``InputSnapshot::Hand`` (0 = left, 1 = right). Keeps the
 *  naming convention in one place instead of string-building at call sites. */
std::string body_finger_slot(const std::string& finger, int segment, int hand);

/** ``"_l"`` or ``"_r"`` for a hand index, for building sided slot names. */
const char* body_side_suffix(int hand);

}  // namespace ptv
