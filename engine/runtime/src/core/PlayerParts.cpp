#include "PlayerParts.h"

#include <algorithm>
#include <cmath>

#include <glm/gtx/quaternion.hpp>

#include "ptv/api/InputSnapshot.h"
#include "ptv/api/MechanicContext.h"
#include "ptv/core/Logger.h"
#include "ptv/ecs/World.h"
#include "ptv/physics/PhysicsWorld.h"

namespace ptv {
namespace {

constexpr std::uint32_t kNone = 0xFFFFFFFFu;
/** Grip past this is "closing on something" (builtin.grab's press point). */
constexpr float kGripClosed = 0.7f;

bool is_grabbable(const World& world, entt::entity e) {
    const auto* tag = world.registry().try_get<CTag>(e);
    if (tag == nullptr) return false;
    return std::find(tag->tags.begin(), tag->tags.end(), "grabbable") != tag->tags.end();
}

}  // namespace

void PlayerParts::build_specs() {
    built_ = true;
    parts_.clear();
    for (int side = 0; side < 2; ++side) {
        const std::string s = side == 0 ? "_l" : "_r";
        // Palm: wrist to the middle knuckle.
        parts_.push_back({side, side, "hand" + s, "middle_1" + s, 0.0f, 0.03f});
        // Each finger from its base knuckle past its last joint to the tip.
        for (const char* f : {"thumb", "index", "middle", "ring", "pinky"}) {
            const float r = std::string(f) == "thumb" ? 0.011f : 0.009f;
            parts_.push_back({side, side, std::string(f) + "_1" + s,
                              std::string(f) + "_3" + s, 0.45f, r});
        }
        if (full_avatar_) {
            parts_.push_back({side, side, "forearm" + s, "hand" + s, 0.0f, 0.035f});
            parts_.push_back({2, side, "shin" + s, "foot" + s, 0.0f, 0.05f});
            parts_.push_back({2, side, "foot" + s, "toes" + s, 0.6f, 0.04f});
        }
    }
}

bool PlayerParts::target_pose(const ISkeleton& skeleton, std::uint32_t skel, const Part& p,
                              Vec3& pos, Quat& rot, float& half_height) const {
    const int j0 = skeleton.joint_for_slot(skel, p.from_slot.c_str());
    const int j1 = skeleton.joint_for_slot(skel, p.to_slot.c_str());
    if (j0 < 0 || j1 < 0 || j0 == j1) return false;
    const Vec3 a(skeleton.joint_world(skel, j0)[3]);
    const Vec3 b(skeleton.joint_world(skel, j1)[3]);
    const Vec3 seg = b - a;
    const float len = glm::length(seg);
    if (!std::isfinite(len) || len < 1e-3f || len > 1.5f) return false;
    const Vec3 dir = seg / len;
    const Vec3 end = b + seg * p.extend;
    const float total = len * (1.0f + p.extend);
    pos = 0.5f * (a + end);
    half_height = std::max(0.002f, 0.5f * total - p.radius);
    rot = glm::rotation(Vec3(0.0f, 1.0f, 0.0f), dir);
    return true;
}

void PlayerParts::update(World& world, PhysicsWorld& physics, const ISkeleton& skeleton,
                         const IHands& hands, const InputSnapshot& input, float dt) {
    if (!built_) build_specs();
    if (dt <= 0.0f) dt = 1.0f / 72.0f;

    // What each open hand is reaching for: its parts pass through it.
    for (int h = 0; h < 2; ++h) {
        std::vector<std::uint32_t> ignore;
        const std::uint32_t held = hands.held_entity(h);
        if (held != kNone) ignore.push_back(held);
        const std::uint32_t support = hands.support_entity(h);
        if (support != kNone) ignore.push_back(support);
        const auto& hs = input.hands[static_cast<std::size_t>(h)];
        if (hs.hand_valid && hs.grip < kGripClosed) {
            auto view = world.registry().view<const CTransform, const CTag>();
            for (auto e : view) {
                if (!is_grabbable(world, e)) continue;
                const float d =
                    physics.distance_to_body(static_cast<std::uint32_t>(e), hs.hand_position);
                if (d >= 0.0f && d <= kHoverReachM) ignore.push_back(static_cast<std::uint32_t>(e));
            }
        }
        physics.set_part_ignore(h, ignore);
    }

    // The avatar (whose legs the leg parts follow) is whichever skeleton
    // draws the fingers, when that is a full body.
    std::uint32_t avatar = kNone;
    if (full_avatar_) {
        avatar = hands.finger_target(1) != kNone ? hands.finger_target(1) : hands.finger_target(0);
    }

    for (auto& p : parts_) {
        const std::uint32_t skel = p.group < 2 ? hands.finger_target(p.side) : avatar;
        Vec3 pos(0.0f);
        Quat rot(1.0f, 0.0f, 0.0f, 0.0f);
        float hh = 0.0f;
        // An untracked hand is drawn where it was last seen; it must not
        // keep shoving whatever it was left resting against.
        const bool tracked =
            p.group >= 2 || input.hands[static_cast<std::size_t>(p.side & 1)].hand_valid;
        const bool posed = tracked && skel != kNone && world.valid(skel) &&
                           skeleton.has_skeleton(skel) && target_pose(skeleton, skel, p, pos, rot, hh);
        if (!posed) {
            if (p.entity != kNone && physics.has_body(p.entity)) physics.remove_body(p.entity);
            p.has_pose = false;
            continue;
        }
        if (p.entity == kNone || !world.valid(p.entity)) {
            const auto e = world.create_entity("__player_part");
            Transform t{};
            t.position = pos;
            t.rotation = rot;
            world.registry().emplace<CTransform>(e, CTransform{t});
            // A collider, not something to look at: untagged it fell through
            // to the renderer's placeholder cube, one per finger.
            world.registry().emplace<CTag>(e, CTag{{"player_part", "collider_only"}});
            p.entity = static_cast<std::uint32_t>(e);
        }
        // A capsule's length is fixed at creation; rebuild it if the
        // skeleton it follows changed size (a different avatar).
        if (physics.has_body(p.entity) && std::abs(hh - p.half_height) > 0.01f) {
            physics.remove_body(p.entity);
        }
        if (!physics.has_body(p.entity)) {
            if (physics.create_part_body(p.entity, pos, rot, hh, p.radius, p.group)) {
                p.half_height = hh;
                p.pos = pos;
                p.rot = rot;
                p.has_pose = true;
            }
            continue;
        }
        const Vec3 delta = pos - p.pos;
        const float d = glm::length(delta);
        if (!p.has_pose || !std::isfinite(d) || d > kTeleportM) {
            physics.teleport(p.entity, pos, rot);
            p.pos = pos;
        } else {
            const float step = std::min(d, kMaxPartSpeed * dt);
            const Vec3 next = d > 1e-6f ? p.pos + delta * (step / d) : pos;
            physics.move_kinematic(p.entity, next, rot, dt);
            p.pos = next;
        }
        p.rot = rot;
        p.has_pose = true;
    }
}

void PlayerParts::clear(World& world, PhysicsWorld& physics) {
    for (auto& p : parts_) {
        if (p.entity == kNone) continue;
        if (physics.has_body(p.entity)) physics.remove_body(p.entity);
        if (world.valid(p.entity)) world.registry().destroy(static_cast<entt::entity>(p.entity));
        p.entity = kNone;
        p.has_pose = false;
    }
    built_ = false;
}

int PlayerParts::live_parts() const {
    int n = 0;
    for (const auto& p : parts_) n += p.has_pose ? 1 : 0;
    return n;
}

}  // namespace ptv
