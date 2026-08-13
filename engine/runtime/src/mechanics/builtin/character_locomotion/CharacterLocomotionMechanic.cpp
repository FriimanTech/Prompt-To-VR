// builtin.character_locomotion -- move an NPC and animate it in agreement.
//
// This is the answer to "the goblin has idle/walk/run/shoot clips, now what?".
// A behaviour writes a desired velocity into ``CLocomotionIntent``; this
// mechanic does everything between that wish and a character that reads as
// genuinely walking:
//
//   1. Moves the body, sweeping it against the world so it does not walk
//      through walls, and snapping it to the floor.
//   2. Turns it to face where it is going (or a target it is tracking).
//   3. Chooses idle / walk / run from the speed it actually achieved.
//   4. Scales the clip's playback rate so the stride matches the ground
//      covered.
//
// Step 4 is the one that is easy to skip and impossible to un-see. Tripo
// retargets with ``animate_in_place=True``, so the clip has no root motion:
// the feet cycle at whatever rate the animator authored while the engine
// slides the character along at whatever speed the AI asked for. When those
// two disagree the feet skate over the floor. The fix used across the
// industry is to scale playback by ``actual_speed / apparent_clip_speed``,
// and to keep that scale inside roughly +/-25% -- past that the character
// visibly moon-walks or scurries, so the right answer is to change clip tier
// (walk -> run) instead of stretching one clip further. That is exactly the
// hysteresis + clamp below.
//
// Setting ``animated: false`` turns steps 3 and 4 off and leaves 1 and 2, so
// the same mechanic drives a prompt that explicitly asks for un-animated
// props sliding across the floor. No separate code path, no separate
// mechanic id for the codegen agent to choose wrongly.

#include <algorithm>
#include <cmath>
#include <string>

#include "../BuiltinMechanicBase.h"

#include <glm/gtc/quaternion.hpp>

