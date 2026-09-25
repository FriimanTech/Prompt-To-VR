// builtin.avatar_body -- the player's own body, first person.
//
// What "the GLB avatar IS the player's body" requires, and what this does:
//
//   1. The body stands UNDER THE HEADSET. The avatar's eye point is placed
//      at the headset horizontally and the feet on the ground the player
//      motor found, every frame -- so walking around the real playspace
//      carries the body along, and looking down shows chest, legs and feet
//      instead of empty floor. (The old version planted the body on the
//      stage origin, which is wherever the playspace centre happens to be:
//      walk a metre and the body stayed behind.)
//   2. It faces the way the player faces. The GLB's own forward axis is
//      measured from its shoulders and corrected for, because assets
//      disagree (glTF says +Z, OpenXR looks down -Z, some exports are
//      turned a quarter turn). The torso follows the head's yaw through a
//      comfortable dead zone and turns instantly with snap turns.
//   3. It is the player's size. The avatar is scaled once so its eyes sit
//      at the player's real eye height (``height_calibration``).
//   4. The head and neck follow the headset, bind-relative, and the head is
//      hidden in the player's own view only (CSelfViewHide), so a mirror
//      still shows a whole avatar.
//   5. The arms reach the controllers with two-bone IK solved in the
//      CURRENT frame's pose: each joint is written and its child re-read
//      before the next is solved. (The old version rotated bones by a world
//      delta written as a LOCAL rotation, measured against last frame's
//      palette, so alternate frames undid each other -- the arms flailed even
//      with the controllers held still.)
//   6. The hands are the avatar's own. Their wrists take the controller's
//      orientation through the rig's measured palm frame, and when the rig
//      has finger bones the finger targets are pointed here so
//      builtin.hand_pose curls THESE fingers. No substitute hands are drawn.
//   7. Legs: idle/walk/run chosen from the speed the body actually moved.
//
// What it does NOT own: fingers (builtin.hand_pose) and where the player is
// (the player motor). It is bound on the ``player_avatar`` entity itself.

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

#include "../BuiltinMechanicBase.h"
#include "../HandRig.h"
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

constexpr float kPi = 3.14159265358979323846f;

/** Scale that hides a joint's geometry without degenerating its basis. */
constexpr float kHiddenScale = 0.001f;

/** Anthropometric ratios of stature, used to find the eyes on a rig that
 *  only names its head joint (which sits at the base of the skull). */
constexpr float kEyeAboveHeadJoint = 0.065f;
constexpr float kEyeForwardOfHeadJoint = 0.045f;
constexpr float kAnkleHeight = 0.045f;

float wrap_pi(float a) {
    while (a > kPi) a -= 2.0f * kPi;
    while (a < -kPi) a += 2.0f * kPi;
    return a;
}

/** Yaw of a forward vector: R_y(yaw) * (0,0,-1) == forward. */
float yaw_of(const Vec3& fwd) { return std::atan2(-fwd.x, -fwd.z); }

Quat rot_of(const Mat4& m) {
    Mat3 r(m);
    r[0] = glm::normalize(r[0]);
    r[1] = glm::normalize(r[1]);
    r[2] = glm::normalize(r[2]);
    return glm::normalize(glm::quat_cast(r));
}

class AvatarBodyMechanic : public BuiltinMechanicBase<AvatarBodyMechanic> {
public:
    static constexpr const char* kId = "builtin.avatar_body";

    // Simulate, so the pose system (which runs immediately after) applies
    // everything written here on the same frame. Player scope: there is one
    // avatar, however many entities happen to carry the binding. Before
    // builtin.hand_pose, which curls fingers on the wrists placed here.
    MechanicPhase phase() const override { return MechanicPhase::Simulate; }
    MechanicScope scope() const override { return MechanicScope::Player; }
    int order() const override { return -10; }

