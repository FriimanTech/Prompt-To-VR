// builtin.locomotion.slide.head_relative
// builtin.locomotion.slide.controller_relative
//
// Continuous ("smooth") locomotion on the ground plane, driven by the left
// stick. The two variants differ only in which tracked pose supplies the
// forward direction:
//
//   head_relative       -- the HMD's forward, so you walk where you look.
//                          Familiar, but couples steering to head movement.
//   controller_relative -- the left controller's aim, so you can look around
//                          freely while gliding. More comfortable for many
//                          players, which is why it is offered separately.
//
// Both share the comfort ramp in SlideComfort.h. See docs/locomotion.md.

#include <algorithm>
#include <cmath>

#include "../BuiltinMechanicBase.h"
#include "../StickFilter.h"
#include "SlideComfort.h"
#include "ptv/core/Logger.h"

namespace ptv {
namespace {

/** Shared behaviour; subclasses only decide where "forward" comes from. */
class SlideMechanicBase : public IMechanic {
public:
    MechanicPhase phase() const override { return MechanicPhase::Input; }
    MechanicScope scope() const override { return MechanicScope::Player; }

    void on_attach(World&, std::uint32_t, const nlohmann::json& p,
                   MechanicContext&) override {
        if (p.contains("speed_mps") && p["speed_mps"].is_number()) {
            max_speed_override_ = p.value("speed_mps", 0.0f);
        } else if (p.contains("max_speed_mps") && p["max_speed_mps"].is_number()) {
            max_speed_override_ = p.value("max_speed_mps", 0.0f);
        }
        if (p.contains("accel_tau_s") && p["accel_tau_s"].is_number()) {
            accel_tau_override_ = p.value("accel_tau_s", 0.0f);
        }
        if (p.contains("decel_tau_s") && p["decel_tau_s"].is_number()) {
            decel_tau_override_ = p.value("decel_tau_s", 0.0f);
        }
    }

    void on_detach(World&, std::uint32_t) override {}

    void on_update(World&, std::uint32_t, float dt, const InputSnapshot& input,
                   MechanicContext& ctx) override {
        if (ctx.locomotion == nullptr) return;

        // Exactly one locomotion style owns the stick, so a scene can bind
        // all three and let the player switch at runtime.
        if (ctx.locomotion->active_mode() != mode() ||
            ctx.locomotion->input_locked()) {
            ramp_.reset();
            return;
        }

        const auto& tun = ctx.locomotion->tunables();
        ramp_.configure(accel_tau_override_ > 0.0f ? accel_tau_override_
                                                   : tun.slide_accel_tau_s,
                        decel_tau_override_ > 0.0f ? decel_tau_override_
                                                   : tun.slide_decel_tau_s);
        const float max_speed = max_speed_override_ > 0.0f
                                    ? max_speed_override_
                                    : tun.slide_max_speed_mps;

        const auto filtered = stick::filter(input.hands[InputSnapshot::Left].thumbstick);
        // OpenXR thumbstick convention (1.1 spec, "Standard input
        // identifiers"): +1 y means all the way UP, i.e. pushed away from the
        // body. So forward is +y, not -y.
        const float forward_axis = filtered.axes.y;
        const float strafe_axis = filtered.axes.x;

        // Recomputed from the LATEST pose every frame -- no fixed world
        // forward, no spawn-yaw fallback -- so turning mid-stride re-steers
        // immediately.
        const slide::Basis basis = compute_basis(input);

        const float target_speed = basis.valid ? filtered.magnitude * max_speed : 0.0f;
        const float speed = ramp_.update(target_speed, dt);

        if (basis.valid && speed > 1e-4f && filtered.magnitude > 0.0f) {
            // Re-normalise the stick intent: its magnitude is already baked
            // into the ramped speed, so using it again would square it.
            const float inv = 1.0f / filtered.magnitude;
            const float fa = forward_axis * inv;
            const float sa = strafe_axis * inv;
            const Vec3 dir = basis.forward * fa + basis.right * sa;
            ctx.locomotion->translate(Vec3(dir.x * speed * dt, 0.0f,
                                           dir.z * speed * dt));
            trace(dt, basis, filtered, speed, ctx);
        } else if (filtered.magnitude > 0.05f && !basis.valid) {
            // The player is pushing but we refused to move because no
            // tracked pose is available. Say so, once a second, otherwise
            // this looks like the game is simply broken.
            warn_accum_s_ += dt;
            if (warn_accum_s_ >= 1.0f) {
                PTV_WARN(
                    "Slide[{}] no basis (head_valid={}, left_aim_valid={}). "
                    "Player won't move until tracking is restored.",
                    trace_name(), input.head_valid ? "yes" : "no",
                    input.hands[InputSnapshot::Left].aim_valid ? "yes" : "no");
                warn_accum_s_ = 0.0f;
            }
        }
    }

protected:
    virtual ::ptv::LocomotionMode mode() const = 0;
    virtual slide::Basis compute_basis(const InputSnapshot& input) = 0;
    virtual const char* trace_name() const = 0;

private:
    void trace(float dt, const slide::Basis& basis,
               const stick::Filtered& filtered, float speed,
               MechanicContext& ctx) {
        // 1 Hz proof of which tracked pose drove motion. If the direction
        // stays fixed across seconds while the player physically rotates,
        // something upstream is feeding a stale pose.
        trace_accum_s_ += dt;
        if (trace_accum_s_ < 1.0f) return;
        const Vec3 rig = ctx.locomotion->position();
        PTV_INFO(
            "Slide[{}] fwd=({:+.2f},{:+.2f}) stick=({:+.2f},{:+.2f}) "
            "speed={:.2f}m/s rig=({:+.2f},{:+.2f})",
            trace_name(), basis.forward.x, basis.forward.z, filtered.axes.x,
            filtered.axes.y, speed, rig.x, rig.z);
        trace_accum_s_ = 0.0f;
    }

