#pragma once

// The player's body parts as colliders: palms and fingers of the hands the
// player sees, and -- with a full-body avatar -- forearms and shins.
//
// The player's BODY (the walking capsule) never touches a prop. Its PARTS
// do, the way real hands and feet would: a finger can tip a can over, a
// palm can push a crate, a shin can knock a bottle off a step. Each part is
// a kinematic capsule on the PlayerPart physics layer that follows the
// joints of the skeleton actually drawn (the default hands, or the avatar).
//
// Two rules keep that from fighting the player:
//   * a part moves at most ``kMaxPartSpeed``; a hand that jumps (a tracking
//     glitch, a teleport) is teleported rather than swept, so it cannot hurl
//     whatever it passes through;
//   * while a hand is open, its parts pass through the grabbable it is
//     reaching for (and anything it already holds), so the fingers close on
//     a pistol instead of pushing it off the table first.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "ptv/math/Math.h"

namespace ptv {

class World;
class PhysicsWorld;
class ISkeleton;
class IHands;
struct InputSnapshot;

class PlayerParts {
public:
    /** ``full_avatar``: also forearms and shins (the avatar's own limbs). */
    void configure(bool full_avatar) { full_avatar_ = full_avatar; }

    /** Move every part onto this frame's pose, creating bodies that are
     *  missing (after a physics rebuild) and parking parts whose skeleton
     *  is gone. Call after poses and attachments are final. */
    void update(World& world, PhysicsWorld& physics, const ISkeleton& skeleton,
                const IHands& hands, const InputSnapshot& input, float dt);

    /** Destroy every part entity and body (scene unload). */
    void clear(World& world, PhysicsWorld& physics);

    /** How many part bodies exist (telemetry, tests). */
    int live_parts() const;

    /** Max speed a part is swept at, m/s. */
    static constexpr float kMaxPartSpeed = 4.0f;
    /** A pose jump larger than this in one frame is teleported, not swept. */
    static constexpr float kTeleportM = 0.35f;
    /** While open, a hand passes through grabbables within this distance of
     *  its palm (the grab search's own reach plus a margin). */
    static constexpr float kHoverReachM = 0.14f;

private:
    struct Part {
        int group{0};  // 0 left hand, 1 right hand, 2 legs
        int side{0};   // 0 left, 1 right
        std::string from_slot;
        std::string to_slot;
        /** Extend past ``to_slot`` by this fraction of the segment (fingertips). */
        float extend{0.0f};
        float radius{0.01f};
        std::uint32_t entity{0xFFFFFFFFu};
        bool has_pose{false};
        Vec3 pos{0.0f};
        Quat rot{1.0f, 0.0f, 0.0f, 0.0f};
        float half_height{0.0f};
    };

    void build_specs();
    bool target_pose(const ISkeleton& skeleton, std::uint32_t skel, const Part& p,
                     Vec3& pos, Quat& rot, float& half_height) const;

    std::vector<Part> parts_;
    bool full_avatar_{false};
    bool built_{false};
};

}  // namespace ptv
