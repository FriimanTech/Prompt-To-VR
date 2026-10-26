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
#include "HeightCalibration.h"
#include "NeckDrift.h"
#include "ptv/api/BodySlots.h"
#include "ptv/api/Facing.h"
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

/** The legs start walking above this body speed and stop below the lower
 *  one (m/s), and the speed they read is smoothed over this long. A real
 *  stride is 0.5 m/s or more; leaning and shifting weight is not. */
constexpr float kLegsStartMps = 0.25f;
constexpr float kLegsStopMps = 0.12f;
constexpr float kGaitSpeedTau = 0.15f;

/** Shoulder-assist weight smoothing (s). */
constexpr float kShoulderAssistTau = 0.1f;
/** A controller that loses tracking is held where it was for this long,
 *  then the arm eases back to the clip over the second interval (s). */
constexpr float kHoldTrackingLossS = 0.3f;
constexpr float kReleaseToClipS = 0.25f;
/** An arm joint moving this far in one frame, in the body's own frame, is a
 *  glitch, not a motion (m); warn about the first few. */
constexpr float kJointJumpM = 0.25f;
constexpr int kMaxJumpWarnings = 5;

float wrap_pi(float a) {
    while (a > kPi) a -= 2.0f * kPi;
    while (a < -kPi) a += 2.0f * kPi;
    return a;
}

