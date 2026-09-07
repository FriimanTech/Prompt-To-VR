#pragma once

// ---------------------------------------------------------------------------
// Shared plumbing for the affordance built-ins (door, switchable, container,
// mover, breakable, harvestable).
//
// These six live together rather than in six sibling directories because they
// are the same mechanic shape three times over: find my own declared record,
// check whether interaction is permitted, react to a hand, and write the
// result back through ctx.interactive. Splitting that across six copies is
// how the copies drift.
//
// Two rules every one of them obeys:
//
//   1. State is NEVER stored on the mechanic. It goes through
//      ``ctx.interactive->set_state``, because that is what makes it persist
//      across an environment transition without any save code here. A
//      mechanic that cached "am I open" would silently lose it on reload.
//   2. Geometry with a collider is moved with ``move_kinematic``, never by
//      writing CTransform alone. A door whose visual mesh swings while its
//      collider stays put blocks nothing and reads as broken.
// ---------------------------------------------------------------------------

#include <algorithm>
#include <cmath>
#include <string>

#include "../BuiltinMechanicBase.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace ptv::affordance {

/** Grip thresholds, matching builtin.grab so a squeeze means the same thing
 *  everywhere. */
constexpr float kGripPress = 0.6f;
constexpr float kGripRelease = 0.35f;

/** How close a hand has to be to act on something, in metres. */
constexpr float kReach = 0.18f;

/** The declared id of ``self``, or "" when the world never declared it.
 *
 *  An affordance mechanic bound to an entity with no interactive record is a
 *  configuration mistake rather than a runtime error: it does nothing, which
 *  is the safe outcome. */
inline std::string declared_id(MechanicContext& ctx, std::uint32_t self) {
    if (ctx.interactive == nullptr) return {};
    const char* id = ctx.interactive->id_of(self);
    return id == nullptr ? std::string{} : std::string(id);
}

/** May the player act on this right now?
 *
 *  Combines the per-scene interaction policy with the ``locked`` modifier.
 *  Both have to be consulted in one place, or a mechanic ends up honouring
 *  the lock but ignoring the policy (or the reverse). */
inline bool actionable(MechanicContext& ctx, const std::string& id) {
    if (ctx.interactive == nullptr || id.empty()) return false;
    if (ctx.interactive->is_locked(id.c_str())) return false;
    return ctx.interactive->interaction_allowed(id.c_str());
}

/** Did this hand just start a grip within reach of ``point``? */
inline bool grip_started_near(const InputSnapshot& in, int hand,
                              const Vec3& point, float reach = kReach) {
    const auto& h = in.hands[hand];
    if (!h.hand_valid) return false;
    if (!(h.prev_grip < kGripPress && h.grip >= kGripPress)) return false;
    return glm::length(h.hand_position - point) <= reach;
}

/** Is this hand holding a grip within reach? Used for continuous drags. */
inline bool gripping_near(const InputSnapshot& in, int hand, const Vec3& point,
                          float reach = kReach) {
    const auto& h = in.hands[hand];
    return h.hand_valid && h.grip >= kGripRelease &&
           glm::length(h.hand_position - point) <= reach;
}

/** Publish the hover cue for ``self``.
 *
 *  Called every frame by every affordance built-in, because the mechanic that
 *  will refuse an action is the only thing that reliably knows it will refuse.
 *  ``actionable`` already folds in the lock and the per-scene interaction
 *  policy, so an object a policy forbids stops advertising itself -- promising
 *  an interaction and then declining it is worse than promising nothing.
 *
 *  Faded rather than switched. A hand drifting along the edge of reach would
 *  otherwise flicker the cue on and off every frame, which in a headset reads
 *  as a rendering fault rather than as feedback. ~120 ms each way.
 */
