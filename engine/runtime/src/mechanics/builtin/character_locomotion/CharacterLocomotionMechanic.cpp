// builtin.character_locomotion -- move an NPC and animate it in agreement.
//
// This is the answer to "the goblin has idle/walk/run/shoot clips, now what?".
// A behaviour writes a desired velocity into ``CLocomotionIntent``; this
// mechanic does everything between that wish and a character that reads as
// genuinely walking:
//
//   1. Moves the body, sweeping it against the world so it does not walk
//      through walls, and keeps it on the floor -- or, for a flying
//      character, at its hover height.
//   2. Turns it to face where it is going (or a target it is tracking), and
//      slows it while it is badly turned so it turns in place instead of
//      walking sideways.
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
// hysteresis + clamp in LocomotionClipSelector.
//
// Setting ``animated: false`` turns steps 3 and 4 off and leaves 1 and 2, so
// the same mechanic drives a prompt that explicitly asks for un-animated
// props sliding across the floor.
//
// Motor modes (param ``motor``):
//   * ``grounded`` (default): horizontal motion only. Steps up to 0.35 m are
//     climbed, slopes steeper than ``max_slope_deg`` are walls, and when the
//     floor drops away by more than ``step_down_m`` the body FALLS under
//     gravity and lands instead of teleporting down. ``jump_velocity`` on the
//     intent launches it.
//   * ``flying``: no floor snap. ``desired_velocity`` is used in 3D; with
//     ``hover_height_m`` > 0 the body instead holds that height above
//     whatever is below it (a hovering drone, a ghost).
//
// Facing relies on the character's model frame: the loader turns every
// character so its face points down the entity's -Z, so the entity's
// rotation IS the way the character faces whatever source it came from.

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "../BuiltinMechanicBase.h"
#include "../LocomotionClipSelector.h"
#include "ptv/api/Facing.h"
#include "ptv/core/Logger.h"

#include <glm/gtc/quaternion.hpp>

namespace ptv {
namespace {

using locomotion_clips::body_height;
using locomotion_clips::kIdleSpeedEpsilon;

enum class Motor { Grounded, Flying };

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
        turn_in_place_deg_ =
            std::clamp(p.value("turn_in_place_deg", 60.0f), 10.0f, 180.0f);
        animated_ = p.value("animated", true);
        ground_snap_ = p.value("ground_snap", true);
        const std::string motor = p.value("motor", std::string("grounded"));
        motor_ = (motor == "flying") ? Motor::Flying : Motor::Grounded;
        hover_height_ = std::clamp(p.value("hover_height_m", 0.0f), 0.0f, 50.0f);
        gravity_ = std::clamp(p.value("gravity_mps2", 9.81f), 0.0f, 50.0f);
        step_down_ = std::clamp(p.value("step_down_m", 0.35f), 0.0f, 2.0f);
        max_slope_cos_ = std::cos(glm::radians(
            std::clamp(p.value("max_slope_deg", 50.0f), 5.0f, 89.0f)));
        // 0 keeps the height-derived default; a positive value overrides it
        // for an asset whose clips were authored at an unusual stride.
        ref_walk_override_ = std::max(0.0f, p.value("reference_walk_mps", 0.0f));
        ref_run_override_ = std::max(0.0f, p.value("reference_run_mps", 0.0f));
        // A stationary NPC (a range target, a sentry, a shopkeeper) is still
        // stood on the floor, turned and animated by this mechanic -- it is
        // the one writer of an NPC's pose -- it just never travels.
        stationary_ = p.value("stationary", false);
        face_player_ = p.value("face_player", false);
    }