/** Yaw of a forward vector: R_y(yaw) * (0,0,-1) == forward. */
float yaw_of(const Vec3& fwd) { return yaw_from_forward(fwd); }

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
        // Unset, these come from the avatar's own measured clip speeds when
        // it is resolved (see resolve()).
        walk_speed_param_ = p.contains("walk_speed_mps");
        run_speed_param_ = p.contains("run_speed_mps");
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
        if (arm_ik_) drive_arms(world, avatar, input, ctx, dt);
        if (leg_animation_) drive_legs(world, avatar, ctx, dt);
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

        // --- per-frame state ---
        float assist_w{0.0f};  // smoothed shoulder-assist weight
        // Last valid controller pose, in the avatar root's frame, so a held
        // arm travels with the body while tracking is lost.
        Vec3 held_pos_local{0.0f};
        Quat held_rot_local{1.0f, 0.0f, 0.0f, 0.0f};
        bool have_held{false};
        float lost_s{0.0f};
        // Diagnostics: last frame's joint positions (root frame) and what
        // happened this second.
        std::array<Vec3, 3> prev_joint_local{};
        bool have_prev_joints{false};
        float reach_ratio{0.0f};
        float assist_deg{0.0f};
        int held_frames{0};
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
        calibration_.reset();

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
        // Rotation that turns the model's forward onto -Z. With a measured
        // model frame the loader has already done that (it is folded into
        // the root joints), so this comes out as identity and serves as a
        // cross-check of the pipeline's measurement.
        facing_ = glm::angleAxis(-yaw_of(fwd_model), Vec3(0.0f, 1.0f, 0.0f));
        forward_model_ = fwd_model;
        if (world.has<CMesh>(avatar) && world.get<CMesh>(avatar).has_model_frame) {
            const float residual_deg = std::abs(glm::degrees(yaw_of(fwd_model)));
            if (residual_deg > 10.0f) {
                PTV_WARN(
                    "avatar_body: shoulders say the framed avatar still faces "
                    "{:.0f} deg off -Z; asset_contract's forward measurement "
                    "and the rig disagree. Using the shoulders.",
                    residual_deg);
            }
        }

        // --- Gait speeds: the clips' own, unless the binding set them ------
        if (world.has<CMesh>(avatar)) {
            const auto& speeds = world.get<CMesh>(avatar).animation_speeds;
            const auto w = speeds.find("walk");
            const auto r = speeds.find("run");
            if (!walk_speed_param_ && w != speeds.end() && w->second > 0.05f) {
                tune_.walk_speed_mps = w->second;
            }
            if (!run_speed_param_ && r != speeds.end() &&
                r->second > tune_.walk_speed_mps) {
                tune_.run_speed_mps = r->second;
            }
        }

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

        // The neck, not the head: head tracking rotates the neck joint, which
        // moves the head joint but never the neck's own position, so the
        // neck reports only what the CLIP did to the torso.
        drift_joint_ = neck_ >= 0 ? neck_ : head_;
        drift_bind_model_ =
            drift_joint_ >= 0 ? bind_pos(ctx, avatar, drift_joint_) : Vec3(0.0f);
        drift_.reset();

        // The neck is the pivot a real head turns about. The eyes sit in
        // front of and above it, so turning or tilting the head swings the
        // headset through an arc while the neck -- and the body below it --
        // stays put. Expressed in the HEADSET's frame (-Z forward, +Y up,
        // +X right) so the arc can be undone with the headset's rotation.
        pivot_model_ = drift_joint_ >= 0 ? drift_bind_model_
                                         : Vec3(eye_model_.x, eye_model_.y -
                                                0.1f * height_m_, eye_model_.z);
        {
            const Vec3 v = eye_model_ - pivot_model_;
            const Vec3 up(0.0f, 1.0f, 0.0f);
            Vec3 right = glm::cross(fwd_model, up);
            if (glm::length(right) < 1e-4f) right = Vec3(1.0f, 0.0f, 0.0f);
            right = glm::normalize(right);
            eye_from_pivot_head_ =
                Vec3(glm::dot(v, right), v.y, -glm::dot(v, fwd_model));
        }
        have_prev_pivot_ = false;
        legs_moving_ = false;

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
        have_prev_pivot_ = false;
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

    /** Size the avatar to the player from their STANDING eye height, and
     *  again if they stand up taller later (see HeightCalibration.h). */
    void calibrate_height(World& world, std::uint32_t avatar,
                          const InputSnapshot& input, MechanicContext& ctx,
                          float dt) {
        (void)world;
        (void)avatar;
        if (!height_calibration_ || !input.head_valid) return;
        const float ground = ctx.locomotion->body_position().y;
        const float user_eye = ctx.locomotion->head_position().y - ground;
        const float avatar_eye = (eye_model_.y - sole_y_model_) * base_scale_;
        const auto ev = calibration_.update(user_eye, avatar_eye, dt);
        if (calibration_.calibrated()) calibrated_scale_ = calibration_.scale();
        using Ev = avatar_body::HeightCalibration::Event;
        if (ev != Ev::None) {
            PTV_INFO(
                "builtin.avatar_body: eye height {:.2f} m -> avatar scale "
                "{:.3f} ({})",
                calibration_.calibrated_eye(), calibration_.target_scale(),
                ev == Ev::First      ? "standing"
                : ev == Ev::Fallback ? "no standing pose in 10 s; using what "
                                       "was seen"
                                     : "player stood taller");
        }
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
        const float rig_turn = wrap_pi(rig_yaw - last_rig_yaw_);
        body_yaw_ += rig_turn;
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
                if (legs_moving_ && speed_ > 0.3f) {
                    body_yaw_ += wrap_pi(head_yaw - body_yaw_) *
                                 (1.0f - std::exp(-dt / 0.35f));
                }
            }
        }
        body_yaw_ = wrap_pi(body_yaw_);

        // Locomotion clips move the upper body: an idle can settle the neck
        // 10 cm forward of where it stands in the bind pose, a run leans it.
        // The eye point was measured in the bind pose, so follow the neck
        // the clip actually produced (last frame's evaluated pose), or the
        // camera ends up behind the avatar's own shoulders.
        if (drift_joint_ >= 0) {
            drift_.update(
                Vec3(ctx.skeleton->joint_model(avatar, drift_joint_)[3]),
                drift_bind_model_, dt);
        }

        const float s = scale();
        const Quat rot =
            glm::angleAxis(body_yaw_, Vec3(0.0f, 1.0f, 0.0f)) * facing_;

        // Where the neck is: the headset minus the eyes' offset from the
        // neck, turned with the head. Looking around moves the headset but
        // not this point, so the body stays still while the head turns.
        Vec3 pivot;
        if (input.head_valid) {
            pivot = head - input.head_rotation * (eye_from_pivot_head_ * s);
        } else {
            pivot = head - rot * ((eye_model_ - pivot_model_) * s);
        }
        const Vec3 neck_offset = rot * ((pivot_model_ + drift_.value()) * s);
        Vec3 root(pivot.x - neck_offset.x, 0.0f, pivot.z - neck_offset.z);
        // Feet on the ground -- unless the player crouches below the
        // avatar's neck height, in which case the body sinks with them rather
        // than leaving the camera inside its chest. Measured at the neck,
        // so looking down does not sink the body.
        const float standing_root_y = ground - sole_y_model_ * s;
        const float neck_root_y = pivot.y - neck_offset.y;
        root.y = std::min(standing_root_y, neck_root_y);

        // Gait speed from where the NECK went: a head turn does not move it,
        // and the stick, teleports and real walking all do. A snap turn
        // swings the neck about the head for one frame and a teleport jumps
        // it; neither is walking, so those frames are not sampled.
        const Vec3 d(pivot.x - prev_pivot_.x, 0.0f, pivot.z - prev_pivot_.z);
        const bool jumped = glm::length(d) > 0.5f || std::abs(rig_turn) > 1e-3f;
        body_jumped_ = jumped;
        if (have_prev_pivot_ && dt > 1.0e-5f && !jumped) {
            const float instant = glm::length(d) / dt;
            speed_ += (instant - speed_) *
                      (1.0f - std::exp(-dt / kGaitSpeedTau));
        }
        prev_pivot_ = pivot;
        have_prev_pivot_ = true;
        // Real bodies sway: a lean or a shuffle is not a step. Hysteresis so
        // the legs neither start on noise nor flicker at the boundary.
        legs_moving_ = legs_moving_ ? speed_ > kLegsStopMps
                                    : speed_ > kLegsStartMps;

        t.position = root;
        t.rotation = rot;
        t.scale = s;
    }

    static bool finite_quat(const Quat& q) {
        return std::isfinite(q.w) && std::isfinite(q.x) && std::isfinite(q.y) &&
               std::isfinite(q.z) && (q.w * q.w + q.x * q.x + q.y * q.y +
                                      q.z * q.z) > 1.0e-8f;
    }
    static bool finite_vec(const Vec3& v) {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    }

    /** Write ``joint`` so its WORLD rotation becomes ``want``. A non-finite
     *  answer is dropped: one NaN in a joint makes everything below it
     *  vanish from the frame. */
    void set_world_rotation(std::uint32_t avatar, int joint, const Quat& want,
                            MechanicContext& ctx) const {
        if (!finite_quat(want)) return;
        const int parent = ctx.skeleton->joint_parent(avatar, joint);
        const Quat parent_world =
            parent >= 0 ? rot_of(ctx.skeleton->joint_world(avatar, parent))
                        : rot_of(ctx.skeleton->joint_world(avatar, joint)) *
                              glm::inverse(ctx.skeleton->joint_local_rotation(
                                  avatar, joint));
        const Quat local = glm::normalize(glm::inverse(parent_world) * want);
        if (!finite_quat(local)) return;
        ctx.skeleton->set_joint_rotation(avatar, joint, local);
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

    void drive_arms(World& world, std::uint32_t avatar,
                    const InputSnapshot& input, MechanicContext& ctx,
                    float dt) {
        const float s = scale();
        const Quat body_yaw = glm::angleAxis(body_yaw_, Vec3(0.0f, 1.0f, 0.0f));
        // The avatar root, as plant_under_head just placed it: held poses
        // and the diagnostics are kept relative to it.
        const auto& rt = world.get<CTransform>(avatar).t;
        const Mat4 root = glm::translate(Mat4(1.0f), rt.position) *
                          glm::mat4_cast(rt.rotation);
        const Mat4 root_inv = glm::inverse(root);
        for (int h = 0; h < 2; ++h) {
            auto& arm = arms_[static_cast<size_t>(h)];
            // Set again only when this frame drives the arm to the end.
            const bool had_prev_joints = arm.have_prev_joints;
            arm.have_prev_joints = false;
            if (arm.upper < 0 || arm.fore < 0 || arm.hand < 0) continue;
            if (arm.upper_len <= 1.0e-4f || arm.lower_len <= 1.0e-4f) continue;
            const auto& hand = input.hands[h];

            const float upper_len = arm.upper_len * s;
            const float lower_len = arm.lower_len * s;

            // --- Where the hand should be --------------------------------
            // A tracked controller is the answer. A controller that drops
            // out for a moment is held where it was last seen (moving with
            // the body), then eased down to the side and handed back to
            // the clip -- never dropped for single frames, which reads as
            // the arm blinking out of existence.
            Vec3 hand_pos;
            Quat hand_rot;
            float release = 0.0f;  // 0 = full IK, 1 = hand back to the clip
            if (hand.hand_valid && finite_vec(hand.hand_position) &&
                finite_quat(hand.hand_rotation)) {
                hand_pos = hand.hand_position;
                hand_rot = glm::normalize(hand.hand_rotation);
                arm.held_pos_local = Vec3(root_inv * Vec4(hand_pos, 1.0f));
                arm.held_rot_local =
                    glm::normalize(glm::inverse(rt.rotation) * hand_rot);
                arm.have_held = true;
                arm.lost_s = 0.0f;
            } else {
                if (!arm.have_held) continue;
                arm.lost_s += dt;
                ++arm.held_frames;
                release = std::clamp((arm.lost_s - kHoldTrackingLossS) /
                                         kReleaseToClipS,
                                     0.0f, 1.0f);
                if (release >= 1.0f) continue;  // the clip owns the arm now
                hand_pos = Vec3(root * Vec4(arm.held_pos_local, 1.0f));
                hand_rot = glm::normalize(rt.rotation * arm.held_rot_local);
            }

            // Wrist target: the controller's grip, through this rig's palm
            // frame, so the hand sits in the controller the way a real one
            // does rather than the wrist sitting on it.
            const Quat wrist_rot = glm::normalize(hand_rot * arm.grip_to_wrist);
            Vec3 wrist_target =
                hand_pos - wrist_rot * (arm.palm_offset_local * s);
            if (!finite_vec(wrist_target)) continue;

            // Reset the WHOLE chain, clavicle included, to bind so this
            // frame's solve starts clean and depends on nothing left over
            // from the last one.
            ctx.skeleton->set_joint_rotation_from_bind(avatar, arm.upper,
                                                       Quat(1, 0, 0, 0));
            ctx.skeleton->set_joint_rotation_from_bind(avatar, arm.fore,
                                                       Quat(1, 0, 0, 0));
            ctx.skeleton->set_joint_rotation_from_bind(avatar, arm.hand,
                                                       Quat(1, 0, 0, 0));

            if (release > 0.0f) {
                // Ease towards an arm hanging at the side.
                const Vec3 sh(ctx.skeleton->joint_world(avatar, arm.upper)[3]);
                const Vec3 rest =
                    sh + Vec3(0.0f, -0.92f * (upper_len + lower_len), 0.0f) +
                    body_yaw * Vec3(0.0f, 0.0f, -0.05f);
                wrist_target = glm::mix(wrist_target, rest, release);
            }

            // Light shoulder assist on a long reach, so a fully extended arm
            // pulls the shoulder forward instead of dislocating visually.
            // Built from the clavicle's bind pose every frame (see
            // two_bone_ik::shoulder_assist for why it must be).
            if (arm.clavicle >= 0) {
                const Quat clav_bind =
                    bind_world_rotation(avatar, arm.clavicle, ctx);
                const Vec3 clav(ctx.skeleton->joint_world(avatar, arm.clavicle)[3]);
                const Vec3 sh(ctx.skeleton->joint_world(avatar, arm.upper)[3]);
                const float arm_len = upper_len + lower_len;
                const float reach = glm::length(wrist_target - sh);
                const float target_w =
                    two_bone_ik::shoulder_assist_weight(reach, arm_len);
                arm.assist_w += (target_w - arm.assist_w) *
                                (1.0f - std::exp(-std::max(0.0f, dt) /
                                                 kShoulderAssistTau));
                const Quat clav_world = two_bone_ik::shoulder_assist(
                    clav_bind, clav, sh, wrist_target, arm.assist_w);
                set_world_rotation(avatar, arm.clavicle, clav_world, ctx);
                arm.reach_ratio = arm_len > 1.0e-5f ? reach / arm_len : 0.0f;
                const Quat delta = clav_world * glm::inverse(clav_bind);
                arm.assist_deg = glm::degrees(
                    2.0f * std::acos(std::clamp(std::abs(delta.w), 0.0f, 1.0f)));
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

            // --- Diagnostics: an arm joint that leaps in one frame --------
            // Measured in the body's own frame, so walking, turning the
            // torso and sliding do not count; teleports and snap turns are
            // excluded outright.
            const int joints[3] = {arm.upper, arm.fore, arm.hand};
            std::array<Vec3, 3> now{};
            for (int k = 0; k < 3; ++k) {
                now[static_cast<size_t>(k)] = Vec3(
                    root_inv * ctx.skeleton->joint_world(avatar, joints[k])[3]);
            }
            if (had_prev_joints && !body_jumped_ &&
                jump_warnings_ < kMaxJumpWarnings) {
                for (int k = 0; k < 3; ++k) {
                    const float d = glm::length(
                        now[static_cast<size_t>(k)] -
                        arm.prev_joint_local[static_cast<size_t>(k)]);
                    if (d > kJointJumpM) {
                        static constexpr const char* kRole[3] = {
                            "upper arm", "forearm", "hand"};
                        ++jump_warnings_;
                        PTV_WARN(
                            "builtin.avatar_body: {} {} (joint {}) jumped "
                            "{:.2f} m in one frame (reach {:.2f} of arm, "
                            "shoulder assist {:.1f} deg, tracking {})",
                            h == 0 ? "left" : "right", kRole[k], joints[k], d,
                            arm.reach_ratio, arm.assist_deg,
                            hand.hand_valid ? "valid" : "held");
                        break;
                    }
                }
            }
            arm.prev_joint_local = now;
            arm.have_prev_joints = true;
        }

        // 1 Hz proof of what the arms are doing, for on-device logcat.
        arm_trace_accum_s_ += dt;
        if (arm_trace_accum_s_ >= 1.0f) {
            arm_trace_accum_s_ = 0.0f;
            const auto& l = arms_[0];
            const auto& r = arms_[1];
            PTV_INFO(
                "builtin.avatar_body arms L reach={:.2f} assist={:.1f}deg "
                "held={} | R reach={:.2f} assist={:.1f}deg held={}",
                l.reach_ratio, l.assist_deg, l.held_frames, r.reach_ratio,
                r.assist_deg, r.held_frames);
            arms_[0].held_frames = 0;
            arms_[1].held_frames = 0;
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

    void drive_legs(World& world, std::uint32_t avatar, MechanicContext& ctx,
                    float dt) {
        const float height = locomotion_clips::body_height(world, avatar) *
                             calibrated_scale_;
        // Running is the player's INTENT (holding the stick in), published
        // by the slide -- not a speed threshold the body happens to cross.
        const bool sprint = ctx.locomotion->sprinting();
        const float leg_speed = legs_moving_ ? speed_ : 0.0f;
        clips_.update(world, avatar, leg_speed, sprint && legs_moving_, height,
                      tune_, ctx);

        // 1 Hz proof of what the legs are doing, for on-device logcat.
        trace_accum_s_ += dt;
        if (trace_accum_s_ >= 1.0f && speed_ > locomotion_clips::kIdleSpeedEpsilon) {
            PTV_INFO("builtin.avatar_body gait={} speed={:.2f}m/s sprint={} "
                     "neck_drift={:.2f}m",
                     locomotion_clips::gait_intent(clips_.gait()), speed_,
                     sprint ? "yes" : "no", glm::length(drift_.value()));
            trace_accum_s_ = 0.0f;
        }
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
    int drift_joint_{-1};
    Vec3 drift_bind_model_{0.0f};
    // The neck in model space, and the eyes' offset from it in the
    // headset's own frame.
    Vec3 pivot_model_{0.0f, 1.45f, 0.0f};
    Vec3 eye_from_pivot_head_{0.0f, 0.15f, -0.08f};
    avatar_body::NeckDrift drift_{};
    float sole_y_model_{0.0f};
    float height_m_{1.75f};

    // --- per-frame state ---
    float base_scale_{1.0f};
    float calibrated_scale_{1.0f};
    avatar_body::HeightCalibration calibration_{};
    float body_yaw_{0.0f};
    float last_rig_yaw_{0.0f};
    Vec3 prev_pivot_{0.0f};
    bool have_prev_pivot_{false};
    bool legs_moving_{false};
    float speed_{0.0f};
    float trace_accum_s_{0.0f};
    float arm_trace_accum_s_{0.0f};
    bool body_jumped_{false};
    int jump_warnings_{0};
    bool walk_speed_param_{false};
    bool run_speed_param_{false};
    locomotion_clips::Tunables tune_{};
    locomotion_clips::Selector clips_;
};

}  // namespace

// Anchor so MSVC keeps this TU (and its registration) when linking
// ptv_runtime as a static library. Referenced by BuiltinMechanics.cpp.
extern "C" int ptv_force_link_builtin_avatar_body = 1;

PTV_REGISTER_MECHANIC("builtin.avatar_body", AvatarBodyMechanic)

}  // namespace ptv
