// builtin.avatar_body -- the player's own body.
//
// Before this existed the player was a kinematic capsule plus two floating
// rigged hands. Looking down showed nothing; there was no torso to wear
// anything on and no elbow to fit a gauntlet to.
//
// What this mechanic owns:
//
//   1. Planting the avatar on the XR rig, so the body travels with
//      locomotion and faces where the rig faces.
//   2. Driving the neck and head from the tracked headset pose, so the body
//      agrees with where the player is actually looking.
//   3. Suppressing the head in the player's OWN view only, via CSelfViewHide,
//      so the camera is not inside the skull while a mirror still shows a
//      complete avatar.
//   4. Two-bone IK on both arms toward the controllers, which is what makes a
//      forearm-mounted item look attached rather than floating.
//   5. Legs: idle/walk/run chosen from the speed the rig actually achieved,
//      with playback scaled so the stride matches the ground covered. Shared
//      with builtin.character_locomotion so the player and an NPC walking at
//      the same speed animate identically.
//
// What it deliberately does NOT own: fingers. builtin.hand_pose already
// implements the whole grip/trigger/thumb curl table, and two writers on the
// same joints fight.
//
// Every behavioural switch is a parameter rather than an inference, because
// the review this was built from is explicit that a modelled head must not
// imply an inside-the-skull camera, and wanting a mirror must not imply
// wanting animated legs.

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

#include "../BuiltinMechanicBase.h"
#include "../LocomotionClipSelector.h"
#include "../TwoBoneIk.h"
#include "ptv/api/BodySlots.h"
#include "ptv/core/Logger.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace ptv {
namespace {

/** How much of the body the asset contains. Mirrors AvatarCoverage in the
 *  BuildSpec schema. */
enum class Coverage : int { FullBody = 0, NeckDown = 1, HandsOnly = 2 };

/** Whether the head renders, and to whom. Mirrors HeadVisibility. */
enum class HeadVisibility : int {
    HiddenInSelfView = 0,
    AlwaysVisible = 1,
    Toggleable = 2,
};

Coverage coverage_from(const std::string& s) {
    if (s == "neck_down") return Coverage::NeckDown;
    if (s == "hands_only") return Coverage::HandsOnly;
    return Coverage::FullBody;
}

HeadVisibility head_visibility_from(const std::string& s) {
    if (s == "always_visible") return HeadVisibility::AlwaysVisible;
    if (s == "toggleable") return HeadVisibility::Toggleable;
    return HeadVisibility::HiddenInSelfView;
}

/** Entity tag the scene compiler puts on the player's avatar. */
constexpr const char* kAvatarTag = "player_avatar";

/** Elbows prefer to bend down and slightly back, in the rig's local frame.
 *  Without a bias like this an arm reaching forward can solve with the elbow
 *  pointing up, which reads as broken even though the hand is correct. */
constexpr float kPoleDown = -1.0f;
constexpr float kPoleBack = 0.45f;

class AvatarBodyMechanic : public BuiltinMechanicBase<AvatarBodyMechanic> {
public:
    static constexpr const char* kId = "builtin.avatar_body";

    // Simulate, so the pose system (which runs immediately after) applies
    // everything written here on the same frame. Player scope: there is one
    // avatar, however many entities happen to carry the binding.
    MechanicPhase phase() const override { return MechanicPhase::Simulate; }
    MechanicScope scope() const override { return MechanicScope::Player; }

    void configure(const nlohmann::json& p) {
        coverage_ = coverage_from(p.value("coverage", std::string("full_body")));
        head_visibility_ = head_visibility_from(
            p.value("head_visibility", std::string("hidden_in_self_view")));
        leg_animation_ = p.value("leg_animation", true);
        // Trim for where the camera sits relative to the rig's head joint.
        // Rigs disagree about whether the head joint is at the base of the
        // skull or between the eyes, and the difference is very visible.
        eye_height_offset_ =
            std::clamp(p.value("eye_height_offset_m", 0.0f), -0.5f, 0.5f);
        arm_ik_ = p.value("arm_ik", true);
        head_tracking_ = p.value("head_tracking", true);
        tune_.walk_speed_mps =
            std::clamp(p.value("walk_speed_mps", 1.4f), 0.05f, 20.0f);
        tune_.run_speed_mps = std::clamp(p.value("run_speed_mps", 4.0f),
                                         tune_.walk_speed_mps, 40.0f);
        // Starts hidden for the toggle case too; the game shows it on demand.
        head_shown_ = head_visibility_ == HeadVisibility::AlwaysVisible;
        resolved_ = false;
    }