inline void publish_cue(World& world, std::uint32_t self,
                        const InputSnapshot& in, const Vec3& point,
                        bool allowed, float dt, float reach = kReach * 2.0f) {
    bool near_hand = false;
    if (allowed) {
        for (int hand = 0; hand < 2 && !near_hand; ++hand) {
            const auto& h = in.hands[hand];
            near_hand = h.hand_valid &&
                        glm::length(h.hand_position - point) <= reach;
        }
    }

    auto& reg = world.registry();
    const auto e = static_cast<entt::entity>(self);
    if (!reg.all_of<CHighlight>(e)) {
        // Nothing to fade from and nothing to show: skip the component
        // entirely, so an untouched world adds no per-entity cost.
        if (!near_hand) return;
        reg.emplace<CHighlight>(e);
    }
    auto& hl = reg.get<CHighlight>(e);
    const float target = near_hand ? 1.0f : 0.0f;
    const float step = std::clamp(dt / 0.12f, 0.0f, 1.0f);
    hl.intensity += (target - hl.intensity) * step;
    if (hl.intensity < 0.01f) hl.intensity = 0.0f;
}

/** Rotate ``self`` about a pivot, and keep its collider with it.
 *
 *  ``base`` is the pose the scene document placed the object in -- the
 *  closed pose -- captured once rather than accumulated, so repeated opening
 *  and closing cannot drift. */
inline void apply_pivot_rotation(World& world, std::uint32_t self,
                                 const Transform& base, const Vec3& pivot_local,
                                 const Vec3& axis, float angle_rad,
                                 MechanicContext& ctx, float dt) {
    const auto e = static_cast<entt::entity>(self);
    if (!world.registry().all_of<CTransform>(e)) return;

    const Vec3 unit_axis = glm::length(axis) > 1.0e-5f
                               ? glm::normalize(axis)
                               : Vec3(0.0f, 1.0f, 0.0f);
    const Quat q = glm::angleAxis(angle_rad, unit_axis);
    // Pivot in world space: the hinge is declared in the object's own frame,
    // so it has to be carried through the base pose before it means anything.
    const Vec3 pivot = base.position + base.rotation * pivot_local;

    auto& t = world.registry().get<CTransform>(e).t;
    t.rotation = glm::normalize(q * base.rotation);
    t.position = pivot + q * (base.position - pivot);

    if (ctx.physics != nullptr && ctx.physics->has_body(self)) {
        ctx.physics->move_kinematic(self, t.position, t.rotation, dt);
    }
}

/** Slide ``self`` along an axis, and keep its collider with it. */
inline void apply_slide(World& world, std::uint32_t self,
                        const Transform& base, const Vec3& axis,
                        float distance, MechanicContext& ctx, float dt) {
    const auto e = static_cast<entt::entity>(self);
    if (!world.registry().all_of<CTransform>(e)) return;
    const Vec3 unit_axis = glm::length(axis) > 1.0e-5f
                               ? glm::normalize(axis)
                               : Vec3(1.0f, 0.0f, 0.0f);
    auto& t = world.registry().get<CTransform>(e).t;
    t.rotation = base.rotation;
    t.position = base.position + base.rotation * (unit_axis * distance);
    if (ctx.physics != nullptr && ctx.physics->has_body(self)) {
        ctx.physics->move_kinematic(self, t.position, t.rotation, dt);
    }
}

/** Frame-rate independent approach, same form the locomotion built-ins use,
 *  so a door reaches its target over the same wall-clock time at 72 and 90
 *  Hz. */
inline float approach(float current, float target, float tau, float dt) {
    if (tau <= 1.0e-4f) return target;
    return current + (target - current) * (1.0f - std::exp(-dt / tau));
}

/** The interactive record for ``self``, or nullptr. */
inline const CInteractive* record(World& world, std::uint32_t self) {
    const auto e = static_cast<entt::entity>(self);
    if (!world.valid(self) || !world.registry().all_of<CInteractive>(e)) {
        return nullptr;
    }
    return &world.registry().get<CInteractive>(e);
}

/** Capture the pose the document placed this object in, once.
 *
 *  Everything else is expressed relative to it, so a door that has been
 *  opened and closed a hundred times ends up exactly where it started rather
 *  than a hundred small errors away. */
struct BasePose {
    Transform value{};
    bool captured{false};

    void ensure(World& world, std::uint32_t self) {
        if (captured) return;
        const auto e = static_cast<entt::entity>(self);
        if (!world.registry().all_of<CTransform>(e)) return;
        value = world.registry().get<CTransform>(e).t;
        captured = true;
    }
};

}  // namespace ptv::affordance