    void configure(const nlohmann::json& p) {
        coverage_ = coverage_from(p.value("coverage", std::string("full_body")));
        head_visibility_ = head_visibility_from(
            p.value("head_visibility", std::string("hidden_in_self_view")));
        leg_animation_ = p.value("leg_animation", true);
        eye_height_offset_ =
            std::clamp(p.value("eye_height_offset_m", 0.0f), -0.5f, 0.5f);
        arm_ik_ = p.value("arm_ik", true);
        head_tracking_ = p.value("head_tracking", true);
        height_calibration_ =
            p.value("height_calibration", std::string("auto")) != "off";
        yaw_deadzone_ = std::clamp(
            p.value("torso_yaw_deadzone_deg", 40.0f), 5.0f, 90.0f) *
                        kPi / 180.0f;
        tune_.walk_speed_mps =
            std::clamp(p.value("walk_speed_mps", 1.4f), 0.05f, 20.0f);
        tune_.run_speed_mps = std::clamp(p.value("run_speed_mps", 4.0f),
                                         tune_.walk_speed_mps, 40.0f);
        head_shown_ = head_visibility_ == HeadVisibility::AlwaysVisible;
        resolved_ = false;
        avatar_ = InputSnapshot::kInvalidEntity;
    }

    void tick(World& world, std::uint32_t, float dt, const InputSnapshot& input,
              MechanicContext& ctx) {
        if (ctx.locomotion == nullptr || ctx.skeleton == nullptr) return;

        const std::uint32_t avatar = find_avatar(world);
        if (avatar == InputSnapshot::kInvalidEntity) return;
        if (!world.has<CTransform>(avatar)) return;
        if (!resolved_) resolve(world, avatar, ctx);
        if (!ctx.skeleton->has_skeleton(avatar)) return;

        calibrate_height(world, avatar, input, ctx, dt);
        plant_under_head(world, avatar, input, ctx, dt);
        if (head_tracking_) drive_head(avatar, input, ctx);
        if (arm_ik_) drive_arms(avatar, input, ctx);
        if (leg_animation_) drive_legs(world, avatar, ctx);
        hide_engine_hands(ctx);
        apply_head_visibility(world, avatar);
    }

    /** Show or hide the head at runtime; respects head_visibility. */
    void set_head_shown(bool shown) {
        if (head_visibility_ == HeadVisibility::Toggleable) head_shown_ = shown;
    }

private:
    struct Arm {
        int clavicle{-1};
        int upper{-1};
        int fore{-1};
        int hand{-1};
        float upper_len{0.0f};  // bind, metres, before entity scale
        float lower_len{0.0f};
        hand_rig::Rig rig{};
        bool has_fingers{false};
        // For a wrist-only rig: grip -> wrist rotation, and palm centre in
        // the wrist's local frame.
        Quat grip_to_wrist{1.0f, 0.0f, 0.0f, 0.0f};
        Vec3 palm_offset_local{0.0f};
    };

