// builtin.locomotion.teleport -- aim-and-commit teleport locomotion.
//
// Push the left stick FORWARD to raise a parabolic aim arc, release it to
// jump to wherever the arc landed. Instant translation produces no optical
// flow, so this is the most comfortable way to cover distance in VR. See
// docs/locomotion.md.
//
// Three rules the on-device playtests settled:
//
//   * Only a FORWARD push aims. A sideways or backward push does nothing,
//     which is what players expect from every Quest title and what stops a
//     casual stick nudge from throwing an arc.
//   * The controller's pitch sets the distance: tilt up to reach farther,
//     tilt down to land closer. Monotonic across the whole range, so there
//     is no point where raising the controller pulls the landing spot back.
//   * No fade, no blink, no vignette. Teleport is already the comfort
//     option; darkening the screen on every hop reads as a flicker.
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

// Forward push that raises the arc, and the (lower) magnitude that counts as
// having let go. The gap is deliberate hysteresis -- a single threshold makes
// the arc flicker while the stick crosses it.
constexpr float kAimThreshold = 0.5f;
constexpr float kReleaseThreshold = 0.1f;
// How far off straight-forward the push may be and still aim.
constexpr float kForwardConeRad = 0.61f;  // 35 degrees

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
        // The engine catalog documents "max_horizontal_m"; the hand-authored
        // starter room passes "max_distance_m"; "max_range_m" is the newer
        // name. Accept all three rather than silently ignoring one.
        for (const char* key : {"max_range_m", "max_horizontal_m",
                                "max_distance_m"}) {
            if (p.contains(key) && p[key].is_number()) {
                max_horizontal_override_ = p.value(key, 0.0f);
                break;
            }
        }
        min_range_m_ = std::clamp(p.value("min_range_m", 1.0f), 0.3f, 5.0f);
        if (p.contains("hand") && p["hand"].is_string()) {
            const std::string hand = p.value("hand", std::string("left"));
            hand_ = (hand == "right") ? InputSnapshot::Right : InputSnapshot::Left;
        }
    }

    void tick(World&, std::uint32_t, float, const InputSnapshot& input,
              MechanicContext& ctx) {
        if (ctx.locomotion == nullptr) return;
        if (!declared_) {
            ctx.locomotion->declare_mode_available(
                ::ptv::LocomotionMode::Teleport);
            declared_ = true;
        }
        // Exactly one locomotion style owns the stick at a time.
        if (ctx.locomotion->active_mode() != ::ptv::LocomotionMode::Teleport ||
            ctx.locomotion->input_locked()) {
            reset();
            return;
        }

        const auto& hand = input.hands[hand_];
        // Thresholds are in terms of the FILTERED stick, matching slide --
        // see StickFilter.h.
        const auto filtered = stick::filter(hand.thumbstick);
        const float mag = filtered.magnitude;
        const Vec2 raw = hand.thumbstick;
        const float angle_off_forward =
            std::atan2(std::abs(raw.x), std::max(1e-4f, raw.y));
        const bool pushing_forward = raw.y > 0.0f && mag > kAimThreshold &&
                                     angle_off_forward <= kForwardConeRad;

        // Only the frames that actually re-solve the arc draw it. Between the
        // two thresholds the player is neither aiming nor committing, and a
        // stale arc left hanging in the world would be worse than none.
        bool solved_this_frame = false;

        // Once aiming, keep aiming while the stick is held at all, so a thumb
        // drifting sideways mid-aim does not cancel the arc.
        const bool keep_aiming = aiming_ && mag >= kReleaseThreshold;
        if (hand.hand_valid && hand.aim_valid &&
            (pushing_forward || keep_aiming)) {
            const auto& tun = ctx.locomotion->tunables();
            teleport::ArcParams p;
            p.floor_y = tun.teleport_floor_y;
            p.max_horizontal_m = max_horizontal_override_ > 0.0f
                                     ? max_horizontal_override_
                                     : tun.teleport_max_horizontal_m;
            if (ctx.hands != nullptr) {
                p.ignore_entities.push_back(ctx.hands->player_entity());
                for (int h = 0; h < 2; ++h) {
                    p.ignore_entities.push_back(input.hands[h].held_entity);
                }
            }

            // Aim along the controller body (/aim pose), clamped in pitch.
            const Vec3 aim = hand.aim_rotation * Vec3(0.0f, 0.0f, -1.0f);
            Vec3 flat(aim.x, 0.0f, aim.z);
            const float flat_len = glm::length(flat);
            if (flat_len > 1e-4f) {
                flat /= flat_len;
                const float pitch =
                    std::clamp(std::atan2(aim.y, flat_len),
                               teleport::kMinAimPitch, teleport::kMaxAimPitch);
                const float height =
                    std::max(0.1f, hand.aim_position.y - p.floor_y);
                const float distance = teleport::reachable_distance(
                    pitch,
                    teleport::distance_for_pitch(pitch, min_range_m_,
                                                 p.max_horizontal_m),
                    height);
                p.launch_speed_mps = teleport::launch_speed_for(
                    pitch, distance, height, p.gravity_mps2);
                const Vec3 dir = glm::normalize(
                    flat * std::cos(pitch) +
                    Vec3(0.0f, std::sin(pitch), 0.0f));
                teleport::solve_arc(hand.aim_position, dir, p, ctx.physics,
                                    arc_);
                aiming_ = true;
                solved_this_frame = true;
            }
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
    bool declared_{false};
    // <= 0 means "not overridden by binding params; use the scene tunable".
    float max_horizontal_override_{0.0f};
    float min_range_m_{1.0f};
    int hand_{InputSnapshot::Left};
};

}  // namespace

extern "C" int ptv_force_link_builtin_teleport = 1;

PTV_REGISTER_MECHANIC("builtin.locomotion.teleport", TeleportMechanic)
// Legacy/alias id: the hand-authored starter room binds "builtin.teleport".
PTV_REGISTER_MECHANIC("builtin.teleport", TeleportMechanic)

}  // namespace ptv
