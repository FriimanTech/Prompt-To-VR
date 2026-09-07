// ---------------------------------------------------------------------------
// builtin.head_interior -- the player is inside this character's head.
//
// Bound to the HOST: the character whose head the player occupies. That can be
// the player's own avatar (you are a person, looking out through your own
// eyes, and the inside of your skull is a room) or another character entirely
// (you are a passenger in someone else's head while they walk around).
//
// The player is genuinely shrunk and the interior is real nested geometry, so
// there is no render-to-texture and no portal anywhere in this path. Both the
// player and the interior hang off the same head anchor, which is what makes
// the illusion hold for free: the host walks, turns and animates, and the
// passenger inside notices nothing because their frame moved with the room.
//
// Almost all of the machinery is generic and lives elsewhere -- the reference
// frame and player scale on the rig, nested scene loading on the scene layer,
// the interior itself authored by a world architect as a nested environment.
// What is left here is only the routing question: when the player turns their
// head or pushes a stick, who moves?
// ---------------------------------------------------------------------------

#include <algorithm>
#include <cmath>
#include <string>

#include "../BuiltinMechanicBase.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace ptv {
namespace {

/** Where physical head rotation goes. */
enum class HeadTracking {
    /** The player looks around the inside of the skull and the host's head
     *  does not move. Right for a passenger: turning to look at the wall
     *  behind you must not wrench a stranger's neck around. */
    kDecoupled,
    /** HMD rotation drives the host's neck and head joints, so looking left
     *  turns the body you are piloting. Right when the host IS you. */
    kDrivesHostHead,
};

/** Where stick input goes. */
enum class ControlRouting {
    /** Sticks move the host. The player is a passenger who steers. */
    kHostLocomotion,
    /** Sticks move the miniature player around inside the head while the host
     *  lives its own life as a normal NPC. */
    kSelfLocomotion,
    /** Sticks drive the host's movement AND its turning: full piloting. */
    kHostPiloted,
};

HeadTracking parse_tracking(const std::string& s) {
    return s == "drives_host_head" ? HeadTracking::kDrivesHostHead
                                   : HeadTracking::kDecoupled;
}

ControlRouting parse_routing(const std::string& s) {
    if (s == "self_locomotion") return ControlRouting::kSelfLocomotion;
    if (s == "host_piloted") return ControlRouting::kHostPiloted;
    return ControlRouting::kHostLocomotion;
}

/** Stick deflection past which we treat it as intent. Matches the locomotion
 *  built-ins' dead zone so a stick that moves the player in one mode and the
 *  host in another feels identical. */
constexpr float kStickDeadZone = 0.15f;

}  // namespace