    slide::ComfortRamp ramp_{};
    float max_speed_override_{0.0f};
    float accel_tau_override_{0.0f};
    float decel_tau_override_{0.0f};
    float trace_accum_s_{0.0f};
    float warn_accum_s_{0.0f};
};

class SlideHeadRelative final : public SlideMechanicBase {
public:
    std::string id() const override {
        return "builtin.locomotion.slide.head_relative";
    }

protected:
    ::ptv::LocomotionMode mode() const override {
        return ::ptv::LocomotionMode::SlideHeadRelative;
    }
    const char* trace_name() const override { return "head_rel"; }

    slide::Basis compute_basis(const InputSnapshot& input) override {
        // Require a real head pose. The cache starts empty on purpose: until
        // the runtime has delivered one valid VIEW pose there is no forward
        // direction and the player simply stands still. A world-space
        // fallback here is what once made players slide in world -Z no
        // matter which way they were facing.
        if (input.head_valid) {
            const slide::Basis b = slide::basis_from_forward(
                input.head_rotation * Vec3(0.0f, 0.0f, -1.0f));
            if (b.valid) {
                last_forward_ = b.forward;
                return b;
            }
        }
        if (glm::length(last_forward_) > 1e-4f) {
            return slide::basis_from_forward(last_forward_);
        }
        return slide::Basis{};
    }

private:
    Vec3 last_forward_{0.0f, 0.0f, 0.0f};
};

class SlideControllerRelative final : public SlideMechanicBase {
public:
    std::string id() const override {
        return "builtin.locomotion.slide.controller_relative";
    }

protected:
    ::ptv::LocomotionMode mode() const override {
        return ::ptv::LocomotionMode::SlideControllerRelative;
    }
    const char* trace_name() const override { return "ctrl_rel"; }

    slide::Basis compute_basis(const InputSnapshot& input) override {
        // /aim, not /grip. Touch controllers' grip handle is angled ~30 deg
        // up from horizontal when held naturally, so a grip-based forward
        // feels like the controller is permanently tilted. /aim points along
        // the controller body, which is where players expect to be pointing.
        //
        // Strictly gated on a tracked pose this frame: if the controller is
        // not tracked the player does not move. We never invent a direction.
        const auto& left = input.hands[InputSnapshot::Left];
        if (!left.aim_valid) return slide::Basis{};
        return slide::basis_from_forward(left.aim_rotation *
                                         Vec3(0.0f, 0.0f, -1.0f));
    }
};

}  // namespace

extern "C" int ptv_force_link_builtin_slide = 1;

PTV_REGISTER_MECHANIC("builtin.locomotion.slide.head_relative",
                      SlideHeadRelative)
PTV_REGISTER_MECHANIC("builtin.locomotion.slide.controller_relative",
                      SlideControllerRelative)

}  // namespace ptv
