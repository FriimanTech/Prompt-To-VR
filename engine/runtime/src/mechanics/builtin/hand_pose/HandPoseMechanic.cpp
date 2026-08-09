// builtin.hand_pose -- curl the player's fingers from the controller.
//
// This is the payoff for the skinning tier. The player's hands are rigged
// entities (16 joints each, created by Application::create_hand_entities from
// the generated hand mesh), and this mechanic states their finger pose every
// frame as a pure function of the CURRENT input:
//
//   index finger        <- trigger
//   middle/ring/pinky   <- grip
//   thumb               <- thumbrest / face-button touch
//
// That is why it is an override rather than a clip. There is no recording of
// "the player pulled the trigger 40% of the way"; the pose is whatever the
// analog value is this instant. Clips are for motion baked into an asset -- a
// walk cycle -- and both write into the same CSkeleton::local_pose, so a
// character could play an idle animation while code steers its fingers.
//
// Capsense is what makes this read as a hand rather than a claw. A Touch
// controller reports whether a finger is RESTING on the trigger separately
// from how hard it is pulled, so a finger that is touching but not pressing
// gets a slight curl instead of sticking out straight. Without that,
// every idle hand looks like it is pointing.
//
// Player-scoped: one instance drives both hands. Simulate phase, because the
// pose system runs immediately after it -- an override written in any other
// phase is a frame late or ignored entirely.
//
// Deliberately NOT coupled to whatever the hand is holding. A weapon that
// reacts to the trigger reads the same input.hands[h].trigger scalar, so the
// two stay in sync every frame with no event plumbing and no ordering
// dependency between the mechanics.

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

#include <glm/gtc/quaternion.hpp>

#include "../BuiltinMechanicBase.h"
#include "ptv/core/Logger.h"

namespace ptv {
namespace {

// Joint naming from engine/tools/make_hand_fixture.py, which uses the
// ``mixamorig:`` prefix so it matches the spec="mixamo" convention the
// auto-rigging stage requests for characters.
constexpr const char* kBonePrefix = "mixamorig:";

constexpr int kFingerCount = 5;
constexpr int kSegmentsPerFinger = 3;

// Index 0 is the thumb; 1 is the index finger; 2..4 are the grip fingers.
const std::array<const char*, kFingerCount> kFingerNames = {
    "thumb", "index", "middle", "ring", "pinky",
};

// How far each segment bends at full curl, in radians. Distal joints bend
// further than proximal ones, which is what makes a fist look like a fist
// rather than three equal hinges.
constexpr float kCurlProximal = 1.15f;
constexpr float kCurlIntermediate = 1.45f;
constexpr float kCurlDistal = 1.20f;

// A finger resting on its control without pressing sits slightly bent.
constexpr float kRestingCurl = 0.18f;

// The thumb folds across the palm rather than curling into it, so it gets its
// own smaller range about a different axis.
constexpr float kThumbCurl = 0.75f;

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

class HandPoseMechanic : public BuiltinMechanicBase<HandPoseMechanic> {
public:
    static constexpr const char* kId = "builtin.hand_pose";

    MechanicPhase phase() const override { return MechanicPhase::Simulate; }
    MechanicScope scope() const override { return MechanicScope::Player; }

    void configure(const nlohmann::json& p) {
        smoothing_tau_ = std::clamp(
            p.value("smoothing_tau_s", kDefaultSmoothingTau), 0.0f, 0.5f);
        // A held object should read as gripped even if the player relaxes the
        // squeeze, which they will, because the controller does the holding.
        hold_curl_ = std::clamp(p.value("hold_curl", 0.85f), 0.0f, 1.0f);
        resolved_ = false;
    }