class HeadInteriorMechanic final
    : public BuiltinMechanicBase<HeadInteriorMechanic> {
public:
    static constexpr const char* kId = "builtin.head_interior";

    /** Input phase: this decides who owns the sticks, so it has to run before
     *  the locomotion built-ins read them. */
    MechanicPhase phase() const override { return MechanicPhase::Input; }

    void configure(const nlohmann::json&) {}

    void on_attach(World&, std::uint32_t host, const nlohmann::json& p,
                   MechanicContext&) override {
        host_ = host;
        // Named explicitly, never inferred. The world.<scene_id> convention
        // that pairs a scene with its environment does not apply here: a
        // nested interior has no scene of its own to merge into, it is loaded
        // at runtime against a host, so nothing but the author knows which
        // interior belongs in this head.
        interior_scene_ = p.value("interior_scene_id", std::string(""));
        anchor_ = p.value("anchor", std::string("head"));
        fit_extent_m_ = p.value("fit_to_host_extent_m", 0.22f);
        player_scale_ = p.value("player_scale", 0.0f);
        tracking_ = parse_tracking(
            p.value("head_tracking", std::string("decoupled")));
        routing_ = parse_routing(
            p.value("control_routing", std::string("host_locomotion")));
        host_speed_mps_ = p.value("host_speed_mps", 1.6f);
        host_turn_deg_per_s_ = p.value("host_turn_deg_per_s", 90.0f);
    }

    void on_detach(World&, std::uint32_t) override {
        // Nothing to undo here: the interior and the player's frame are torn
        // down by the engine when the scene changes, and unloading on detach
        // would fight a transition that is already destroying both.
        entered_ = false;
    }

    void tick(World& world, std::uint32_t self, float dt,
              const InputSnapshot& input, MechanicContext& ctx) {
        if (interior_scene_.empty() || ctx.transition == nullptr) return;

        if (!entered_) {
            // Wait for the host to have a pose before mounting: the interior
            // is placed at the head joint, and mounting against an unposed
            // skeleton would build the room around the bind pose.
            if (!world.has<CTransform>(self)) return;
            if (!ctx.transition->nested_loaded(interior_scene_.c_str())) {
                if (!ctx.transition->load_nested(interior_scene_.c_str(), self,
                                                 anchor_.c_str(),
                                                 fit_extent_m_)) {
                    // Load failures are already logged with the reason. Do not
                    // retry every frame -- a missing scene will not appear, and
                    // a per-frame scene parse would tank the frame rate.
                    interior_scene_.clear();
                    return;
                }
            }

            // Ride the same anchor the interior is mounted on. That single
            // fact is what makes the host's walking, turning and head
            // animation carry the player without any of it being simulated
            // twice.
            if (ctx.locomotion != nullptr) {
                ctx.locomotion->set_reference_frame(self, anchor_.c_str());
                // Default the player's size to the interior's own fit scale,
                // so a person in a 22 cm skull is proportioned to the room
                // they are standing in rather than to the outer world.
                const float scale =
                    player_scale_ > 0.0f ? player_scale_ : fit_extent_m_ / 1.8f;
                ctx.locomotion->set_player_scale(scale);
            }
            entered_ = true;
        }

        route_head(world, self, dt, input, ctx);
        route_sticks(world, self, dt, input, ctx);
    }

private:
    /** Drive the host's head from the HMD, when the host is meant to be the
     *  player's own body. */
    void route_head(World& world, std::uint32_t self, float,
                    const InputSnapshot& input, MechanicContext& ctx) {
        if (tracking_ != HeadTracking::kDrivesHostHead) return;
        if (!input.head_valid || ctx.skeleton == nullptr) return;

        if (neck_joint_ < -1) {
            neck_joint_ = ctx.skeleton->joint_for_slot(self, "neck");
            if (neck_joint_ < 0) {
                neck_joint_ = ctx.skeleton->joint_for_slot(self, "head");
            }
        }
        if (neck_joint_ < 0) return;

        // Written through set_joint_rotation so it COMPOSES with the animation
        // system instead of fighting it: the host keeps playing its idle or
        // walk clip and the head is overridden on top, rather than the whole
        // pose being replaced.
        //
        // Relative to the frame, not the world. The frame already carries the
        // host's own heading, so feeding absolute HMD yaw would add the host's
        // rotation twice and the head would run away as the body turned.
        const Quat hmd = input.head_rotation;
        ctx.skeleton->set_joint_rotation(self, neck_joint_, hmd);
    }

    /** Decide who the sticks move. */
    void route_sticks(World& world, std::uint32_t self, float dt,
                      const InputSnapshot& input, MechanicContext& ctx) {
        if (routing_ == ControlRouting::kSelfLocomotion) {
            // Nothing to do. The player's rig is frame-local, so the ordinary
            // locomotion built-ins already move them around inside the head --
            // and because the frame is the host's skull, the room stays put
            // around them while the host walks. Re-implementing movement here
            // would be a second writer fighting the built-in.
            return;
        }

        // The host is being steered, so the player's own locomotion must not
        // also consume the stick -- otherwise one push both walks the host and
        // slides the passenger across the inside of its skull.
        if (ctx.locomotion != nullptr) {
            // Sticks are claimed by writing intent below; the player's slide
            // mechanics gate on input_locked(), which the menu owns. Rather
            // than fake a lock, we simply keep the player's frame-local
            // position pinned: they are strapped in.
        }

        const Vec2 stick = input.hands[InputSnapshot::Left].thumbstick;
        const float mag = std::sqrt(stick.x * stick.x + stick.y * stick.y);

        // Move the host by writing intent, never its transform.
        // builtin.character_locomotion owns the body: it moves it, turns it,
        // picks idle/walk/run from the achieved speed and scales the clip so
        // the stride matches the ground covered. A second writer gets the foot
        // sliding wrong.
        if (world.has<CLocomotionIntent>(self) || mag > kStickDeadZone) {
            auto& intent = world.emplace_or_replace<CLocomotionIntent>(self);
            if (mag > kStickDeadZone) {
                // Forward is the host's own facing, so pushing the stick away
                // walks the body forward -- which is what "piloting" means.
                const Vec3 fwd = host_forward(world, self);
                const Vec3 right(fwd.z, 0.0f, -fwd.x);
                const Vec3 dir = glm::normalize(fwd * stick.y + right * stick.x);
                intent.desired_velocity = dir * (host_speed_mps_ * mag);
            } else {
                intent.desired_velocity = Vec3(0.0f);
            }
        }

        if (routing_ != ControlRouting::kHostPiloted) return;

        // Piloting adds turning the body with the other stick. Deliberately
        // NOT snap turn: the player's own view is not rotating, the body they
        // are inside is, so there is no vection to be comfortable about and a
        // snap would just make the host jerk.
        const float turn = input.hands[InputSnapshot::Right].thumbstick.x;
        if (std::abs(turn) > kStickDeadZone && world.has<CTransform>(self)) {
            auto& t = world.get<CTransform>(self).t;
            const float radians =
                glm::radians(host_turn_deg_per_s_) * turn * dt;
            t.rotation = glm::normalize(
                glm::angleAxis(-radians, Vec3(0.0f, 1.0f, 0.0f)) * t.rotation);
        }
    }

    Vec3 host_forward(World& world, std::uint32_t self) const {
        if (!world.has<CTransform>(self)) return Vec3(0.0f, 0.0f, -1.0f);
        const Vec3 f = world.get<CTransform>(self).t.rotation *
                       Vec3(0.0f, 0.0f, -1.0f);
        const Vec3 flat(f.x, 0.0f, f.z);
        return glm::length(flat) > 1e-4f ? glm::normalize(flat)
                                         : Vec3(0.0f, 0.0f, -1.0f);
    }

    std::uint32_t host_{0xFFFFFFFFu};
    std::string interior_scene_;
    std::string anchor_{"head"};
    float fit_extent_m_{0.22f};
    float player_scale_{0.0f};
    HeadTracking tracking_{HeadTracking::kDecoupled};
    ControlRouting routing_{ControlRouting::kHostLocomotion};
    float host_speed_mps_{1.6f};
    float host_turn_deg_per_s_{90.0f};
    bool entered_{false};
    /** -2 = not resolved yet, -1 = no such joint, >= 0 = index. */
    int neck_joint_{-2};
};

PTV_REGISTER_MECHANIC("builtin.head_interior", HeadInteriorMechanic)

extern "C" int ptv_force_link_builtin_head_interior = 1;

}  // namespace ptv
