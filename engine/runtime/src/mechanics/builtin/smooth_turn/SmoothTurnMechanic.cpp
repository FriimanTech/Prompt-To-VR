// builtin.locomotion.smooth_turn -- continuous right-stick rotation.
//
// The alternative to snap turning for players who find discrete jumps
// disorienting. Continuous rotation is the strongest vection cue there is,
// so this is the one turning style that may carry a comfort vignette -- and
// only when the game asked for one (see ComfortVignette.h).
//
// Gated on ILocomotion::turn_mode() like snap turn, so both can be bound and
// the player picks one in a settings menu.

#include <algorithm>
#include <cmath>

#include "../BuiltinMechanicBase.h"
#include "../ComfortVignette.h"

namespace ptv {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kDeadzone = 0.2f;

class SmoothTurnMechanic : public BuiltinMechanicBase<SmoothTurnMechanic> {
public:
    static constexpr const char* kId = "builtin.locomotion.smooth_turn";

    MechanicPhase phase() const override { return MechanicPhase::Input; }
    MechanicScope scope() const override { return MechanicScope::Player; }

    void configure(const nlohmann::json& p) {
        degrees_per_s_ =
            std::clamp(p.value("degrees_per_second", 90.0f), 20.0f, 360.0f);
        // Seconds to reach full speed. A short ease-in removes the jolt of
        // an instant angular velocity without making the turn feel laggy.
        ease_tau_s_ = std::clamp(p.value("ease_tau_s", 0.08f), 0.0f, 0.5f);
        vignette_ = comfort::parse_vignette(p);
        offered_ = false;
    }

    void tick(World&, std::uint32_t, float dt, const InputSnapshot& input,
              MechanicContext& ctx) {
        if (ctx.locomotion == nullptr) return;
        if (!offered_) {
            ctx.locomotion->declare_turn_available(::ptv::TurnMode::Smooth);
            comfort::offer(ctx.comfort, ComfortChannel::Turning, vignette_);
            offered_ = true;
        }
        if (ctx.locomotion->turn_mode() != ::ptv::TurnMode::Smooth ||
            ctx.locomotion->input_locked()) {
            rate_ = 0.0f;
            return;
        }

        const float rx = input.hands[InputSnapshot::Right].thumbstick.x;
        float target = 0.0f;
        if (std::abs(rx) > kDeadzone) {
            const float t = (std::abs(rx) - kDeadzone) / (1.0f - kDeadzone);
            target = std::copysign(std::min(t, 1.0f), rx);
        }
        const float k = (ease_tau_s_ <= 1e-4f)
                            ? 1.0f
                            : 1.0f - std::exp(-dt / ease_tau_s_);
        const float prev = rate_;
        rate_ += (target - rate_) * k;

        if (std::abs(rate_) > 1e-3f) {
            // Stick right turns the view right, i.e. yaw decreases.
            ctx.locomotion->rotate_yaw(-rate_ * degrees_per_s_ * kPi / 180.0f *
                                       dt);
        }
        if (ctx.comfort != nullptr && dt > 1e-5f) {
            const float accel = std::abs(rate_ - prev) / dt;
            const float s = std::max(std::abs(rate_),
                                     std::clamp(accel * 0.25f, 0.0f, 1.0f));
            if (s > 0.02f) {
                ctx.comfort->request_vignette(
                    ComfortChannel::Turning, s * vignette_.strength,
                    vignette_.inner_fov_deg, vignette_.feather_deg);
            }
        }
    }

private:
    float degrees_per_s_{90.0f};
    float ease_tau_s_{0.08f};
    float rate_{0.0f};
    comfort::VignetteParams vignette_{};
    bool offered_{false};
};

}  // namespace

extern "C" int ptv_force_link_builtin_smooth_turn = 1;

PTV_REGISTER_MECHANIC("builtin.locomotion.smooth_turn", SmoothTurnMechanic)

}  // namespace ptv