    void tick(World&, std::uint32_t, float dt, const InputSnapshot& input,
              MechanicContext& ctx) {
        if (ctx.skeleton == nullptr || ctx.hands == nullptr) return;
        if (!resolved_) resolve_joints(ctx);

        for (int h = 0; h < 2; ++h) {
            const std::uint32_t hand_entity = ctx.hands->hand_entity(h);
            if (hand_entity == InputSnapshot::kInvalidEntity) continue;

            const auto& hand = input.hands[h];
            const auto hand_enum = static_cast<InputSnapshot::Hand>(h);

            // --- target curl per finger group -------------------------
            // Touching without pressing gives a resting bend; the analog
            // value takes over the moment it exceeds that.
            float index_target = hand.trigger;
            if (input.trigger_touching(hand_enum)) {
                index_target = std::max(index_target, kRestingCurl);
            }
            float grip_target = hand.grip;

            // Holding something pins the fingers closed regardless of how
            // lightly the player is now squeezing.
            if (hand.held_entity != InputSnapshot::kInvalidEntity) {
                grip_target = std::max(grip_target, hold_curl_);
                index_target = std::max(index_target, kRestingCurl);
            }

            // The thumb has no analog axis at all -- capsense is the only
            // signal there is. Resting it on the thumbrest or a face button
            // folds it down over the palm.
            const bool thumb_down =
                input.thumbrest_touching(hand_enum) ||
                (h == 0 ? (input.button_x || input.button_y)
                        : (input.button_a || input.button_b));
            const float thumb_target = thumb_down ? 1.0f : 0.0f;

            auto& state = smoothed_[static_cast<size_t>(h)];
            state.index = approach(state.index, index_target, smoothing_tau_, dt);
            state.grip = approach(state.grip, grip_target, smoothing_tau_, dt);
            state.thumb = approach(state.thumb, thumb_target, smoothing_tau_, dt);

            apply(ctx, hand_entity, h, state);
        }
    }

private:
    struct Curl {
        float index{0.0f};
        float grip{0.0f};
        float thumb{0.0f};
    };

    void resolve_joints(MechanicContext& ctx) {
        // Resolved once, then cached: joint_index is a string lookup, and
        // doing it for 15 joints on both hands every frame would be pure
        // waste.
        //
        // Only latch ``resolved_`` once a hand actually answered. Bindings are
        // attached before the renderer exists, so the very first attempt can
        // legitimately find no hand entity yet; latching then would leave the
        // fingers frozen for the whole session.
        int found = 0;
        for (int h = 0; h < 2; ++h) {
            const std::uint32_t e = ctx.hands->hand_entity(h);
            if (e == InputSnapshot::kInvalidEntity) continue;
            for (int f = 0; f < kFingerCount; ++f) {
                for (int s = 0; s < kSegmentsPerFinger; ++s) {
                    const std::string name = std::string(kBonePrefix) +
                                             kFingerNames[static_cast<size_t>(f)] +
                                             "_0" + std::to_string(s + 1);
                    const int j = ctx.skeleton->joint_index(e, name.c_str());
                    joints_[static_cast<size_t>(h)][static_cast<size_t>(f)]
                           [static_cast<size_t>(s)] = j;
                    if (j >= 0) ++found;
                }
            }
            resolved_ = true;
        }
        if (resolved_ && found == 0) {
            PTV_WARN(
                "builtin.hand_pose found no finger joints; the hand asset's "
                "bones are not named '{}<finger>_0N'",
                kBonePrefix);
        }
    }

    void apply(MechanicContext& ctx, std::uint32_t entity, int hand,
               const Curl& curl) const {
        for (int f = 0; f < kFingerCount; ++f) {
            const bool is_thumb = (f == 0);
            const bool is_index = (f == 1);
            const float amount = is_thumb ? curl.thumb
                                          : (is_index ? curl.index : curl.grip);

            for (int s = 0; s < kSegmentsPerFinger; ++s) {
                const int joint =
                    joints_[static_cast<size_t>(hand)][static_cast<size_t>(f)]
                           [static_cast<size_t>(s)];
                if (joint < 0) continue;

                // Fingers close around +X (the palm normal in the OpenXR grip
                // frame), so they hinge about X. The thumb folds ACROSS the
                // palm instead, which is a rotation about Y.
                const float angle =
                    amount * (is_thumb ? kThumbCurl : curl_for_segment(s));
                const Vec3 axis = is_thumb ? Vec3(0.0f, 1.0f, 0.0f)
                                           : Vec3(1.0f, 0.0f, 0.0f);
                // Mirror the bend direction so the left hand closes inward
                // too rather than bending backwards.
                const float sign = (hand == 0) ? -1.0f : 1.0f;
                ctx.skeleton->set_joint_rotation(
                    entity, joint, glm::angleAxis(angle * sign, axis));
            }
        }
    }

    float smoothing_tau_{kDefaultSmoothingTau};
    float hold_curl_{0.85f};
    bool resolved_{false};
    std::array<Curl, 2> smoothed_{};
    // [hand][finger][segment] -> joint index, or -1.
        int joints_[2][kFingerCount][kSegmentsPerFinger]{};

};

}  // namespace

// MSVC drops an .obj whose only content is a static-initializer registration
// unless something references it; BuiltinMechanics.cpp sums these anchors.
extern "C" int ptv_force_link_builtin_hand_pose = 1;

PTV_REGISTER_MECHANIC("builtin.hand_pose", HandPoseMechanic)

}  // namespace ptv
