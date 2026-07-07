#pragma once

#include <cstdint>
#include <memory>

#include "ptv/math/Math.h"

namespace ptv {

class World;

/**
 * Thin Jolt Physics wrapper.
 *
 * The wrapper deliberately hides Jolt's global factory / layer setup behind
 * a single RAII class. Each scene load calls ``reset()`` and then
 * ``add_rigid_body()`` for every entity that has a CPhysics component.
 */
class PhysicsWorld {
public:
    PhysicsWorld();
    ~PhysicsWorld();

    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    bool initialize();
    void shutdown();

    /** Build/rebuild the physics state from an ECS world. */
    void rebuild_from_world(World& world);

    /** Step the simulation. Typically called once per frame. */
    void step(float dt);

    /** Write updated transforms back onto CTransform components. */
    void sync_to_world(World& world);

    // ------------------------------------------------------------------
    // Runtime interaction APIs (used by grab/throw and by generated code).
    // All ``entity`` arguments are the raw ``entt::entity`` cast to a uint32.
    // These are no-ops when the entity has no associated physics body.
    // ------------------------------------------------------------------

    /** Returns true if there is a physics body for this entity. */
    bool has_body(std::uint32_t entity) const;

    /** Toggle a body between Dynamic and Kinematic (useful for grab). */
    void set_kinematic(std::uint32_t entity, bool kinematic);

    /** Sweep a kinematic body toward the target pose. Must be called each
     *  frame between ``set_kinematic(true)`` and ``set_kinematic(false)``;
     *  Jolt will integrate the implied linear/angular velocity so contacts
     *  with other bodies resolve correctly. */
    void move_kinematic(std::uint32_t entity, const Vec3& pos,
                        const Quat& rot, float dt);

    /** Directly set the position+rotation of a body, activating it. Intended
     *  for teleport/respawn; avoid calling this every frame on kinematic or
     *  dynamic bodies (use ``move_kinematic`` for kinematic followers). */
    void teleport(std::uint32_t entity, const Vec3& pos, const Quat& rot);

    /** Apply a world-space linear impulse (kg*m/s) at the center of mass.
     *  Only meaningful for dynamic bodies. */
    void apply_impulse(std::uint32_t entity, const Vec3& impulse);

    /** Cast a ray against all physics bodies. ``dir`` need not be normalized;
     *  the ray extends ``max_dist`` meters along ``normalize(dir)``. On a hit,
     *  fills ``out_hit`` (world point) + ``out_normal`` and returns true. Used
     *  by teleport to land on the generated environment at any height. */
    bool raycast(const Vec3& origin, const Vec3& dir, float max_dist,
                 Vec3& out_hit, Vec3& out_normal) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ptv