    void tick(World& world, std::uint32_t, float dt, const InputSnapshot& input,
              MechanicContext& ctx) {
        if (ctx.locomotion == nullptr || ctx.skeleton == nullptr) return;

        const std::uint32_t avatar = find_avatar(world);
        if (avatar == InputSnapshot::kInvalidEntity) return;
        if (!world.has<CTransform>(avatar)) return;

        if (!resolved_) resolve(world, avatar, ctx);

        plant_on_rig(world, avatar, ctx, dt);
        if (head_tracking_) drive_head(avatar, input, ctx);
        if (arm_ik_) drive_arms(avatar, input, ctx);
        if (leg_animation_) drive_legs(world, avatar, ctx, dt);
        apply_head_visibility(world, avatar, ctx);
    }

    /** Show or hide the head at runtime. Reached through the mechanic rather
     *  than the component so the toggle respects head_visibility: a game that
     *  asked for an always-visible head cannot be talked into hiding it. */
    void set_head_shown(bool shown) {
        if (head_visibility_ == HeadVisibility::Toggleable) head_shown_ = shown;
    }

private:
    std::uint32_t find_avatar(World& world) {
        if (avatar_ != InputSnapshot::kInvalidEntity && world.valid(avatar_)) {
            return avatar_;
        }
        auto view = world.registry().view<const CTag, const CTransform>();
        for (auto e : view) {
            for (const auto& t : view.get<const CTag>(e).tags) {
                if (t == kAvatarTag) {
                    avatar_ = static_cast<std::uint32_t>(e);
                    return avatar_;
                }
            }
        }
        return InputSnapshot::kInvalidEntity;
    }

    void resolve(World& world, std::uint32_t avatar, MechanicContext& ctx) {
        if (!ctx.skeleton->has_skeleton(avatar)) {
            // A coverage that has no rig is legitimate (hands_only), so this
            // is not an error -- but a full body without a skeleton cannot be
            // posed at all and is worth saying out loud once.
            if (coverage_ != Coverage::HandsOnly) {
                PTV_WARN(
                    "builtin.avatar_body: the player avatar has no skeleton; "
                    "the body will not be posed");
            }
            resolved_ = true;
            return;
        }

        neck_ = ctx.skeleton->joint_for_slot(avatar, "neck");
        head_ = ctx.skeleton->joint_for_slot(avatar, "head");
        for (int h = 0; h < 2; ++h) {
            const std::string side = body_side_suffix(h);
            auto& arm = arms_[static_cast<size_t>(h)];
            arm.shoulder =
                ctx.skeleton->joint_for_slot(avatar, ("shoulder" + side).c_str());
            arm.upper =
                ctx.skeleton->joint_for_slot(avatar, ("upper_arm" + side).c_str());
            arm.fore =
                ctx.skeleton->joint_for_slot(avatar, ("forearm" + side).c_str());
            arm.hand =
                ctx.skeleton->joint_for_slot(avatar, ("hand" + side).c_str());

            // Bone lengths come from the rig's own bind pose, so a child-sized
            // avatar and a giant both solve correctly without configuration.
            if (arm.upper >= 0 && arm.fore >= 0 && arm.hand >= 0) {
                const Mat4 su = ctx.skeleton->joint_world(avatar, arm.upper);
                const Mat4 sf = ctx.skeleton->joint_world(avatar, arm.fore);
                const Mat4 sh = ctx.skeleton->joint_world(avatar, arm.hand);
                arm.upper_len = glm::length(Vec3(sf[3]) - Vec3(su[3]));
                arm.lower_len = glm::length(Vec3(sh[3]) - Vec3(sf[3]));
            }
        }

        // Cache the head subtree once so the per-frame path is a component
        // write rather than a hierarchy walk.
        if (head_ >= 0) {
            std::array<int, 64> buf{};
            const int n = ctx.skeleton->joint_subtree(
                avatar, head_, buf.data(), static_cast<int>(buf.size()));
            head_subtree_.assign(buf.begin(), buf.begin() + n);
        }

        (void)world;
        resolved_ = true;
    }

