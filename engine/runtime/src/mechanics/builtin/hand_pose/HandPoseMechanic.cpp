// builtin.hand_pose -- curl the player's fingers from the controller.
//
// States the finger pose every frame as a pure function of the CURRENT input:
//
//   nothing touched         -> relaxed, open hand
//   trigger                 -> index finger curls
//   grip                    -> middle, ring and little fingers close into
//                              the palm
//   grip + trigger          -> full fist
//   thumb on the thumbstick -> thumb lowers toward the index side, as if
//                              resting on the stick
//   finger resting on the trigger without pulling -> slight index bend
//   holding something       -> grip fingers stay closed around it
//
// WHICH hand gets posed is not this mechanic's decision. It curls exactly
// ``ctx.hands->finger_target(h)`` -- the skeleton whose fingers the player
// actually sees on that hand:
//
//   * Hands Only: the engine's default rigged hands.
//   * Full Player Avatar: the avatar itself, pointed at by
//     builtin.avatar_body once it has found finger bones on that side.
//   * An avatar whose rig stops at the wrist: nothing. Its hands follow the
//     controllers; there are no fingers to bend.
//
// The previous version curled the engine's own hand models only, so on a
// full-body avatar -- where those models are hidden -- the visible fingers
// never moved. Driving whatever is visible, and nothing else, is the fix.
//
// HOW the joints bend comes from HandRig, measured from the rig's own rest
// pose, and every curl is applied on top of the joint's REST rotation. The
// old code replaced each joint's rotation outright with a hard-coded
// axis-angle, which only matched the procedural hands it was written for;
// on a real rig it threw the rest pose away, so fingers bent backwards,
// sideways, or into the wrong hand.
//
// Player-scoped: one instance drives both hands. Simulate phase, after
// builtin.avatar_body (which places the wrist this curl sits on), because the
// pose system runs immediately after that phase.

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

#include <glm/gtc/quaternion.hpp>

#include "../BuiltinMechanicBase.h"
#include "../HandRig.h"
#include "ptv/core/Logger.h"

namespace ptv {
namespace {

// How far each segment bends at full curl, in radians. Distal joints bend
// further than proximal ones, which is what makes a fist look like a fist
// rather than three equal hinges.
constexpr float kCurlProximal = 1.35f;
constexpr float kCurlIntermediate = 1.55f;
constexpr float kCurlDistal = 1.15f;

// A finger resting on its control without pressing sits slightly bent.
constexpr float kRestingCurl = 0.18f;
// The relaxed open hand is not flat: fingers keep a gentle natural bend.
constexpr float kDefaultRelaxedCurl = 0.08f;
// Index finger resting on a trigger it is not pulling.
constexpr float kTriggerRestCurl = 0.3f;
// All four fingers around a hilt or strap.
constexpr float kWrapCurl = 0.92f;
// A support hand around a handguard or the shooting hand.
constexpr float kSupportCurl = 0.75f;

// The thumb resting on the stick: swings toward the index finger and bends a
// little at each joint. Deliberately modest -- it rests on the side of the
// index, it does not fold across the palm.
constexpr float kThumbAdduct = 0.45f;
constexpr float kThumbFlex[3] = {0.15f, 0.35f, 0.40f};

// Exponential smoothing time constant, matching the convention the locomotion
// built-ins use. Fingers reach ~95% of a new target in ~3 tau; at 45 ms that
// is about a seventh of a second, fast enough to feel direct and slow enough
// that controller jitter does not buzz the mesh.
constexpr float kDefaultSmoothingTau = 0.045f;

float curl_for_segment(int segment) {
    switch (segment) {
        case 0: return kCurlProximal;
        case 1: return kCurlIntermediate;
        default: return kCurlDistal;
    }
}

/** Frame-rate independent exponential approach, same form as
 *  slide::ComfortRamp: a value reaches its target over the same wall-clock
 *  time at 72 Hz and 90 Hz. */
float approach(float current, float target, float tau, float dt) {
    if (tau <= 1e-4f) return target;
    const float k = 1.0f - std::exp(-dt / tau);
    return current + (target - current) * k;
}

enum ThumbSource : unsigned {
    kThumbstick = 1u << 0,
    kThumbrest = 1u << 1,
    kFaceButtons = 1u << 2,
};

class HandPoseMechanic : public BuiltinMechanicBase<HandPoseMechanic> {
public:
    static constexpr const char* kId = "builtin.hand_pose";

