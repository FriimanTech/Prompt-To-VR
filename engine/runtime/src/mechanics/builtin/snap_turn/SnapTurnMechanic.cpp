// builtin.locomotion.snap_turn -- discrete right-stick rotation.
//
// Snapping the view by a fixed angle instead of rotating it smoothly is the
// standard VR comfort trick: continuous rotation is the single biggest cause
// of vection sickness, while a discrete jump gives the vestibular system
// nothing to disagree with. See docs/locomotion.md.
//
// Runs in the Input phase (before the frame's InputSnapshot is frozen) and is
// Player-scoped, so binding it to several entities still yields exactly one
// turn per stick flick rather than one per binding.

#include <algorithm>
#include <cmath>

#include "../BuiltinMechanicBase.h"

namespace ptv {
namespace {

constexpr float kPi = 3.14159265358979323846f;
// How far the stick must be pushed before it counts. High on purpose: the
// right stick is a coarse input and an accidental nudge should not spin the
// player.
constexpr float kTurnDeadzone = 0.7f;

class SnapTurnMechanic : public BuiltinMechanicBase<SnapTurnMechanic> {
public:
    static constexpr const char* kId = "builtin.locomotion.snap_turn";

    MechanicPhase phase() const override { return MechanicPhase::Input; }
    MechanicScope scope() const override { return MechanicScope::Player; }

    void configure(const nlohmann::json& p) {
        // Binding params win; otherwise fall back to the scene's player block
        // (resolved in tick(), where the context is available).
        if (p.contains("degrees") && p["degrees"].is_number()) {
            degrees_override_ = p.value("degrees", 45.0f);
        }
        if (p.contains("cooldown_s") && p["cooldown_s"].is_number()) {
            cooldown_override_ = p.value("cooldown_s", 0.30f);
        }
    }

    void tick(World&, std::uint32_t, float dt, const InputSnapshot& input,
              MechanicContext& ctx) {
        if (ctx.locomotion == nullptr) return;

        cooldown_s_ = std::max(0.0f, cooldown_s_ - dt);

        // Snap turn stays available even while the locomotion menu is open --
        // the player may need to physically turn to reach a menu pad. This
        // matches the pre-migration behaviour.
        const auto& tun = ctx.locomotion->tunables();
        const float degrees =
            degrees_override_ > 0.0f ? degrees_override_ : tun.snap_turn_degrees;
        const float cooldown = cooldown_override_ > 0.0f ? cooldown_override_
                                                         : tun.snap_turn_cooldown_s;

        if (cooldown_s_ > 0.0f) return;

        const float rx = input.hands[InputSnapshot::Right].thumbstick.x;
        if (rx > kTurnDeadzone) {
            // Stick right turns the view right, i.e. yaw decreases.
            ctx.locomotion->rotate_yaw(-degrees * kPi / 180.0f);
            cooldown_s_ = cooldown;
        } else if (rx < -kTurnDeadzone) {
            ctx.locomotion->rotate_yaw(degrees * kPi / 180.0f);
            cooldown_s_ = cooldown;
        }
    }

private:
    // <= 0 means "not set by binding params; use the scene tunable".
    float degrees_override_{0.0f};
    float cooldown_override_{0.0f};
    float cooldown_s_{0.0f};
};

}  // namespace

// Anchor so MSVC keeps this TU (and therefore its registrations) when
// linking ptv_runtime as a static library. Referenced by BuiltinMechanics.cpp.
extern "C" int ptv_force_link_builtin_snap_turn = 1;

PTV_REGISTER_MECHANIC("builtin.locomotion.snap_turn", SnapTurnMechanic)
// Legacy/alias id: the hand-authored starter room binds "builtin.snap_turn".
PTV_REGISTER_MECHANIC("builtin.snap_turn", SnapTurnMechanic)

}  // namespace ptv