    /** Put the avatar where the rig is, facing where the rig faces. */
    void plant_on_rig(World& world, std::uint32_t avatar, MechanicContext& ctx,
                      float dt) {
        auto& t = world.registry().get<CTransform>(
            static_cast<entt::entity>(avatar)).t;
        const Vec3 rig = ctx.locomotion->position();
        const float yaw = ctx.locomotion->yaw_radians();

        const Vec3 previous = t.position;
        t.position = rig;
        t.rotation = glm::angleAxis(yaw, Vec3(0.0f, 1.0f, 0.0f));

        // Speed the body ACHIEVED, which is what the gait selection needs. A
        // player walking into a wall should stand still, not run on the spot.
        if (dt > 1.0e-5f) {
            const Vec3 delta(t.position.x - previous.x, 0.0f,
                             t.position.z - previous.z);
            const float instant = glm::length(delta) / dt;
            // Light smoothing: the rig's position is integrated per frame and
            // raw per-frame differences are noisy enough to flicker gaits.
            speed_ += (instant - speed_) * std::min(1.0f, dt / 0.08f);
        }
    }

    /** Point the head where the headset points, so the body is not staring
     *  forward while the player looks sideways. */
    void drive_head(std::uint32_t avatar, const InputSnapshot& input,
                    MechanicContext& ctx) {
        if (head_ < 0 || !input.head_valid) return;
        // The rig's yaw is already baked into the avatar's root, so only the
        // headset's rotation RELATIVE to the rig belongs on the neck.
        const Quat rig_yaw =
            glm::angleAxis(ctx.locomotion->yaw_radians(), Vec3(0, 1, 0));
        const Quat relative = glm::inverse(rig_yaw) * input.head_rotation;
        // Split it: most on the head, a little on the neck, which is what
        // stops a 90-degree look reading as a broken neck.
        if (neck_ >= 0) {
            ctx.skeleton->set_joint_rotation(
                avatar, neck_, glm::slerp(Quat(1, 0, 0, 0), relative, 0.35f));
            ctx.skeleton->set_joint_rotation(
                avatar, head_, glm::slerp(Quat(1, 0, 0, 0), relative, 0.65f));
        } else {
            ctx.skeleton->set_joint_rotation(avatar, head_, relative);
        }
    }

    void drive_arms(std::uint32_t avatar, const InputSnapshot& input,
                    MechanicContext& ctx) {
        for (int h = 0; h < 2; ++h) {
            const auto& arm = arms_[static_cast<size_t>(h)];
            if (arm.upper < 0 || arm.fore < 0) continue;
            const auto& hand = input.hands[h];
            if (!hand.hand_valid) continue;
            if (arm.upper_len <= 1.0e-4f || arm.lower_len <= 1.0e-4f) continue;

            const Mat4 upper_world = ctx.skeleton->joint_world(avatar, arm.upper);
            const Vec3 shoulder(upper_world[3]);

            // Bend down and back in the avatar's frame, not the world's, so
            // the elbow behaves the same whichever way the player is facing.
            const Quat rig_yaw =
                glm::angleAxis(ctx.locomotion->yaw_radians(), Vec3(0, 1, 0));
            const Vec3 pole =
                rig_yaw * glm::normalize(Vec3(0.0f, kPoleDown, kPoleBack));

            const auto sol = two_bone_ik::solve(shoulder, hand.hand_position,
                                                arm.upper_len, arm.lower_len,
                                                pole);
            if (!sol.valid) continue;

            // Rotate each bone from where the rig has it to where the solve
            // wants it. Writing rotations rather than joint translations
            // keeps the mesh from stretching.
            const Mat4 fore_world = ctx.skeleton->joint_world(avatar, arm.fore);
            const Vec3 bind_upper_dir = Vec3(fore_world[3]) - shoulder;
            const Vec3 want_upper_dir = sol.elbow - shoulder;
            apply_bone_rotation(avatar, arm.upper, bind_upper_dir,
                                want_upper_dir, ctx);

            if (arm.hand >= 0) {
                const Mat4 hand_world =
                    ctx.skeleton->joint_world(avatar, arm.hand);
                const Vec3 bind_lower_dir =
                    Vec3(hand_world[3]) - Vec3(fore_world[3]);
                const Vec3 want_lower_dir = sol.wrist - sol.elbow;
                apply_bone_rotation(avatar, arm.fore, bind_lower_dir,
                                    want_lower_dir, ctx);
            }
        }
    }