    MechanicPhase phase() const override { return MechanicPhase::Simulate; }
    MechanicScope scope() const override { return MechanicScope::Player; }
    // After builtin.avatar_body, which places the wrist these curls sit on
    // and points the finger targets at the avatar.
    int order() const override { return 10; }

    void configure(const nlohmann::json& p) {
        smoothing_tau_ = std::clamp(
            p.value("smoothing_tau_s", kDefaultSmoothingTau), 0.0f, 0.5f);
        // A held object should read as gripped even if the player relaxes the
        // squeeze, which they will, because the controller does the holding.
        hold_curl_ = std::clamp(p.value("hold_curl", 0.85f), 0.0f, 1.0f);
        relaxed_curl_ =
            std::clamp(p.value("relaxed_curl", kDefaultRelaxedCurl), 0.0f, 0.4f);

        // Which capsense lowers the thumb. The thumbstick by default: that is
        // where a Touch player's thumb rests nearly all the time. A thumbrest
        // or face-button trigger only when a prompt asks for it.
        thumb_sources_ = kThumbstick;
        if (p.contains("thumb_sources") && p["thumb_sources"].is_array()) {
            thumb_sources_ = 0u;
            for (const auto& s : p["thumb_sources"]) {
                if (!s.is_string()) continue;
                const std::string v = s.get<std::string>();
                if (v == "thumbstick") thumb_sources_ |= kThumbstick;
                if (v == "thumbrest") thumb_sources_ |= kThumbrest;
                if (v == "face_buttons") thumb_sources_ |= kFaceButtons;
            }
        }
        for (auto& r : rigs_) r = Resolved{};
    }

    void tick(World& world, std::uint32_t, float dt, const InputSnapshot& input,
              MechanicContext& ctx) {
        if (ctx.skeleton == nullptr || ctx.hands == nullptr) return;

        for (int h = 0; h < 2; ++h) {
            const std::uint32_t target = ctx.hands->finger_target(h);
            auto& res = rigs_[static_cast<size_t>(h)];
            if (target == InputSnapshot::kInvalidEntity ||
                !ctx.skeleton->has_skeleton(target)) {
                res = Resolved{};
                continue;
            }
            // Re-measured whenever the target changes -- a skeleton attaches
            // after load, and an avatar can be swapped mid-game. A one-shot
            // latch is what used to freeze fingers for a whole session.
            if (res.entity != target) {
                res.entity = target;
                res.rig = hand_rig::analyze(*ctx.skeleton, target, h);
                if (!res.rig.valid) {
                    PTV_INFO(
                        "builtin.hand_pose: {} hand target has no finger "
                        "bones; fingers will not animate",
                        h == 0 ? "left" : "right");
                }
            }
            if (!res.rig.valid) continue;

            const auto& hand = input.hands[h];
            const auto hand_enum = static_cast<InputSnapshot::Hand>(h);

            // --- target curl per finger group -------------------------
            float index_target = std::max(relaxed_curl_, hand.trigger);
            if (input.trigger_touching(hand_enum)) {
                index_target = std::max(index_target, kRestingCurl);
            }
            float grip_target = std::max(relaxed_curl_, hand.grip);
            // Holding something pins the fingers closed regardless of how
            // lightly the player is now squeezing. The item's hold profile
            // says HOW: a gun keeps the index finger on the trigger (and
            // follows it), a hilt or strap is wrapped by all four fingers.
            const std::uint32_t support = ctx.hands->support_entity(h);
            if (hand.held_entity != InputSnapshot::kInvalidEntity) {
                grip_target = std::max(grip_target, hold_curl_);
                index_target = std::max(index_target, kRestingCurl);
                const auto* hp = world.registry().try_get<CHoldProfile>(
                    static_cast<entt::entity>(hand.held_entity));
                if (hp != nullptr) {
                    switch (hp->finger_pose) {
                        case HoldFingerPose::Trigger:
                            index_target = std::max(kTriggerRestCurl, hand.trigger);
                            break;
                        case HoldFingerPose::Wrap:
                        case HoldFingerPose::Strap:
                            grip_target = std::max(grip_target, kWrapCurl);
                            index_target = std::max(index_target, kWrapCurl);
                            break;
                        case HoldFingerPose::Pinch:
                            grip_target = std::min(grip_target, 0.45f);
                            index_target = std::max(index_target, 0.7f);
                            break;
                        case HoldFingerPose::OpenSupport:
                            grip_target = std::min(grip_target, 0.6f);
                            index_target = std::max(index_target, 0.5f);
                            break;
                    }
                }
            } else if (support != InputSnapshot::kInvalidEntity) {
                // The second hand cups the handguard / wraps the first hand.
                grip_target = std::max(grip_target, kSupportCurl);
                index_target = std::max(index_target, kSupportCurl);
            }
            bool thumb_down = false;
            if ((thumb_sources_ & kThumbstick) &&
                input.thumbstick_touching(hand_enum)) {
                thumb_down = true;
            }
            if ((thumb_sources_ & kThumbrest) &&
                input.thumbrest_touching(hand_enum)) {
                thumb_down = true;
            }
            if ((thumb_sources_ & kFaceButtons) &&
                input.face_button_touching(hand_enum)) {
                thumb_down = true;
            }
            const float thumb_target = thumb_down ? 1.0f : 0.0f;

            auto& state = smoothed_[static_cast<size_t>(h)];
            state.index = approach(state.index, index_target, smoothing_tau_, dt);
            state.grip = approach(state.grip, grip_target, smoothing_tau_, dt);
            state.thumb = approach(state.thumb, thumb_target, smoothing_tau_, dt);

            apply(ctx, target, res.rig, state);
        }
    }

private:
    struct Curl {
        float index{0.0f};
        float grip{0.0f};
        float thumb{0.0f};
    };

