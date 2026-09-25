// builtin.locomotion.snap_turn -- discrete right-stick rotation.
//
// Snapping the view by a fixed angle instead of rotating it smoothly is the
// standard VR comfort trick: continuous rotation is the single biggest cause
// of vection sickness, while a discrete jump gives the vestibular system
// nothing to disagree with. See docs/locomotion.md.
//
// One snap per flick. The stick has to come back toward centre before it can
// snap again, so holding it over does NOT keep spinning the player -- that
// repeat is what made holding the stick feel like smooth turning with a
// stutter. And never a blink: a snap is already the comfort option, and a
// dark frame on every flick is the flicker players complain about.
//
// Runs in the Input phase (before the frame's InputSnapshot is frozen) and is
// Player-scoped, so binding it to several entities still yields exactly one
// turn per stick flick rather than one per binding.

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "../BuiltinMechanicBase.h"

namespace ptv {
namespace {

constexpr float kPi = 3.14159265358979323846f;
// How far the stick must be pushed before it counts. High on purpose: the
// right stick is a coarse input and an accidental nudge should not spin the
// player.
constexpr float kTurnDeadzone = 0.7f;
// How far back toward centre it must return before the next snap is armed.
constexpr float kRearmZone = 0.3f;

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
        if (p.contains("activation_threshold") &&
            p["activation_threshold"].is_number()) {
            activate_ = std::clamp(p.value("activation_threshold", kTurnDeadzone),
                                   0.3f, 0.95f);
        }
        if (p.contains("recenter_threshold") &&
            p["recenter_threshold"].is_number()) {
            rearm_ = std::clamp(p.value("recenter_threshold", kRearmZone), 0.05f,
                                activate_ - 0.05f);
        }
        // The angles a settings panel may offer ("15 / 30 / 45"). Whatever
        // angle is in force is snapped to the nearest one.
        if (p.contains("allowed_degrees") && p["allowed_degrees"].is_array()) {
            for (const auto& v : p["allowed_degrees"]) {
                if (v.is_number()) {
                    const float d = v.get<float>();
                    if (d > 0.0f && d <= 180.0f) allowed_.push_back(d);
                }
            }
        }
        // A persisted number the player's settings write; read every frame so
        // a change in a settings panel takes effect on the next flick.
        if (p.contains("degrees_key") && p["degrees_key"].is_string()) {
            degrees_key_ = p["degrees_key"].get<std::string>();
        }
    }

    void tick(World&, std::uint32_t, float dt, const InputSnapshot& input,
              MechanicContext& ctx) {
        if (ctx.locomotion == nullptr) return;
        if (!declared_) {
            ctx.locomotion->declare_turn_available(::ptv::TurnMode::Snap);
            declared_ = true;
        }

        cooldown_s_ = std::max(0.0f, cooldown_s_ - dt);
        const float rx = input.hands[InputSnapshot::Right].thumbstick.x;
        if (std::abs(rx) < rearm_) armed_ = true;

        // Only the selected turn style reacts to the stick, so a game can
        // offer snap and smooth turning and let the player choose.
        if (ctx.locomotion->turn_mode() != ::ptv::TurnMode::Snap) return;
        if (ctx.locomotion->input_locked()) return;

        const auto& tun = ctx.locomotion->tunables();
        float degrees =
            degrees_override_ > 0.0f ? degrees_override_ : tun.snap_turn_degrees;
        if (!degrees_key_.empty() && ctx.persistence != nullptr &&
            ctx.persistence->has(degrees_key_.c_str())) {
            const float saved =
                ctx.persistence->number(degrees_key_.c_str(), degrees);
            if (std::isfinite(saved) && saved > 0.0f) degrees = saved;
        }
        if (!allowed_.empty()) {
            float best = allowed_.front();
            for (float a : allowed_) {
                if (std::abs(a - degrees) < std::abs(best - degrees)) best = a;
            }
            degrees = best;
        }
        // The cooldown is only a floor between two separate flicks now; the
        // re-arm rule is what stops a held stick from repeating.
        const float cooldown =
            std::min(cooldown_override_ > 0.0f ? cooldown_override_
                                               : tun.snap_turn_cooldown_s,
                     0.3f);

        if (!armed_ || cooldown_s_ > 0.0f) return;
        if (rx > activate_) {
            // Stick right turns the view right, i.e. yaw decreases.
            ctx.locomotion->rotate_yaw(-degrees * kPi / 180.0f);
            cooldown_s_ = cooldown;
            armed_ = false;
        } else if (rx < -activate_) {
            ctx.locomotion->rotate_yaw(degrees * kPi / 180.0f);
            cooldown_s_ = cooldown;
            armed_ = false;
        }
    }

private:
    // <= 0 means "not set by binding params; use the scene tunable".
    float degrees_override_{0.0f};
    float cooldown_override_{0.0f};
    float activate_{kTurnDeadzone};
    float rearm_{kRearmZone};
    std::vector<float> allowed_;
    std::string degrees_key_;
    float cooldown_s_{0.0f};
    bool armed_{true};
    bool declared_{false};
};

}  // namespace

// Anchor so MSVC keeps this TU (and therefore its registrations) when
// linking ptv_runtime as a static library. Referenced by BuiltinMechanics.cpp.
extern "C" int ptv_force_link_builtin_snap_turn = 1;

PTV_REGISTER_MECHANIC("builtin.locomotion.snap_turn", SnapTurnMechanic)
// Legacy/alias id: the hand-authored starter room binds "builtin.snap_turn".
PTV_REGISTER_MECHANIC("builtin.snap_turn", SnapTurnMechanic)

}  // namespace ptv
