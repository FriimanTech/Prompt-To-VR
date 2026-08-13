#pragma once

#include <cstdint>
#include <memory>
#include <vector>

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

    /** Create the body for a single entity that has CTransform + CPhysics.
     *
     *  Exists so entities spawned after the scene loaded still collide.
     *  ``rebuild_from_world`` only ever ran once, at startup, which meant a
     *  runtime-spawned enemy or projectile was a ghost. Idempotent: an entity
     *  that already has a body returns true without creating a second one. */
    bool add_body_for(World& world, std::uint32_t entity);

    /** Destroy the body for one entity, if it has one. */
    void remove_body(std::uint32_t entity);

    /** Sentinel for "no entity", matching InputSnapshot::kInvalidEntity. */
    static constexpr std::uint32_t kInvalidEntity = 0xFFFFFFFFu;

    /** Every entity whose collider currently intersects ``entity``.
     *
     *  An explicit query rather than a Jolt contact callback on purpose:
     *  Jolt only reports contacts for pairs where at least one body is
     *  DYNAMIC, but the two things gameplay most wants to know about
     *  touching -- the player and an NPC -- are both kinematic, because
     *  their motion is authored rather than simulated. A narrow-phase
     *  overlap test works for every motion type. */
    void overlapping(std::uint32_t entity,
                     std::vector<std::uint32_t>& out) const;

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

    /** Resolve a character-sized move against the world, sliding along walls.
     *
     *  Sweeps an upright capsule of ``height`` metres centred on ``center``
     *  through ``delta`` and returns the movement actually available: the
     *  full delta in open space, or a shortened, wall-parallel version when
     *  something is in the way. ``ignore_entity`` is the mover's own body,
     *  which must be excluded or every cast hits itself at distance zero.
     *
     *  Deliberately fails open: if the sweep resolves to almost no movement
     *  the mover is probably already inside geometry, and the original delta
     *  is returned instead. A player frozen in place cannot recover, while a
     *  player who clips a wall for one frame can just walk back out. */
    Vec3 slide_character(const Vec3& center, float height, const Vec3& delta,
                         std::uint32_t ignore_entity) const;

    /** Cast a ray against all physics bodies. ``dir`` need not be normalized;
     *  the ray extends ``max_dist`` meters along ``normalize(dir)``. On a hit,
     *  fills ``out_hit`` (world point) + ``out_normal`` and returns true. Used
     *  by teleport to land on the generated environment at any height. */
    bool raycast(const Vec3& origin, const Vec3& dir, float max_dist,
                 Vec3& out_hit, Vec3& out_normal) const;

    /** Raycast that also reports WHICH entity was hit (``kInvalidEntity``
     *  when the body has no entity behind it). Shooting, perception and
     *  line-of-sight checks all need the target's identity, not just a
     *  point in space. */
    bool raycast(const Vec3& origin, const Vec3& dir, float max_dist,
                 Vec3& out_hit, Vec3& out_normal,
                 std::uint32_t& out_entity) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ptv