    void tick(World& world, std::uint32_t self, float dt,
              const InputSnapshot&, MechanicContext& ctx) {
        if (dt <= 0.0f || !world.valid(self)) return;
        if (!world.has<CTransform>(self)) return;

        CLocomotionIntent intent =
            world.has<CLocomotionIntent>(self)
                ? world.get<CLocomotionIntent>(self)
                : CLocomotionIntent{};
        // One-shot: consumed whether or not it can be honoured this frame,
        // so a jump requested mid-air does not fire on landing.
        if (intent.jump_velocity != 0.0f && world.has<CLocomotionIntent>(self)) {
            world.get<CLocomotionIntent>(self).jump_velocity = 0.0f;
        }

        auto& transform = world.get<CTransform>(self).t;
        const float height = body_height(world, self);
        const float foot = foot_offset(world, self, height);
        const bool flying = motor_ == Motor::Flying;

        // --- Desired motion -------------------------------------------
        Vec3 wanted = intent.desired_velocity;
        if (!flying || hover_height_ > 0.0f) wanted.y = 0.0f;
        if (stationary_) {
            wanted.x = 0.0f;
            wanted.z = 0.0f;
        }
        float wanted_speed = glm::length(wanted);
        if (wanted_speed > run_speed_) {
            wanted *= (run_speed_ / wanted_speed);
            wanted_speed = run_speed_;
        }
        if (!std::isfinite(wanted_speed)) {
            wanted = Vec3(0.0f);
            wanted_speed = 0.0f;
        }

        // --- Turn-in-place --------------------------------------------
        // A character facing away from where it wants to go turns before it
        // walks. Without this it travels sideways (or backwards) at full
        // speed while the turn catches up, which reads as sliding.
        const Vec3 facing = facing_of(transform.rotation);
        const Vec3 wanted_xz(wanted.x, 0.0f, wanted.z);
        if (intent.face_travel && glm::length(wanted_xz) > kIdleSpeedEpsilon) {
            const float err = glm::degrees(heading_error(facing, wanted_xz));
            if (err > turn_in_place_deg_) {
                const float span = std::max(1.0f, 180.0f - turn_in_place_deg_);
                const float k = std::clamp(
                    1.0f - (err - turn_in_place_deg_) / span, 0.1f, 1.0f);
                wanted.x *= k;
                wanted.z *= k;
            }
        }

        // --- Move horizontally ----------------------------------------
        Vec3 horizontal(wanted.x * dt, 0.0f, wanted.z * dt);
        if (glm::length(horizontal) > kIdleSpeedEpsilon * dt &&
            ctx.physics != nullptr) {
            horizontal = ctx.physics->slide_move(self, horizontal);
            horizontal.y = 0.0f;
        }
        Vec3 next = transform.position + horizontal;

        // --- Vertical motor -------------------------------------------
        if (flying) {
            next = fly_vertical(ctx, self, next, wanted.y, height, foot, dt);
        } else if (ground_snap_ && ctx.physics != nullptr) {
            next = ground_vertical(world, ctx, self, transform.position, next,
                                   height, foot, intent.jump_velocity, dt);
            if (have_floor_ && !airborne_) {
                last_grounded_ = next;
                has_last_grounded_ = true;
            }
            next.y += plant_feet(world, ctx, self, next, height, dt);
            if (next.y - transform.position.y > 1.0f && rise_warnings_ < 3) {
                ++rise_warnings_;
                const std::string* id = world.id_of(static_cast<entt::entity>(self));
                PTV_WARN("character_locomotion: '{}' rose {:.2f} m in one frame (dt {:.3f}, "
                         "floor {} at {:.2f}, airborne {}, plant {:.3f})",
                         id != nullptr ? *id : std::to_string(self),
                         next.y - transform.position.y, dt, have_floor_ ? "yes" : "no", floor_y_,
                         airborne_ ? 1 : 0, plant_offset_);
            }
        }

        // The speed ACHIEVED, not the speed requested. A character pressed
        // against a wall must fall back to idle rather than run on the spot.
        const Vec3 moved(next.x - transform.position.x, 0.0f,
                         next.z - transform.position.z);
        const float achieved = glm::length(moved) / dt;

        transform.position = next;

        // --- Face ------------------------------------------------------
        Vec3 face(0.0f);
        if (intent.has_face_target && !intent.face_travel) {
            face = intent.face_target - transform.position;
            face.y = 0.0f;
        } else if (glm::length(wanted_xz) > kIdleSpeedEpsilon) {
            // Toward where it WANTS to go, not where the wall let it slide:
            // a character deflected along a wall still faces its goal.
            face = wanted_xz;
        } else if (achieved > kIdleSpeedEpsilon) {
            face = moved;
        } else if (face_player_ && ctx.hands != nullptr) {
            const std::uint32_t player = ctx.hands->player_entity();
            if (player != self && world.valid(player) && world.has<CTransform>(player)) {
                face = world.get<CTransform>(player).t.position - transform.position;
                face.y = 0.0f;
            }
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

        if (animated_ && !airborne_) {
            drive_animation(world, self, achieved, intent.sprint, height, ctx,
                            intent.gait_variant);
        }
        if (intent.has_look_target) {
            look_at(ctx, self, transform, intent.look_target);
        }
    }

private:
    /** Grounded vertical motion: snap to the floor, climb steps, refuse
     *  steep slopes, fall off ledges and land. */
    Vec3 ground_vertical(World& world, MechanicContext& ctx, std::uint32_t self,
                         const Vec3& from_pos, Vec3 next, float height,
                         float foot, float jump_velocity, float dt) {
        PhysicsRayHit hit{};
        bool has_floor = probe_floor(ctx, self, next, height, foot, hit);

        // A floor this steep is a wall: undo the horizontal step onto it.
        // Only when it is ABOVE the current floor -- walking down a steep
        // bank is a fall, handled below. So is a "floor" more than a step
        // above the one it stands on: a crate or a table top is an obstacle
        // to walk around, not a step to climb.
        const bool steep = has_floor && hit.normal.y < max_slope_cos_ &&
                           hit.point.y + foot > from_pos.y + 0.05f;
        const bool ledge = has_floor && have_floor_ &&
                           hit.point.y > floor_y_ + kMaxStepUpM + 0.02f;
        if (!airborne_ && (steep || ledge)) {
            next.x = from_pos.x;
            next.z = from_pos.z;
            has_floor = probe_floor(ctx, self, next, height, foot, hit);
            if (!has_floor) {
                have_floor_ = false;
                return next;
            }
        }
        have_floor_ = has_floor;
        if (has_floor) floor_y_ = hit.point.y;

        if (!airborne_ && jump_velocity > 0.0f) {
            airborne_ = true;
            vertical_speed_ = std::min(jump_velocity, 20.0f);
            if (animated_ && ctx.skeleton != nullptr &&
                ctx.skeleton->has_clip(self, "jump")) {
                ctx.skeleton->play_clip(self, "jump", /*looping=*/false, 0.1f);
            }
        }

        if (!has_floor) {
            // Thrown far above the floor it stood on (something outside the
            // character moved its body): go back to where it last stood.
            if (!airborne_ && has_last_grounded_ && next.y > last_grounded_.y + 2.0f) {
                if (!warned_recover_) {
                    warned_recover_ = true;
                    const std::string* id = world.id_of(static_cast<entt::entity>(self));
                    PTV_WARN("character_locomotion: '{}' lost its floor {:.1f} m up; returned "
                             "to where it last stood",
                             id != nullptr ? *id : std::to_string(self),
                             next.y - last_grounded_.y);
                }
                have_floor_ = true;
                return last_grounded_;
            }
            // Nothing underneath at all (a scene with no floor here): leave
            // the height alone rather than drop into the void.
            if (!airborne_) return next;
        }
        const float rest_y = has_floor ? hit.point.y + foot : -1.0e9f;

        if (!airborne_) {
            if (rest_y < next.y - step_down_) {
                // The floor dropped away: fall rather than teleport down.
                airborne_ = true;
                vertical_speed_ = 0.0f;
            } else {
                const float max_rise =
                    std::max(kMaxStepUpM, kMaxRiseMps * std::max(0.0f, dt));
                next.y = std::min(rest_y, next.y + max_rise);
                return next;
            }
        }

        vertical_speed_ -= gravity_ * dt;
        vertical_speed_ = std::max(vertical_speed_, -kTerminalFallMps);
        next.y += vertical_speed_ * dt;
        if (next.y <= rest_y && vertical_speed_ <= 0.0f) {
            next.y = rest_y;
            airborne_ = false;
            vertical_speed_ = 0.0f;
        }
        (void)world;
        return next;
    }

    /** Keep the DRAWN feet on the floor, whatever the clip does with the
     *  hips.
     *
     *  The model frame puts the soles on the floor in the BIND pose. A walk
     *  clip can still carry the body lower (a heavy brute's crouched gait)
     *  or higher, which sinks or floats the feet by a few centimetres while
     *  the capsule is exactly right. This measures the lowest foot / toe
     *  joint of the pose being drawn, less that joint's bind-pose height
     *  above the soles, against the floor the snap found, and eases the body
     *  by the difference -- slowly enough that the stance foot's normal
     *  rise and fall in a stride is not chased. Clamped so a bad pose cannot
     *  move the body far. Returns the vertical offset to apply this frame. */
    float plant_feet(World& world, MechanicContext& ctx, std::uint32_t self,
                     const Vec3& next, float height, float dt) {
        if (airborne_ || !have_floor_ || !animated_ || ctx.skeleton == nullptr ||
            !ctx.skeleton->has_skeleton(self) || !world.has<CSkeleton>(self) ||
            !world.has<CTransform>(self)) {
            plant_offset_ = 0.0f;
            return 0.0f;
        }
        if (!feet_resolved_) {
            feet_resolved_ = true;
            const float bind_min = world.get<CSkeleton>(self).skeleton.bind_min_y;
            for (const char* slot : {"foot_l", "foot_r", "toes_l", "toes_r"}) {
                const int j = ctx.skeleton->joint_for_slot(self, slot);
                if (j < 0) continue;
                feet_.push_back({j, ctx.skeleton->joint_bind_model(self, j)[3].y - bind_min});
            }
        }
        if (feet_.empty()) return 0.0f;
        const auto& t = world.get<CTransform>(self).t;
        float sole = 1.0e30f;
        for (const auto& f : feet_) {
            sole = std::min(sole, ctx.skeleton->joint_world(self, f.joint)[3].y -
                                      f.above_sole * t.scale);
        }
        if (sole > 1.0e29f) return 0.0f;
        // The drawn pose is last frame's; its soles sit (sole - body y) from
        // the body whatever offset was applied. The offset that puts them on
        // the floor at this frame's snapped height is therefore:
        const float target = floor_y_ - next.y - (sole - t.position.y);
        const float k = 1.0f - std::exp(-dt / kPlantTauS);
        plant_offset_ += (target - plant_offset_) * k;
        // Never so far that the snap would mistake it for a ledge.
        const float limit = std::min(kMaxPlantFraction * height, 0.8f * step_down_);
        plant_offset_ = std::clamp(plant_offset_, -limit, limit);
        return plant_offset_;
    }

    /** Flying vertical motion: free 3D, or hold a hover height. */
    Vec3 fly_vertical(MechanicContext& ctx, std::uint32_t self, Vec3 next,
                      float wanted_vy, float height, float foot,
                      float dt) const {
        airborne_ = false;
        if (hover_height_ <= 0.0f || ctx.physics == nullptr) {
            next.y += wanted_vy * dt;
            return next;
        }
        PhysicsRayHit hit{};
        if (!probe_floor(ctx, self, next, height + hover_height_, foot + hover_height_, hit)) {
            return next;
        }
        const float target = hit.point.y + hover_height_ + foot;
        next.y += (target - next.y) * (1.0f - std::exp(-dt / 0.25f));
        return next;
    }

    /** Floor under ``position``: level geometry and walkable bodies only
     *  (``raycast_floor``), never a prop, a character or anything carried.
     *
     *  The ray starts just above a climbable step over the SOLES, not at the
     *  chest: from the chest, a table edge the NPC walked under, a balcony
     *  overhead or the rifle in its own hands read as floor. Only when that
     *  finds nothing -- a body spawned with its origin at the floor, soles
     *  below it -- does it look again from the chest, which with static-only
     *  floors cannot climb anything that moves with the body. */
    static bool probe_floor(MechanicContext& ctx, std::uint32_t self,
                            const Vec3& position, float height, float foot,
                            PhysicsRayHit& hit) {
        if (ctx.physics == nullptr) return false;
        const Vec3 down(0.0f, -1.0f, 0.0f);
        const float sole = position.y - foot;
        const Vec3 low(position.x, sole + kProbeAboveSoleM, position.z);
        if (ctx.physics->raycast_floor(self, low, down, kProbeAboveSoleM + height + 4.0f,
                                       hit)) {
            return true;
        }
        const Vec3 high(position.x, position.y + 0.5f * height, position.z);
        return ctx.physics->raycast_floor(self, high, down, 0.5f * height + foot + 4.0f, hit);
    }

    /** A step this high is climbed in one frame; anything taller rises at
     *  ``kMaxRiseMps``. Generous enough for stairs and spawn settling. */
    static constexpr float kMaxStepUpM = 0.35f;
    /** Floor probes start this far above the soles: over a climbable step,
     *  under a table top or a held weapon. */
    static constexpr float kProbeAboveSoleM = 0.5f;
    static constexpr float kMaxRiseMps = 6.0f;
    static constexpr float kTerminalFallMps = 30.0f;

    /** How far below the origin the soles are. A framed character's is half
     *  its height by construction; anything else carries a value the loader
     *  measured from its bind pose. Only a body with no mesh at all is the
     *  bare capsule, whose soles really are half its height down. */
    float foot_offset(const World& world, std::uint32_t self,
                      float height) const {
        if (world.has<CMesh>(self)) {
            const float f = world.get<CMesh>(self).foot_offset_m;
            if (f > 1.0e-4f && f < 2.0f * height) return f;
            if (!warned_foot_) {
                warned_foot_ = true;
                PTV_WARN(
                    "character_locomotion: entity {} has a mesh but no "
                    "foot_offset_m; standing it at half its height, which "
                    "floats or sinks any mesh not centred on its origin",
                    self);
            }
        }
        return 0.5f * height;
    }

    /** Rotate toward ``forward_xz`` at the configured rate. */
    Quat turn_toward(const Quat& current, const Vec3& forward_xz,
                     float dt) const {
        const Quat target = rotation_facing(forward_xz);
        const float max_step = glm::radians(turn_rate_) * dt;
        // Angle between the two rotations, via the quaternion dot product.
        float d = glm::dot(current, target);
        d = std::clamp(std::fabs(d), 0.0f, 1.0f);
        const float angle = 2.0f * std::acos(d);
        if (angle <= max_step || angle < 1.0e-4f) return target;
        return glm::normalize(glm::slerp(current, target, max_step / angle));
    }

    /** Turn the head toward a point, clamped, leaving the body alone. */
    void look_at(MechanicContext& ctx, std::uint32_t self, const Transform& body,
                 const Vec3& target) {
        if (ctx.skeleton == nullptr || !ctx.skeleton->has_skeleton(self)) return;
        if (head_joint_ == -2) {
            head_joint_ = ctx.skeleton->joint_for_slot(self, "head");
        }
        if (head_joint_ < 0) return;
        Vec3 to = target - body.position;
        to.y = 0.0f;
        if (glm::length(to) < 1.0e-3f) return;
        const float yaw = std::clamp(
            wrap_angle(yaw_from_forward(to) - yaw_from_forward(facing_of(body.rotation))),
            -kMaxHeadYaw, kMaxHeadYaw);
        const int parent = ctx.skeleton->joint_parent(self, head_joint_);
        const Quat parent_world =
            parent >= 0 ? glm::quat_cast(Mat3(ctx.skeleton->joint_world(self, parent)))
                        : body.rotation;
        // World up, expressed in the head's parent space, so the turn is a
        // yaw however the rig's bones are oriented.
        const Vec3 up_parent =
            glm::normalize(glm::inverse(parent_world) * Vec3(0.0f, 1.0f, 0.0f));
        const Quat bind = ctx.skeleton->joint_bind_rotation(self, head_joint_);
        const Quat turn = glm::angleAxis(yaw, up_parent);
        ctx.skeleton->set_joint_rotation_from_bind(
            self, head_joint_, glm::normalize(glm::inverse(bind) * turn * bind));
    }

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

    static constexpr float kMaxHeadYaw = 1.0471976f;  // 60 degrees

    float walk_speed_{1.4f};
    float run_speed_{4.0f};
    float turn_rate_{540.0f};
    float turn_in_place_deg_{60.0f};
    bool animated_{true};
    bool ground_snap_{true};
    int rise_warnings_{0};
    Vec3 last_grounded_{0.0f};
    bool has_last_grounded_{false};
    bool warned_recover_{false};
    Motor motor_{Motor::Grounded};
    float hover_height_{0.0f};
    float gravity_{9.81f};
    float step_down_{0.35f};
    float max_slope_cos_{0.6427876f};
    float ref_walk_override_{0.0f};
    float ref_run_override_{0.0f};
    bool stationary_{false};
    bool face_player_{false};
    mutable bool airborne_{false};
    float vertical_speed_{0.0f};
    mutable bool warned_foot_{false};
    int head_joint_{-2};
    // Foot planting (plant_feet).
    struct FootJoint {
        int joint{-1};
        float above_sole{0.0f};  // bind-pose height above the mesh's soles
    };
    static constexpr float kPlantTauS = 0.25f;
    static constexpr float kMaxPlantFraction = 0.12f;
    std::vector<FootJoint> feet_;
    bool feet_resolved_{false};
    float plant_offset_{0.0f};
    float floor_y_{0.0f};
    bool have_floor_{false};
    locomotion_clips::Selector clips_;
};

}  // namespace

// Anchor so MSVC keeps this TU (and therefore its registration) when linking
// ptv_runtime as a static library. Referenced by BuiltinMechanics.cpp.
extern "C" int ptv_force_link_builtin_character_locomotion = 1;

PTV_REGISTER_MECHANIC("builtin.character_locomotion",
                      CharacterLocomotionMechanic)

}  // namespace ptv
