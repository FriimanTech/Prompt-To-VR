// builtin.grab -- pick objects up with the grip button.
//
// Squeeze near a grabbable object to attach it to that hand; let go to drop
// it. While held, the object's rigid body is switched to KINEMATIC and swept
// toward the hand every frame, so it pushes other bodies around instead of
// tunnelling through them, and it cannot be shoved out of your grip by the
// physics solver.
//
// Player scope: the per-hand loop below must run exactly once per frame, but
// the mechanic is bound to every grabbable prop in the scene (that binding is
// what MARKS a prop as grabbable). Entity scope would run the whole loop once
// per prop and grab, drop and throw N times over.
//
// Input phase: the held entity has to be decided before the frame's
// InputSnapshot is frozen, so a weapon mechanic sees "I am now held" on the
// same frame it was picked up rather than one frame later.

#include <algorithm>
#include <cmath>

#include "../BuiltinMechanicBase.h"
#include "GrabRelease.h"
#include "ptv/core/Logger.h"

namespace ptv {
namespace {

// Asymmetric thresholds (Schmitt trigger): squeezing past 0.7 grabs, but you
// have to relax below 0.4 to let go. A single threshold makes an object
// flicker between held and dropped while the grip rests near it.
constexpr float kGrabPress = 0.7f;
constexpr float kGrabRelease = 0.4f;
// Reach. Roughly a palm's width around the hand origin.
constexpr float kGrabRadius = 0.18f;

constexpr std::uint32_t kNone = InputSnapshot::kInvalidEntity;

/** Nearest entity tagged "grabbable" within ``max_dist`` of ``pos``. */
std::uint32_t find_grabbable(World& world, const Vec3& pos, float max_dist) {
    entt::entity best = entt::null;
    float best_d2 = max_dist * max_dist;
    auto view = world.registry().view<const CTransform, const CTag>();
    for (auto e : view) {
        const auto& tags = view.get<const CTag>(e).tags;
        bool grabbable = false;
        for (const auto& t : tags) {
            if (t == "grabbable") {
                grabbable = true;
                break;
            }
        }
        if (!grabbable) continue;
        const auto& ct = view.get<const CTransform>(e).t;
        const Vec3 dp = ct.position - pos;
        const float d2 = glm::dot(dp, dp);
        if (d2 < best_d2) {
            best_d2 = d2;
            best = e;
        }
    }
    return best == entt::null ? kNone : static_cast<std::uint32_t>(best);
}

class GrabMechanic : public IMechanic {
public:
    std::string id() const override { return "builtin.grab"; }
    MechanicPhase phase() const override { return MechanicPhase::Input; }
    MechanicScope scope() const override { return MechanicScope::Player; }

    // Called once per bound entity even though only one instance exists.
    void on_attach(World& world, std::uint32_t entity, const nlohmann::json& p,
                   MechanicContext&) override {
        // Binding this mechanic is what makes an object grabbable. Previously
        // the engine had two disagreeing notions -- a CGrabbable component
        // that nothing read, and a "grabbable" tag that the search actually
        // used. Unify on the tag, and add it here so a scene no longer has to
        // remember to author both.
        auto& tag = world.registry().get_or_emplace<CTag>(
            static_cast<entt::entity>(entity));
        if (std::find(tag.tags.begin(), tag.tags.end(), "grabbable") ==
            tag.tags.end()) {
            tag.tags.push_back("grabbable");
        }
        if (p.contains("radius_m") && p["radius_m"].is_number()) {
            radius_m_ = std::max(0.01f, p.value("radius_m", kGrabRadius));
        }
    }

    void on_detach(World&, std::uint32_t) override {}

