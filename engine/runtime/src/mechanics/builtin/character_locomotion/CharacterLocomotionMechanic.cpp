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
#include "../LocomotionClipSelector.h"

#include <glm/gtc/quaternion.hpp>

namespace ptv {
namespace {

// Gait selection, clip-rate scaling and the height/reference-speed helpers
// all live in LocomotionClipSelector.h now, shared with builtin.avatar_body.
using locomotion_clips::body_height;
using locomotion_clips::kIdleSpeedEpsilon;

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
        const float height = body_height(world, self);

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
            next = snap_to_ground(ctx, next, height, foot_offset(world, self, height));
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
        drive_animation(world, self, achieved, intent.sprint, height, ctx,
                        intent.gait_variant);
    }

private:
    /** Keep the character's feet on whatever it is standing on.
     *
     *  Ray starts above the head so a step up is found, and reaches below the
     *  feet so a step down is too. No hit means there is nothing underneath
     *  (a ledge, or a scene with no floor), and the safe answer is to leave
     *  the height alone rather than drop the character into the void. */
    Vec3 snap_to_ground(MechanicContext& ctx, const Vec3& position,
                        float height, float feet_below_origin) const {
        const float half = 0.5f * height;
        const Vec3 from(position.x, position.y + half, position.z);
        PhysicsRayHit hit{};
        const float reach = 2.0f * height;
        if (!ctx.physics->raycast(from, Vec3(0.0f, -1.0f, 0.0f), reach, hit)) {
            return position;
        }
        // The entity origin sits at the body's middle, so resting on the
        // ground means the origin-to-sole distance above the contact point.
        return Vec3(position.x, hit.point.y + feet_below_origin, position.z);
    }

    /** How far below the origin the soles are: measured by the pipeline when
     *  known, otherwise half the height (a symmetric body). */
    static float foot_offset(const World& world, std::uint32_t self, float height) {
        // Same space as ``body_height`` (the mesh's target size), so the
        // measured value and the half-height fallback agree.
        if (world.has<CMesh>(self)) {
            const float f = world.get<CMesh>(self).foot_offset_m;
            if (f > 1.0e-4f && f < 2.0f * height) return f;
        }
        return 0.5f * height;
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
     *  to the ground actually being covered.
     *
     *  The selection itself lives in LocomotionClipSelector because
     *  builtin.avatar_body needs the identical answer for the player's own
     *  legs: an NPC and the player walking side by side at the same speed
     *  have to scale their clips the same way, or one of them skates. */
    void drive_animation(World& world, std::uint32_t self, float speed,
                         bool sprint, float height, MechanicContext& ctx,
                         const std::string& variant) {
        locomotion_clips::Tunables tune{};
        tune.walk_speed_mps = walk_speed_;
        tune.run_speed_mps = run_speed_;
        tune.reference_walk_mps = ref_walk_override_;
        tune.reference_run_mps = ref_run_override_;
        clips_.update(world, self, speed, sprint, height, tune, ctx, variant);
    }

    float walk_speed_{1.4f};
    float run_speed_{4.0f};
    float turn_rate_{540.0f};
    bool animated_{true};
    bool ground_snap_{true};
    float ref_walk_override_{0.0f};
    float ref_run_override_{0.0f};
    locomotion_clips::Selector clips_;
};

}  // namespace

// Anchor so MSVC keeps this TU (and therefore its registration) when linking
// ptv_runtime as a static library. Referenced by BuiltinMechanics.cpp.
extern "C" int ptv_force_link_builtin_character_locomotion = 1;

PTV_REGISTER_MECHANIC("builtin.character_locomotion",
                      CharacterLocomotionMechanic)

}  // namespace ptv
