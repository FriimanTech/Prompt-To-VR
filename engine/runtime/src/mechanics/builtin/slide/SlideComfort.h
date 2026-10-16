#pragma once

// Comfort machinery shared by the two slide (smooth locomotion) mechanics.
//
// Continuous motion is the least comfortable way to move in VR, so the
// details here are load-bearing rather than cosmetic:
//   * speed ramps in and out exponentially instead of snapping, because an
//     instantaneous velocity change is a strong vection cue;
//   * deceleration is slower than acceleration, which reads as momentum
//     rather than as a hard stop;
//   * top speed is capped around a walking pace, per Meta's guidance.

#include <algorithm>
#include <cmath>

#include "ptv/api/MechanicContext.h"
#include "ptv/math/Math.h"

namespace ptv {
namespace slide {

/** Exponential approach with separate accel/decel time constants.
 *
 * Frame-rate independent: the step is ``1 - exp(-dt/tau)``, so 72 Hz and
 * 90 Hz reach the same speed after the same wall-clock time. */
class ComfortRamp {
public:
    void configure(float accel_tau_s, float decel_tau_s) {
        accel_tau_s_ = std::max(1e-3f, accel_tau_s);
        decel_tau_s_ = std::max(1e-3f, decel_tau_s);
    }

    float update(float target_speed, float dt) {
        const float tau = (target_speed > speed_) ? accel_tau_s_ : decel_tau_s_;
        const float k = 1.0f - std::exp(-dt / tau);
        speed_ += (target_speed - speed_) * k;
        return speed_;
    }

    float speed() const { return speed_; }
    void reset() { speed_ = 0.0f; }

private:
    float accel_tau_s_{0.12f};
    float decel_tau_s_{0.18f};
    float speed_{0.0f};
};

/** Walk or run: whether the player is sprinting, and how far the run has
 *  ramped in (0 = walking pace, 1 = running pace).
 *
 *  Sprint needs the stick pushed mostly FORWARD and far enough: running
 *  sideways or backwards is neither natural nor comfortable. ``Hold`` runs
 *  while the stick is held in; ``Toggle`` flips on a click and ends when
 *  the stick comes back to centre, the way most Quest games do it. */
class SprintGate {
public:
    struct Config {
        SprintActivation activation{SprintActivation::Hold};
        float accel_tau_s{0.35f};
        float decel_tau_s{0.25f};
        float forward_cone_deg{70.0f};
        float min_stick{0.6f};
    };

    /** ``axes``: filtered stick (x strafe, +y forward); ``magnitude``: its
     *  filtered length. Returns the ramped blend. */
    float update(const Config& c, Vec2 axes, float magnitude, bool click_down,
                 bool clicked, float dt) {
        const float cone = c.forward_cone_deg * 3.14159265358979f / 180.0f;
        const bool forwardish =
            magnitude >= c.min_stick && axes.y > 0.0f &&
            std::atan2(std::abs(axes.x), axes.y) <= cone;
        bool want = false;
        switch (c.activation) {
            case SprintActivation::Hold:
                toggled_ = false;
                want = click_down && forwardish;
                break;
            case SprintActivation::Toggle:
                if (clicked) toggled_ = !toggled_;
                if (magnitude < 0.2f) toggled_ = false;
                want = toggled_ && forwardish;
                break;
            case SprintActivation::Off:
            default:
                toggled_ = false;
                break;
        }
        const float target = want ? 1.0f : 0.0f;
        const float tau =
            std::max(1e-3f, target > blend_ ? c.accel_tau_s : c.decel_tau_s);
        blend_ += (target - blend_) * (1.0f - std::exp(-std::max(0.0f, dt) / tau));
        if (!want && blend_ < 1e-3f) blend_ = 0.0f;
        wanted_ = want;
        return blend_;
    }

    float blend() const { return blend_; }
    bool wanted() const { return wanted_; }
    void reset() {
        blend_ = 0.0f;
        toggled_ = false;
        wanted_ = false;
    }

private:
    float blend_{0.0f};
    bool toggled_{false};
    bool wanted_{false};
};

/** Ground-plane movement basis derived from some tracked pose. */
struct Basis {
    Vec3 forward{0.0f, 0.0f, 0.0f};
    Vec3 right{0.0f, 0.0f, 0.0f};
    bool valid{false};
};

/** Flatten a world-space forward vector onto the XZ plane and build the
 *  matching right vector.
 *
 * Discarding pitch is deliberate: looking up or down must not launch or
 * bury the player. Returns an invalid basis for a degenerate (straight
 * up/down) input rather than inventing a direction -- if we are not sure
 * where "forward" is, the player should stand still. */
inline Basis basis_from_forward(const Vec3& forward_world) {
    Basis b;
    const Vec3 flat(forward_world.x, 0.0f, forward_world.z);
    const float len = glm::length(flat);
    if (len < 1e-5f) return b;
    b.forward = flat / len;
    // Right-handed, Y up: facing -Z, the player's right is +X, which is
    // cross(forward, up) -- NOT cross(up, forward), which silently mirrors
    // strafe.
    b.right = glm::normalize(glm::cross(b.forward, Vec3(0.0f, 1.0f, 0.0f)));
    b.valid = true;
    return b;
}

}  // namespace slide
}  // namespace ptv