    void on_update(World& world, std::uint32_t, float dt,
                   const InputSnapshot& input, MechanicContext& ctx) override {
        if (ctx.hands == nullptr || ctx.physics == nullptr) return;

        for (int h = 0; h < 2; ++h) {
            auto& hs = hold_[h];
            const auto& hand = input.hands[h];

            const bool was_pressed = hand.prev_grip >= kGrabPress;
            const bool is_pressed = hand.grip >= kGrabPress;
            const bool is_released = hand.grip < kGrabRelease;

            if (!was_pressed && is_pressed && hs.entity == kNone &&
                hand.hand_valid) {
                try_grab(world, h, hand, ctx);
            } else if (was_pressed && is_released && hs.entity != kNone) {
                release(h, hand, ctx);
            }

            // Sweep the held body toward the hand. Done here, in the Input
            // phase, so the target is set BEFORE the physics step integrates
            // it -- doing it after would make held objects lag a frame and
            // visibly jitter.
            if (hs.entity != kNone && hand.hand_valid) {
                const Vec3 target_pos =
                    hand.hand_position + hand.hand_rotation * hs.local_offset;
                const Quat target_rot = hand.hand_rotation * hs.local_rotation;
                ctx.physics->move_kinematic(hs.entity, target_pos, target_rot,
                                            dt);
            }
        }
    }

private:
    struct Hold {
        std::uint32_t entity{kNone};
        Vec3 local_offset{0.0f, 0.0f, 0.0f};
        Quat local_rotation{1.0f, 0.0f, 0.0f, 0.0f};
    };

    void try_grab(World& world, int h, const InputSnapshot::HandState& hand,
                  MechanicContext& ctx) {
        const std::uint32_t target =
            find_grabbable(world, hand.hand_position, radius_m_);
        if (target == kNone) return;

        const auto e = static_cast<entt::entity>(target);
        if (!world.registry().all_of<CTransform>(e)) return;
        const auto& ct = world.registry().get<CTransform>(e).t;

        // Record the object's pose in the hand's frame so it keeps the
        // relative orientation it had at the moment of the grab, rather than
        // snapping to a canned pose.
        const Quat inv_hand = glm::inverse(hand.hand_rotation);
        hold_[h].entity = target;
        hold_[h].local_offset = inv_hand * (ct.position - hand.hand_position);
        hold_[h].local_rotation = inv_hand * ct.rotation;

        // Steal it from the other hand if that hand had it.
        if (hold_[1 - h].entity == target) {
            hold_[1 - h].entity = kNone;
            ctx.hands->set_held_entity(1 - h, kNone);
        }

        ctx.physics->set_kinematic(target, true);
        ctx.hands->set_held_entity(h, target);
        PTV_INFO("Grab: hand {} -> entity {}", h == 0 ? "L" : "R", target);
    }

    void release(int h, const InputSnapshot::HandState& hand,
                 MechanicContext& ctx) {
        const std::uint32_t held = hold_[h].entity;
        // Back to dynamic first: an impulse on a kinematic body does nothing.
        ctx.physics->set_kinematic(held, false);
        hold_[h].entity = kNone;
        ctx.hands->set_held_entity(h, kNone);

        // A throw mechanic bound to this entity turns the release into an
        // impulse. With no throw bound, the object simply drops -- which is
        // exactly what a scene that binds grab without throw is asking for.
        grab::publish_release(held, hand.velocity);
        PTV_INFO("Release: hand {} drops entity {} @ {:.2f} m/s",
                 h == 0 ? "L" : "R", held, glm::length(hand.velocity));
    }

    Hold hold_[2]{};
    float radius_m_{kGrabRadius};
};

}  // namespace

namespace grab {
namespace {
ReleaseEvent& release_slot() {
    static ReleaseEvent ev;
    return ev;
}
}  // namespace

const ReleaseEvent& last_release() { return release_slot(); }

void publish_release(std::uint32_t entity, const Vec3& hand_velocity) {
    ReleaseEvent& ev = release_slot();
    ev.entity = entity;
    ev.hand_velocity = hand_velocity;
    ++ev.serial;
}

}  // namespace grab

extern "C" int ptv_force_link_builtin_grab = 1;

PTV_REGISTER_MECHANIC("builtin.grab", GrabMechanic)

}  // namespace ptv