namespace ptv {
namespace {

constexpr float kPi = 3.14159265358979323846f;

// Below this the character is standing still, whatever the AI asked for.
constexpr float kIdleSpeedEpsilon = 0.05f;

// How far past a tier boundary the speed must go before the clip changes,
// as a fraction. Without it a character hovering exactly at the walk/run
// threshold flickers between the two clips every frame.
constexpr float kTierHysteresis = 0.12f;

// The usable window for playback-rate scaling. Outside it the motion reads as
// wrong rather than fast, so the tier changes instead.
constexpr float kMinClipSpeed = 0.80f;
constexpr float kMaxClipSpeed = 1.25f;

// Apparent ground speed of a locomotion clip, as a multiple of the
// character's height. A 1.8 m human walks at about 1.4 m/s and runs at about
// 4 m/s, which is where these come from; scaling by height keeps a
// house-sized creature and a knee-high one both looking right.
constexpr float kWalkSpeedPerMeterOfHeight = 0.78f;
constexpr float kRunSpeedPerMeterOfHeight = 2.2f;

enum class Gait : int { Idle = 0, Walk = 1, Run = 2 };

const char* gait_intent(Gait g) {
    switch (g) {
        case Gait::Walk: return "walk";
        case Gait::Run: return "run";
        case Gait::Idle:
        default: return "idle";
    }
}

float character_height(const World& world, std::uint32_t entity) {
    if (world.has<CMesh>(entity)) {
        const auto& m = world.get<CMesh>(entity);
        if (m.target_scale_meters > 1.0e-4f) return m.target_scale_meters;
        if (m.bounds_radius_m > 1.0e-4f) return 2.0f * m.bounds_radius_m;
    }
    if (world.has<CTransform>(entity)) {
        return std::max(0.2f, world.get<CTransform>(entity).t.scale);
    }
    return 1.8f;
}

/** Apparent speed of one locomotion clip, in m/s.
 *
 *  Prefers what the scene compiler measured, falls back to a height-derived
 *  estimate. Never returns zero: it is a divisor. */
float reference_speed(const World& world, std::uint32_t entity,
                      const char* intent, float height, float fallback) {
    if (world.has<CMesh>(entity)) {
        const auto& speeds = world.get<CMesh>(entity).animation_speeds;
        const auto it = speeds.find(intent);
        if (it != speeds.end() && it->second > 1.0e-3f) return it->second;
    }
    return std::max(0.1f, fallback * height);
}

class CharacterLocomotionMechanic
    : public BuiltinMechanicBase<CharacterLocomotionMechanic> {
public:
    static constexpr const char* kId = "builtin.character_locomotion";

    // Simulate: the body has settled and the pose system runs right after, so
    // a clip chosen here is applied on the same frame.
    MechanicPhase phase() const override { return MechanicPhase::Simulate; }
    MechanicScope scope() const override { return MechanicScope::Entity; }

    void configure(const nlohmann::json& p) {
        walk_speed_ = std::clamp(p.value("walk_speed_mps", 1.4f), 0.05f, 20.0f);
        run_speed_ = std::clamp(p.value("run_speed_mps", 4.0f), walk_speed_,
                                40.0f);
        turn_rate_ = std::clamp(p.value("turn_rate_deg_s", 540.0f), 15.0f,
                                3600.0f);
        animated_ = p.value("animated", true);
        ground_snap_ = p.value("ground_snap", true);
        // 0 keeps the height-derived default; a positive value overrides it
        // for an asset whose clips were authored at an unusual stride.
        ref_walk_override_ = std::max(0.0f, p.value("reference_walk_mps", 0.0f));
        ref_run_override_ = std::max(0.0f, p.value("reference_run_mps", 0.0f));
    }

    void tick(World& world, std::uint32_t self, float dt,
              const InputSnapshot&, MechanicContext& ctx) {
        if (dt <= 0.0f || !world.valid(self)) return;
        if (!world.has<CTransform>(self)) return;

        const CLocomotionIntent intent =
            world.has<CLocomotionIntent>(self)
                ? world.get<CLocomotionIntent>(self)
                : CLocomotionIntent{};

        auto& transform = world.get<CTransform>(self).t;
        const float height = character_height(world, self);

        // --- Desired horizontal motion --------------------------------
        //
        // Y is dropped: this is a ground character, and letting a behaviour
        // write vertical velocity would fly it. Gravity is not simulated
        // either; the ground snap below owns height.
        Vec3 wanted(intent.desired_velocity.x, 0.0f, intent.desired_velocity.z);
        float wanted_speed = glm::length(wanted);
        const float ceiling = intent.sprint ? run_speed_ : run_speed_;
        if (wanted_speed > ceiling) {
            wanted *= (ceiling / wanted_speed);
            wanted_speed = ceiling;
        }
        if (!std::isfinite(wanted_speed)) {
            wanted = Vec3(0.0f);
            wanted_speed = 0.0f;
        }

        // --- Move ------------------------------------------------------
        Vec3 delta = wanted * dt;
        if (wanted_speed > kIdleSpeedEpsilon && ctx.physics != nullptr) {
            delta = ctx.physics->slide_move(self, delta);
        }
        Vec3 next = transform.position + delta;

        if (ground_snap_ && ctx.physics != nullptr) {
            next = snap_to_ground(ctx, next, height);
        }

        // The speed ACHIEVED, not the speed requested. A character pressed
        // against a wall must fall back to idle rather than run on the spot.
        const float achieved =
            (dt > 0.0f) ? glm::length(next - transform.position) / dt : 0.0f;

        transform.position = next;

        // --- Face ------------------------------------------------------
        Vec3 face(0.0f);
        if (intent.has_face_target && !intent.face_travel) {
            face = intent.face_target - transform.position;
            face.y = 0.0f;
        } else if (achieved > kIdleSpeedEpsilon) {
            face = Vec3(delta.x, 0.0f, delta.z);
        }
        if (glm::length(face) > 1.0e-4f) {
            transform.rotation =
                turn_toward(transform.rotation, glm::normalize(face), dt);
        }

        // Push the resolved pose into physics so the collider follows the
        // visual and other bodies can be pushed by it.
        if (ctx.physics != nullptr && ctx.physics->has_body(self)) {
            ctx.physics->move_kinematic(self, transform.position,
                                        transform.rotation, dt);
        }

        if (!animated_) return;
        drive_animation(world, self, achieved, intent.sprint, height, ctx);
    }

private:
    /** Keep the character's feet on whatever it is standing on.
     *
     *  Ray starts above the head so a step up is found, and reaches below the
     *  feet so a step down is too. No hit means there is nothing underneath
     *  (a ledge, or a scene with no floor), and the safe answer is to leave
     *  the height alone rather than drop the character into the void. */
    Vec3 snap_to_ground(MechanicContext& ctx, const Vec3& position,
                        float height) const {
        const float half = 0.5f * height;
        const Vec3 from(position.x, position.y + half, position.z);
        PhysicsRayHit hit{};
        const float reach = 2.0f * height;
        if (!ctx.physics->raycast(from, Vec3(0.0f, -1.0f, 0.0f), reach, hit)) {
            return position;
        }
        // The entity origin sits at the capsule centre, so resting on the
        // ground means one half-height above the contact point.
        return Vec3(position.x, hit.point.y + half, position.z);
    }

    /** Rotate toward ``forward_xz`` at the configured rate. */
    Quat turn_toward(const Quat& current, const Vec3& forward_xz,
                     float dt) const {
        // -Z is forward in the glTF / OpenXR convention the engine uses.
        const float target_yaw = std::atan2(forward_xz.x, -forward_xz.z);
        const Quat target = glm::angleAxis(target_yaw, Vec3(0.0f, 1.0f, 0.0f));
        const float max_step = glm::radians(turn_rate_) * dt;

        // Angle between the two rotations, via the quaternion dot product.
        float d = glm::dot(current, target);
        d = std::clamp(std::fabs(d), 0.0f, 1.0f);
        const float angle = 2.0f * std::acos(d);
        if (angle <= max_step || angle < 1.0e-4f) return target;
        return glm::normalize(glm::slerp(current, target, max_step / angle));
    }

    /** Pick the clip tier from the achieved speed and match its playback rate
     *  to the ground actually being covered. */
    void drive_animation(World& world, std::uint32_t self, float speed,
                         bool sprint, float height, MechanicContext& ctx) {
        if (ctx.skeleton == nullptr || !ctx.skeleton->has_skeleton(self)) {
            return;
        }

        const float ref_walk =
            ref_walk_override_ > 0.0f
                ? ref_walk_override_
                : reference_speed(world, self, "walk", height,
                                  kWalkSpeedPerMeterOfHeight);
        const float ref_run =
            ref_run_override_ > 0.0f
                ? ref_run_override_
                : reference_speed(world, self, "run", height,
                                  kRunSpeedPerMeterOfHeight);

        const Gait wanted = choose_gait(speed, sprint, ref_walk);

        // Fall back down the tiers the asset actually has. A creature rig has
        // one locomotion preset, so "run" resolves to the walk clip played
        // faster rather than to nothing at all.
        Gait gait = wanted;
        if (gait == Gait::Run && !ctx.skeleton->has_clip(self, "run")) {
            gait = Gait::Walk;
        }
        if (gait == Gait::Walk && !ctx.skeleton->has_clip(self, "walk")) {
            gait = Gait::Idle;
        }

        const char* intent = gait_intent(gait);
        // play_clip is a no-op when this clip is already running, so calling
        // it every frame costs a string compare and does not restart the
        // cycle.
        ctx.skeleton->play_clip(self, intent, /*looping=*/true, kGaitFade);

        float rate = 1.0f;
        if (gait == Gait::Walk) {
            rate = speed / std::max(0.1f, ref_walk);
        } else if (gait == Gait::Run) {
            rate = speed / std::max(0.1f, ref_run);
        }
        ctx.skeleton->set_clip_speed(
            self, std::clamp(rate, kMinClipSpeed, kMaxClipSpeed));

        gait_ = gait;
    }

    /** Tier selection with hysteresis around each boundary. */
    Gait choose_gait(float speed, bool sprint, float ref_walk) const {
        // The walk/run boundary sits where a walk clip would have to stretch
        // past its usable range anyway.
        const float run_boundary =
            std::max(ref_walk * kMaxClipSpeed, 0.5f * (walk_speed_ + run_speed_));
        const float up = 1.0f + kTierHysteresis;
        const float down = 1.0f - kTierHysteresis;

        if (speed <= kIdleSpeedEpsilon) return Gait::Idle;
        if (sprint) return Gait::Run;

        switch (gait_) {
            case Gait::Idle:
                return speed > kIdleSpeedEpsilon * up ? Gait::Walk : Gait::Idle;
            case Gait::Walk:
                if (speed > run_boundary * up) return Gait::Run;
                return speed < kIdleSpeedEpsilon * down ? Gait::Idle
                                                        : Gait::Walk;
            case Gait::Run:
            default:
                return speed < run_boundary * down ? Gait::Walk : Gait::Run;
        }
    }

    // Gait changes are frequent, so the blend is short enough to feel
    // responsive but long enough to hide the pop.
    static constexpr float kGaitFade = 0.18f;

    float walk_speed_{1.4f};
    float run_speed_{4.0f};
    float turn_rate_{540.0f};
    bool animated_{true};
    bool ground_snap_{true};
    float ref_walk_override_{0.0f};
    float ref_run_override_{0.0f};
    Gait gait_{Gait::Idle};
};

}  // namespace

// Anchor so MSVC keeps this TU (and therefore its registration) when linking
// ptv_runtime as a static library. Referenced by BuiltinMechanics.cpp.
extern "C" int ptv_force_link_builtin_character_locomotion = 1;

PTV_REGISTER_MECHANIC("builtin.character_locomotion",
                      CharacterLocomotionMechanic)

}  // namespace ptv