    void apply_bone_rotation(std::uint32_t avatar, int joint, const Vec3& from,
                             const Vec3& to, MechanicContext& ctx) const {
        if (glm::length(from) < 1.0e-5f || glm::length(to) < 1.0e-5f) return;
        const Quat delta = two_bone_ik::rotation_between(from, to);
        ctx.skeleton->set_joint_rotation(avatar, joint, delta);
    }

    void drive_legs(World& world, std::uint32_t avatar, MechanicContext& ctx,
                    float dt) {
        (void)dt;
        const float height = locomotion_clips::body_height(world, avatar);
        clips_.update(world, avatar, speed_, /*sprint=*/false, height, tune_,
                      ctx);
    }

    /** Keep CSelfViewHide in agreement with the configured policy.
     *
     *  Written every frame rather than once at attach because the toggle case
     *  changes it at runtime, and because the head subtree is not resolvable
     *  until the avatar's skeleton exists. */
    void apply_head_visibility(World& world, std::uint32_t avatar,
                               MechanicContext& ctx) {
        (void)ctx;
        const bool should_hide =
            coverage_ == Coverage::FullBody && !head_subtree_.empty() &&
            head_visibility_ != HeadVisibility::AlwaysVisible && !head_shown_;

        const auto e = static_cast<entt::entity>(avatar);
        if (!should_hide) {
            if (world.registry().all_of<CSelfViewHide>(e)) {
                world.registry().get<CSelfViewHide>(e).scale = 1.0f;
            }
            return;
        }
        auto& hide = world.registry().get_or_emplace<CSelfViewHide>(e);
        if (hide.joints != head_subtree_) hide.joints = head_subtree_;
        hide.scale = 0.0f;
    }

    struct Arm {
        int shoulder{-1};
        int upper{-1};
        int fore{-1};
        int hand{-1};
        float upper_len{0.0f};
        float lower_len{0.0f};
    };

    Coverage coverage_{Coverage::FullBody};
    HeadVisibility head_visibility_{HeadVisibility::HiddenInSelfView};
    bool leg_animation_{true};
    bool arm_ik_{true};
    bool head_tracking_{true};
    float eye_height_offset_{0.0f};
    bool head_shown_{false};

    bool resolved_{false};
    std::uint32_t avatar_{InputSnapshot::kInvalidEntity};
    int neck_{-1};
    int head_{-1};
    std::vector<int> head_subtree_;
    std::array<Arm, 2> arms_{};

    float speed_{0.0f};
    locomotion_clips::Tunables tune_{};
    locomotion_clips::Selector clips_;
};

}  // namespace

// Anchor so MSVC keeps this TU (and its registration) when linking
// ptv_runtime as a static library. Referenced by BuiltinMechanics.cpp.
extern "C" int ptv_force_link_builtin_avatar_body = 1;

PTV_REGISTER_MECHANIC("builtin.avatar_body", AvatarBodyMechanic)

}  // namespace ptv