    struct Resolved {
        std::uint32_t entity{InputSnapshot::kInvalidEntity};
        hand_rig::Rig rig{};
    };

    void apply(MechanicContext& ctx, std::uint32_t entity,
               const hand_rig::Rig& rig, const Curl& curl) const {
        for (int f = hand_rig::Index; f <= hand_rig::Pinky; ++f) {
            const float amount = (f == hand_rig::Index) ? curl.index : curl.grip;
            const auto& chain = rig.fingers[static_cast<size_t>(f)];
            for (int s = 0; s < hand_rig::kSegments; ++s) {
                const auto& seg = chain.segments[static_cast<size_t>(s)];
                if (seg.joint < 0) continue;
                ctx.skeleton->set_joint_rotation_from_bind(
                    entity, seg.joint,
                    glm::angleAxis(amount * curl_for_segment(s),
                                   seg.curl_axis_local));
            }
        }

        const auto& thumb = rig.fingers[hand_rig::Thumb];
        for (int s = 0; s < hand_rig::kSegments; ++s) {
            const auto& seg = thumb.segments[static_cast<size_t>(s)];
            if (seg.joint < 0) continue;
            Quat q = glm::angleAxis(curl.thumb * kThumbFlex[s],
                                    seg.curl_axis_local);
            if (s == 0) {
                // The base joint also swings toward the index finger, which
                // is what reads as "resting on the stick".
                q = glm::angleAxis(curl.thumb * kThumbAdduct,
                                   rig.thumb_adduct_axis_local) *
                    q;
            }
            ctx.skeleton->set_joint_rotation_from_bind(entity, seg.joint, q);
        }
    }

    float smoothing_tau_{kDefaultSmoothingTau};
    float hold_curl_{0.85f};
    float relaxed_curl_{kDefaultRelaxedCurl};
    unsigned thumb_sources_{kThumbstick};
    std::array<Curl, 2> smoothed_{};
    std::array<Resolved, 2> rigs_{};
};

}  // namespace

// MSVC drops an .obj whose only content is a static-initializer registration
// unless something references it; BuiltinMechanics.cpp sums these anchors.
extern "C" int ptv_force_link_builtin_hand_pose = 1;

PTV_REGISTER_MECHANIC("builtin.hand_pose", HandPoseMechanic)

}  // namespace ptv