    std::uint32_t find_avatar(World& world) {
        if (avatar_ != InputSnapshot::kInvalidEntity && world.valid(avatar_)) {
            return avatar_;
        }
        resolved_ = false;
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

    Vec3 bind_pos(MechanicContext& ctx, std::uint32_t a, int j) const {
        return Vec3(ctx.skeleton->joint_bind_model(a, j)[3]);
    }

    void resolve(World& world, std::uint32_t avatar, MechanicContext& ctx) {
        // Nothing else may own the avatar's transform: tick_attachments runs
        // AFTER Simulate, so an attachment would pin the body to whatever it
        // was parented to.
        const auto e = static_cast<entt::entity>(avatar);
        if (world.registry().all_of<CAttachment>(e)) {
            world.registry().remove<CAttachment>(e);
            PTV_WARN(
                "builtin.avatar_body: the player avatar was attached to "
                "another entity; dropping the attachment so the body follows "
                "the player. Tag the avatar and leave attached_to unset.");
        }
        base_scale_ = world.get<CTransform>(avatar).t.scale;
        calibrated_scale_ = 1.0f;
        calibration_samples_.clear();
        calibration_done_ = !height_calibration_;

        if (!ctx.skeleton->has_skeleton(avatar)) {
            // A skeleton can attach a frame or two after load; try again.
            if (coverage_ != Coverage::HandsOnly && !warned_no_skeleton_) {
                PTV_WARN(
                    "builtin.avatar_body: the player avatar has no skeleton "
                    "yet; the body cannot be posed until it does");
                warned_no_skeleton_ = true;
            }
            return;
        }

        auto slot = [&](const std::string& s) {
            return ctx.skeleton->joint_for_slot(avatar, s.c_str());
        };
        neck_ = slot("neck");
        head_ = slot("head");
        for (int h = 0; h < 2; ++h) {
            const std::string side = body_side_suffix(h);
            auto& arm = arms_[static_cast<size_t>(h)];
            arm = Arm{};
            arm.clavicle = slot("shoulder" + side);
            arm.upper = slot("upper_arm" + side);
            arm.fore = slot("forearm" + side);
            arm.hand = slot("hand" + side);
            if (arm.upper >= 0 && arm.fore >= 0 && arm.hand >= 0) {
                const Vec3 su = bind_pos(ctx, avatar, arm.upper);
                const Vec3 sf = bind_pos(ctx, avatar, arm.fore);
                const Vec3 sh = bind_pos(ctx, avatar, arm.hand);
                arm.upper_len = glm::length(sf - su);
                arm.lower_len = glm::length(sh - sf);
            }
            arm.rig = hand_rig::analyze(*ctx.skeleton, avatar, h);
            arm.has_fingers = arm.rig.valid && arm.rig.wrist == arm.hand;
            if (arm.has_fingers) {
                arm.grip_to_wrist = arm.rig.grip_to_wrist_rot;
                arm.palm_offset_local = arm.rig.palm_offset_wrist_local;
            } else if (arm.hand >= 0 && arm.fore >= 0) {
                wrist_only_alignment(ctx, avatar, h, arm);
            }
        }

        // --- Which way does this GLB face? ------------------------------
        // Across the shoulders is the most reliable "right" a humanoid rig
        // offers; forward is up x right.
        Vec3 fwd_model(0.0f, 0.0f, 1.0f);  // glTF convention as a fallback
        const auto& al = arms_[0];
        const auto& ar = arms_[1];
        if (al.upper >= 0 && ar.upper >= 0) {
            Vec3 right = bind_pos(ctx, avatar, ar.upper) -
                         bind_pos(ctx, avatar, al.upper);
            right.y = 0.0f;
            if (glm::length(right) > 1e-4f) {
                fwd_model = glm::normalize(
                    glm::cross(Vec3(0.0f, 1.0f, 0.0f), glm::normalize(right)));
            }
        }
        // Rotation that turns the model's forward onto -Z.
        facing_ = glm::angleAxis(-yaw_of(fwd_model), Vec3(0.0f, 1.0f, 0.0f));
        forward_model_ = fwd_model;

        // --- Eyes and soles, in model space ------------------------------
        height_m_ = locomotion_clips::body_height(world, avatar);
        float sole = 0.0f;
        {
            float lowest = 1e9f;
            bool any = false;
            for (int h = 0; h < 2; ++h) {
                for (const char* s : {"foot", "toes"}) {
                    const int j = slot(std::string(s) + body_side_suffix(h));
                    if (j < 0) continue;
                    lowest = std::min(lowest, bind_pos(ctx, avatar, j).y);
                    any = true;
                }
            }
            if (any) sole = lowest - kAnkleHeight * height_m_ * 0.5f;
        }
        sole_y_model_ = sole;
        if (head_ >= 0) {
            eye_model_ = bind_pos(ctx, avatar, head_) +
                         Vec3(0.0f, kEyeAboveHeadJoint * height_m_, 0.0f) +
                         fwd_model * (kEyeForwardOfHeadJoint * height_m_);
        } else {
            eye_model_ = Vec3(0.0f, sole + 0.935f * height_m_, 0.0f) +
                         fwd_model * (0.04f * height_m_);
        }
        eye_model_.y += eye_height_offset_;

        head_subtree_.clear();
        if (head_ >= 0) {
            std::array<int, 64> buf{};
            const int n = ctx.skeleton->joint_subtree(
                avatar, head_, buf.data(), static_cast<int>(buf.size()));
            head_subtree_.assign(buf.begin(), buf.begin() + n);
        }

        // Point hand_pose at the avatar's fingers, or say plainly there are
        // none. A wrist-only rig keeps its own rigid hands.
        for (int h = 0; h < 2; ++h) {
            const auto& arm = arms_[static_cast<size_t>(h)];
            if (ctx.hands != nullptr) {
                ctx.hands->set_finger_target(
                    h, arm.has_fingers ? avatar : InputSnapshot::kInvalidEntity);
            }
        }
        if (!arms_[0].has_fingers && !arms_[1].has_fingers) {
            PTV_INFO(
                "builtin.avatar_body: this avatar's rig has no finger bones; "
                "its hands follow the controllers but the fingers will not "
                "curl");
        }

        body_yaw_ = ctx.locomotion->yaw_radians();
        last_rig_yaw_ = body_yaw_;
        have_prev_root_ = false;
        resolved_ = true;
    }

    /** Grip alignment for a hand with no fingers: the hand points along the
     *  forearm, and the palm faces down in the rig's rest pose (the T-pose
     *  every auto-rigger produces). */
    void wrist_only_alignment(MechanicContext& ctx, std::uint32_t avatar,
                              int hand, Arm& arm) const {
        const Mat4 wrist_bind = ctx.skeleton->joint_bind_model(avatar, arm.hand);
        const Vec3 f = glm::normalize(bind_pos(ctx, avatar, arm.hand) -
                                      bind_pos(ctx, avatar, arm.fore));
        Vec3 n(0.0f, -1.0f, 0.0f);
        n -= f * glm::dot(n, f);
        if (glm::length(n) < 1e-4f) n = Vec3(0.0f, 0.0f, 1.0f);
        n = glm::normalize(n);
        const Vec3 gx = (hand == 0) ? n : -n;
        const Vec3 gy = -f;
        const Vec3 gz = glm::normalize(glm::cross(gx, gy));
        Mat4 H(1.0f);
        H[0] = Vec4(gx, 0.0f);
        H[1] = Vec4(gy, 0.0f);
        H[2] = Vec4(gz, 0.0f);
        // The grip sits in the palm, roughly a hand's width past the wrist.
        H[3] = Vec4(Vec3(wrist_bind[3]) + f * (0.045f * height_m_), 1.0f);
        const Mat4 in_wrist = glm::inverse(wrist_bind) * H;
        arm.grip_to_wrist =
            glm::normalize(glm::inverse(glm::quat_cast(Mat3(in_wrist))));
        arm.palm_offset_local = Vec3(in_wrist[3]);
    }

    float scale() const { return base_scale_ * calibrated_scale_; }

    /** Size the avatar to the player once, from their standing eye height. */
    void calibrate_height(World& world, std::uint32_t avatar,
                          const InputSnapshot& input, MechanicContext& ctx,
                          float dt) {
        if (calibration_done_ || !input.head_valid) return;
        calibration_wait_s_ += dt;
        if (calibration_wait_s_ < 0.5f) return;  // let tracking settle
        const float ground = ctx.locomotion->body_position().y;
        calibration_samples_.push_back(ctx.locomotion->head_position().y -
                                       ground);
        if (calibration_samples_.size() < 45) return;  // ~0.6 s of poses
        std::vector<float> s = calibration_samples_;
        std::nth_element(s.begin(), s.begin() + s.size() / 2, s.end());
        const float user_eye = s[s.size() / 2];
        const float avatar_eye = (eye_model_.y - sole_y_model_) * base_scale_;
        if (avatar_eye > 0.3f && user_eye > 0.5f) {
            calibrated_scale_ = std::clamp(user_eye / avatar_eye, 0.75f, 1.3f);
            PTV_INFO(
                "builtin.avatar_body: eye height {:.2f} m -> avatar scale "
                "{:.3f}",
                user_eye, calibrated_scale_);
        }
        calibration_done_ = true;
        (void)world;
        (void)avatar;
    }

    void plant_under_head(World& world, std::uint32_t avatar,
                          const InputSnapshot& input, MechanicContext& ctx,
                          float dt) {
        auto& t = world.registry().get<CTransform>(
            static_cast<entt::entity>(avatar)).t;
        const Vec3 head = ctx.locomotion->head_position();
        const float ground = ctx.locomotion->body_position().y;

        // Rig turns (snap, smooth) carry the torso with them immediately.
        const float rig_yaw = ctx.locomotion->yaw_radians();
        body_yaw_ += wrap_pi(rig_yaw - last_rig_yaw_);
        last_rig_yaw_ = rig_yaw;

        // The torso follows the head's yaw through a dead zone, like a real
        // body: glancing sideways turns the head, turning further turns the
        // shoulders.
        if (input.head_valid) {
            const Vec3 f = input.head_rotation * Vec3(0.0f, 0.0f, -1.0f);
            if (glm::length(Vec3(f.x, 0.0f, f.z)) > 1e-3f) {
                const float head_yaw = yaw_of(f);
                const float diff = wrap_pi(head_yaw - body_yaw_);
                if (std::abs(diff) > yaw_deadzone_) {
                    body_yaw_ += diff - std::copysign(yaw_deadzone_, diff);
                }
                // While walking, the body settles back under the gaze.
                if (speed_ > 0.3f) {
                    body_yaw_ += wrap_pi(head_yaw - body_yaw_) *
                                 (1.0f - std::exp(-dt / 0.35f));
                }
            }
        }
        body_yaw_ = wrap_pi(body_yaw_);

        const float s = scale();
        const Quat rot =
            glm::angleAxis(body_yaw_, Vec3(0.0f, 1.0f, 0.0f)) * facing_;
        const Vec3 eye_offset = rot * (eye_model_ * s);
        Vec3 root(head.x - eye_offset.x, 0.0f, head.z - eye_offset.z);
        // Feet on the ground -- unless the player crouches below the
        // avatar's eye height, in which case the body sinks with them rather
        // than leaving the camera inside its chest.
        const float standing_root_y = ground - sole_y_model_ * s;
        const float head_root_y = head.y - eye_offset.y;
        root.y = std::min(standing_root_y, head_root_y);

        // Gait speed from where the BODY went, not the avatar root: the root
        // also swings whenever the torso turns about the eye point, and that
        // must not start a walk cycle while the player stands still.
        const Vec3 feet = ctx.locomotion->body_position();
        if (have_prev_root_ && dt > 1.0e-5f) {
            const Vec3 d(feet.x - prev_root_.x, 0.0f, feet.z - prev_root_.z);
            const float instant = glm::length(d) / dt;
            speed_ += (instant - speed_) * std::min(1.0f, dt / 0.08f);
        }
        prev_root_ = feet;
        have_prev_root_ = true;

        t.position = root;
        t.rotation = rot;
        t.scale = s;
    }

    /** Write ``joint`` so its WORLD rotation becomes ``want``. */
    void set_world_rotation(std::uint32_t avatar, int joint, const Quat& want,
                            MechanicContext& ctx) const {
        const int parent = ctx.skeleton->joint_parent(avatar, joint);
        const Quat parent_world =
            parent >= 0 ? rot_of(ctx.skeleton->joint_world(avatar, parent))
                        : rot_of(ctx.skeleton->joint_world(avatar, joint)) *
                              glm::inverse(ctx.skeleton->joint_local_rotation(
                                  avatar, joint));
        ctx.skeleton->set_joint_rotation(
            avatar, joint, glm::normalize(glm::inverse(parent_world) * want));
    }

    /** World rotation ``joint`` would have with its own local at bind. */
    Quat bind_world_rotation(std::uint32_t avatar, int joint,
                             MechanicContext& ctx) const {
        ctx.skeleton->set_joint_rotation_from_bind(avatar, joint,
                                                   Quat(1.0f, 0.0f, 0.0f, 0.0f));
        return rot_of(ctx.skeleton->joint_world(avatar, joint));
    }

    void drive_head(std::uint32_t avatar, const InputSnapshot& input,
                    MechanicContext& ctx) {
        if (head_ < 0 || !input.head_valid) return;
        // The headset's rotation relative to where the torso faces.
        const Quat body_yaw = glm::angleAxis(body_yaw_, Vec3(0.0f, 1.0f, 0.0f));
        const Quat relative =
            glm::normalize(input.head_rotation * glm::inverse(body_yaw));
        // Split it: some on the neck, the rest on the head, which is what
        // stops a 90-degree look reading as a broken neck.
        if (neck_ >= 0) {
            const Quat neck_bind = bind_world_rotation(avatar, neck_, ctx);
            set_world_rotation(
                avatar, neck_,
                glm::slerp(Quat(1, 0, 0, 0), relative, 0.35f) * neck_bind, ctx);
        }
        const Quat head_bind = bind_world_rotation(avatar, head_, ctx);
        // head_bind was measured with the neck already turned by 35%, so
        // the head adds only the remainder.
        const Quat remaining =
            neck_ >= 0 ? glm::slerp(Quat(1, 0, 0, 0), relative, 0.65f)
                       : relative;
        set_world_rotation(avatar, head_, remaining * head_bind, ctx);
    }

    void drive_arms(std::uint32_t avatar, const InputSnapshot& input,
                    MechanicContext& ctx) {
        const float s = scale();
        const Quat body_yaw = glm::angleAxis(body_yaw_, Vec3(0.0f, 1.0f, 0.0f));
        for (int h = 0; h < 2; ++h) {
            const auto& arm = arms_[static_cast<size_t>(h)];
            if (arm.upper < 0 || arm.fore < 0 || arm.hand < 0) continue;
            const auto& hand = input.hands[h];
            if (!hand.hand_valid) continue;
            if (arm.upper_len <= 1.0e-4f || arm.lower_len <= 1.0e-4f) continue;

            // Wrist target: the controller's grip, through this rig's palm
            // frame, so the hand sits in the controller the way a real one
            // does rather than the wrist sitting on it.
            const Quat wrist_rot =
                glm::normalize(hand.hand_rotation * arm.grip_to_wrist);
            const Vec3 wrist_target =
                hand.hand_position - wrist_rot * (arm.palm_offset_local * s);

            // Reset the chain to bind so this frame's solve starts clean.
            ctx.skeleton->set_joint_rotation_from_bind(avatar, arm.upper,
                                                       Quat(1, 0, 0, 0));
            ctx.skeleton->set_joint_rotation_from_bind(avatar, arm.fore,
                                                       Quat(1, 0, 0, 0));
            ctx.skeleton->set_joint_rotation_from_bind(avatar, arm.hand,
                                                       Quat(1, 0, 0, 0));

            const float upper_len = arm.upper_len * s;
            const float lower_len = arm.lower_len * s;

            // Light clavicle assist on a long reach, so a fully extended arm
            // pulls the shoulder forward instead of dislocating visually.
            if (arm.clavicle >= 0) {
                const Vec3 clav = Vec3(ctx.skeleton->joint_world(avatar, arm.clavicle)[3]);
                const Vec3 sh = Vec3(ctx.skeleton->joint_world(avatar, arm.upper)[3]);
                const float reach = glm::length(wrist_target - sh);
                const float t = std::clamp(
                    (reach - 0.85f * (upper_len + lower_len)) /
                        (0.3f * (upper_len + lower_len)),
                    0.0f, 1.0f);
                if (t > 0.0f) {
                    const Quat swing = two_bone_ik::rotation_between(
                        sh - clav, wrist_target - clav);
                    const Quat cw = rot_of(
                        ctx.skeleton->joint_world(avatar, arm.clavicle));
                    set_world_rotation(
                        avatar, arm.clavicle,
                        glm::slerp(Quat(1, 0, 0, 0), swing, 0.2f * t) * cw,
                        ctx);
                }
            }

            const Mat4 upper_w = ctx.skeleton->joint_world(avatar, arm.upper);
            const Vec3 shoulder(upper_w[3]);
            const float side = (h == 0) ? -1.0f : 1.0f;
            // Elbow bends down, a little back and out, in the body's frame.
            const Vec3 pole =
                body_yaw * glm::normalize(Vec3(0.25f * side, -1.0f, 0.45f));
            const auto sol = two_bone_ik::solve(shoulder, wrist_target,
                                                upper_len, lower_len, pole);
            if (!sol.valid) continue;

            // Upper arm: swing its current direction onto shoulder->elbow.
            {
                const Vec3 elbow_now(
                    ctx.skeleton->joint_world(avatar, arm.fore)[3]);
                const Quat swing = two_bone_ik::rotation_between(
                    elbow_now - shoulder, sol.elbow - shoulder);
                set_world_rotation(avatar, arm.upper,
                                   swing * rot_of(upper_w), ctx);
            }
            // Forearm: re-read the elbow where the new upper arm put it.
            {
                const Mat4 fore_w = ctx.skeleton->joint_world(avatar, arm.fore);
                const Vec3 elbow(fore_w[3]);
                const Vec3 wrist_now(
                    ctx.skeleton->joint_world(avatar, arm.hand)[3]);
                const Quat swing = two_bone_ik::rotation_between(
                    wrist_now - elbow, sol.wrist - elbow);
                set_world_rotation(avatar, arm.fore, swing * rot_of(fore_w),
                                   ctx);
            }
            // Wrist: exactly the controller's orientation.
            set_world_rotation(avatar, arm.hand, wrist_rot, ctx);
        }
    }

    /** With the avatar's own hands in use, any engine default hands left in
     *  the scene (an older scene that did not say ``hands.source: avatar``)
     *  would be a second pair on top of the body's. Collapse them. */
    void hide_engine_hands(MechanicContext& ctx) const {
        if (ctx.hands == nullptr || coverage_ == Coverage::HandsOnly) return;
        for (int h = 0; h < 2; ++h) {
            const std::uint32_t e = ctx.hands->hand_entity(h);
            if (e == InputSnapshot::kInvalidEntity) continue;
            if (!ctx.skeleton->has_skeleton(e)) continue;
            if (e == avatar_) continue;
            const int n = ctx.skeleton->joint_count(e);
            for (int j = 0; j < n; ++j) {
                ctx.skeleton->set_joint_scale(e, j, Vec3(kHiddenScale));
            }
        }
    }

    void drive_legs(World& world, std::uint32_t avatar, MechanicContext& ctx) {
        const float height = locomotion_clips::body_height(world, avatar) *
                             calibrated_scale_;
        clips_.update(world, avatar, speed_, /*sprint=*/false, height, tune_,
                      ctx);
    }

    /** Keep CSelfViewHide in agreement with the configured policy. */
    void apply_head_visibility(World& world, std::uint32_t avatar) {
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

    // --- configuration ---
    Coverage coverage_{Coverage::FullBody};
    HeadVisibility head_visibility_{HeadVisibility::HiddenInSelfView};
    bool leg_animation_{true};
    bool arm_ik_{true};
    bool head_tracking_{true};
    bool height_calibration_{true};
    float eye_height_offset_{0.0f};
    float yaw_deadzone_{0.70f};
    bool head_shown_{false};

    // --- measured from the rig ---
    bool resolved_{false};
    bool warned_no_skeleton_{false};
    std::uint32_t avatar_{InputSnapshot::kInvalidEntity};
    int neck_{-1};
    int head_{-1};
    std::vector<int> head_subtree_;
    std::array<Arm, 2> arms_{};
    Quat facing_{1.0f, 0.0f, 0.0f, 0.0f};
    Vec3 forward_model_{0.0f, 0.0f, 1.0f};
    Vec3 eye_model_{0.0f, 1.6f, 0.0f};
    float sole_y_model_{0.0f};
    float height_m_{1.75f};

    // --- per-frame state ---
    float base_scale_{1.0f};
    float calibrated_scale_{1.0f};
    bool calibration_done_{false};
    float calibration_wait_s_{0.0f};
    std::vector<float> calibration_samples_;
    float body_yaw_{0.0f};
    float last_rig_yaw_{0.0f};
    Vec3 prev_root_{0.0f};
    bool have_prev_root_{false};
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
