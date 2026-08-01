// builtin.locomotion.teleport -- aim-and-commit teleport locomotion.
//
// Push the left stick to raise a parabolic aim arc, release it to jump to
// wherever the arc landed. Instant translation produces no optical flow, so
// this is the most comfortable way to cover distance in VR and it is the
// default mode. See docs/locomotion.md.
//
// Input phase + Player scope: the rig must move before the frame's snapshot
// is frozen, and binding the mechanic to several entities must not give the
// player several teleports per flick.

#include <algorithm>
#include <cmath>

#include "../BuiltinMechanicBase.h"
#include "../StickFilter.h"
#include "ptv/core/Logger.h"
#include "TeleportArc.h"

namespace ptv {
namespace {

// Stick magnitude to raise the arc, and the (lower) magnitude that counts as
// having let go. The gap is deliberate hysteresis -- a single threshold makes
// the arc flicker while the stick crosses it.
constexpr float kAimThreshold = 0.5f;
constexpr float kReleaseThreshold = 0.1f;

// Touch controllers' grip handle is angled upward, so the AIM pose is used
// for direction and then biased slightly down. Without this, a natural wrist
// angle throws the arc into the distance instead of the comfortable 3-6m
// range players expect.
constexpr float kDownwardAimBias = 0.15f;

const Vec4 kArcValid{0.30f, 0.75f, 1.00f, 1.0f};
const Vec4 kArcInvalid{0.60f, 0.30f, 0.30f, 1.0f};
const Vec4 kLandingDisc{0.30f, 0.85f, 1.00f, 1.0f};
constexpr float kLandingDiscRadius = 0.4f;
constexpr float kArcThickness = 0.03f;

class TeleportMechanic : public BuiltinMechanicBase<TeleportMechanic> {
public:
    static constexpr const char* kId = "builtin.locomotion.teleport";

    MechanicPhase phase() const override { return MechanicPhase::Input; }
    MechanicScope scope() const override { return MechanicScope::Player; }

    void configure(const nlohmann::json& p) {
        // The engine catalog documents "max_horizontal_m" but the
        // hand-authored starter room passes "max_distance_m". Accept both
        // rather than silently ignoring one of them.
        if (p.contains("max_horizontal_m") && p["max_horizontal_m"].is_number()) {
            max_horizontal_override_ = p.value("max_horizontal_m", 0.0f);
        } else if (p.contains("max_distance_m") && p["max_distance_m"].is_number()) {
            max_horizontal_override_ = p.value("max_distance_m", 0.0f);
        }
        if (p.contains("launch_speed_mps") && p["launch_speed_mps"].is_number()) {
            launch_speed_override_ = p.value("launch_speed_mps", 0.0f);
        }
        if (p.contains("hand") && p["hand"].is_string()) {
            const std::string hand = p.value("hand", std::string("left"));
            hand_ = (hand == "right") ? InputSnapshot::Right : InputSnapshot::Left;
        }
    }

    void tick(World&, std::uint32_t, float, const InputSnapshot& input,
              MechanicContext& ctx) {
        if (ctx.locomotion == nullptr) return;
        // Exactly one locomotion style owns the stick at a time.
        if (ctx.locomotion->active_mode() != ::ptv::LocomotionMode::Teleport ||
            ctx.locomotion->input_locked()) {
            reset();
            return;
        }

        const auto& hand = input.hands[hand_];
        // Thresholds below are in terms of the FILTERED magnitude, matching
        // slide -- see StickFilter.h.
        const float mag = stick::filter(hand.thumbstick).magnitude;

        // Only the frames that actually re-solve the arc draw it. Between the
        // two thresholds the player is neither aiming nor committing, and a
        // stale arc left hanging in the world would be worse than none.
        bool solved_this_frame = false;

        if (hand.hand_valid && hand.aim_valid && mag > kAimThreshold) {
            const auto& tun = ctx.locomotion->tunables();
            teleport::ArcParams p;
            p.launch_speed_mps = launch_speed_override_ > 0.0f
                                     ? launch_speed_override_
                                     : tun.teleport_launch_speed_mps;
            p.floor_y = tun.teleport_floor_y;
            p.max_horizontal_m = max_horizontal_override_ > 0.0f
                                     ? max_horizontal_override_
                                     : tun.teleport_max_horizontal_m;

            Vec3 forward = hand.aim_rotation * Vec3(0.0f, 0.0f, -1.0f);
            forward.y -= kDownwardAimBias;
            forward = glm::normalize(forward);

            teleport::solve_arc(hand.hand_position, forward, p, ctx.physics,
                                arc_);
            aiming_ = true;
            solved_this_frame = true;
        } else if (aiming_ && mag < kReleaseThreshold) {
            // Release edge: commit to the last valid landing spot.
            if (arc_.valid) {
                ctx.locomotion->teleport_to(arc_.target);
                PTV_INFO("Teleport -> ({:.2f}, {:.2f}, {:.2f})", arc_.target.x,
                         arc_.target.y, arc_.target.z);
            }
            reset();
        }

        // Draw the preview. IDebugDraw geometry lives for one frame, so this
        // has to be re-submitted every frame it should be visible.
        if (solved_this_frame && ctx.draw != nullptr && arc_.points.size() >= 2) {
            ctx.draw->polyline(arc_.points.data(), arc_.points.size(),
                               arc_.valid ? kArcValid : kArcInvalid,
                               kArcThickness);
            if (arc_.valid) {
                ctx.draw->marker(arc_.target + Vec3(0.0f, 0.01f, 0.0f),
                                 kLandingDiscRadius, kLandingDisc);
            }
        }
    }

private:
    void reset() {
        aiming_ = false;
        arc_.valid = false;
        arc_.points.clear();
    }

    teleport::ArcResult arc_{};
    bool aiming_{false};
    // <= 0 means "not overridden by binding params; use the scene tunable".
    float max_horizontal_override_{0.0f};
    float launch_speed_override_{0.0f};
    int hand_{InputSnapshot::Left};
};

}  // namespace

extern "C" int ptv_force_link_builtin_teleport = 1;

PTV_REGISTER_MECHANIC("builtin.locomotion.teleport", TeleportMechanic)
// Legacy/alias id: the hand-authored starter room binds "builtin.teleport".
PTV_REGISTER_MECHANIC("builtin.teleport", TeleportMechanic)

}  // namespace ptv
