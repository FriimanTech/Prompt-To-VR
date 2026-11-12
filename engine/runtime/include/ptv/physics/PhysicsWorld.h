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

    /** Centre of the body's world-space bounds (what a shot should aim at). */
    bool body_center(std::uint32_t entity, Vec3* out) const;

    /** Toggle a body between Dynamic and Kinematic (useful for grab).
     *
     *  A prop made kinematic is being held or attached, so it moves to the
     *  ``Held`` class (it no longer touches its holder); made dynamic again
     *  it returns to ``Prop``. */
    void set_kinematic(std::uint32_t entity, bool kinematic);

    /** What a body is, which decides what it may touch (docs/physics.md).
     *  The scene sets it from the entity on creation: static geometry,
     *  NPC capsules and kinematic world pieces (``Moving``), dynamic props,
     *  attached items (``Held``). The player's own bodies are set by the
     *  engine. */
    enum class BodyClass : std::uint8_t {
        Static,
        Moving,
        Prop,
        Held,
        PlayerBody,
        PlayerPart,
        Debris,
    };
    void set_body_class(std::uint32_t entity, BodyClass body_class);
    BodyClass body_class(std::uint32_t entity) const;

    /** Mark a moving body as floor (a lift, a raft): floor probes stand on
     *  it. Static geometry is always floor; nothing else ever is. */
    void set_walkable(std::uint32_t entity, bool walkable);

    /** Mass of a dynamic body in kg, or 0 for anything else. */
    float body_mass(std::uint32_t entity) const;

    /** A kinematic capsule (axis = local +Y) for one of the player's body
     *  parts -- a finger, a palm, a forearm, a shin. It is on the PlayerPart
     *  layer: it nudges props as a real hand would, never touches the
     *  player's own body, an NPC or anything held. ``group`` is the hand
     *  (0 left, 1 right) or 2 for the legs (see ``set_part_ignore``). */
    bool create_part_body(std::uint32_t entity, const Vec3& pos, const Quat& rot,
                          float half_height, float radius, int group);

    /** The bodies a part group passes through this frame: the prop its hand
     *  is reaching to grab, so the fingers do not push it away first. */
    void set_part_ignore(int group, const std::vector<std::uint32_t>& entities);

    /** Remove ``entity``'s body from the simulation without destroying it
     *  (``false``), or add it back (``true``). */
    void set_collision_enabled(std::uint32_t entity, bool enabled);

    /** Distance from ``point`` to the entity's collider (its oriented local
     *  bounds): 0 inside, negative when the entity has no body. */
    float distance_to_body(std::uint32_t entity, const Vec3& point) const;

    /** Put a dynamic body to sleep (a prop settled at load: it stays where
     *  the layout put it until something touches it). */
    void sleep_body(std::uint32_t entity);
    /** True when the body is currently simulated. */
    bool body_active(std::uint32_t entity) const;

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
     *  Fails CLOSED: a mover already touching a wall may still move away
     *  from it or along it, but never further into it. Returning the full
     *  delta when wedged -- the old behaviour -- is exactly what let a
     *  player who backed into a wall walk straight through it. */
    Vec3 slide_character(const Vec3& center, float height, const Vec3& delta,
                         std::uint32_t ignore_entity) const;

    /** Same, ignoring every entity in ``ignore`` (the mover and everything
     *  that rides on it). */
    Vec3 slide_character(const Vec3& center, float height, const Vec3& delta,
                         const std::vector<std::uint32_t>& ignore) const;

    // ------------------------------------------------------------------
    // The player's body.
    //
    // A Jolt CharacterVirtual: a swept capsule with ground detection,
    // stair stepping, slope limits and penetration recovery. It is not a
    // rigid body, so nothing else is simulated against it; the separate
    // kinematic player capsule still exists for contacts with NPCs and
    // triggers. Positions here are the capsule's FEET.
    // ------------------------------------------------------------------

    struct CharacterState {
        Vec3 feet{0.0f};
        bool grounded{false};
        Vec3 ground_normal{0.0f, 1.0f, 0.0f};
        float vertical_speed{0.0f};
        float height{1.7f};
        /** Velocity of whatever the feet stand on (a moving platform);
         *  zero on static ground. */
        Vec3 ground_velocity{0.0f};
    };

    /** Create (or re-create) the player character at ``feet``. */
    void create_player_character(const Vec3& feet, float height, float radius);
    void destroy_player_character();
    bool has_player_character() const;

    /** Bodies the player character must pass through: the player's own
     *  contact capsule and everything that moves WITH the player (held,
     *  worn and attached items). A kinematic body that follows the player
     *  and overlaps the capsule pushes it at the player's own velocity --
     *  a feedback loop that accelerates the player without bound. */
    void set_player_character_ignored(const std::vector<std::uint32_t>& entities);

    /** Entities the character actually touched during its last move. */
    void player_character_contacts(std::vector<std::uint32_t>& out) const;

    /** Set the character's feet without touching its velocity (unlike
     *  ``teleport_player_character``, which also zeroes it). */
    void set_player_character_position(const Vec3& feet);

    /** Move the character through ``displacement`` (world metres) this
     *  frame and return how far it actually went.
     *
     *  ``grounded`` applies gravity and keeps the feet on the floor --
     *  stepping up stairs and down slopes -- and IGNORES the vertical part
     *  of ``displacement``: vertical position is owned by the ground, not by
     *  whoever asked to move. ``grounded == false`` is free flight: gravity
     *  is off and the displacement is used as-is. ``jump_speed > 0`` launches
     *  the character upward if it is standing on something. */
    Vec3 move_player_character(const Vec3& displacement, bool grounded,
                               float jump_speed, float dt,
                               bool apply_gravity = true);

    /** Put the character at ``feet`` without sweeping (teleport, spawn). */
    void teleport_player_character(const Vec3& feet);

    /** Change capsule height (crouching). Refused -- returning false --
     *  when the taller shape would not fit, e.g. standing up under a desk. */
    bool resize_player_character(float height);

    CharacterState player_character_state() const;

    /** Keep a head-sized sphere out of static geometry.
     *
     *  Sweeps a sphere of ``radius`` from ``from`` to ``to`` against STATIC
     *  bodies only, then pushes it out of anything it still overlaps.
     *  Returns where the head is allowed to be. Static only on purpose: the
     *  head should be stopped by walls, not by a crate the player is leaning
     *  over or the gun in their own hand. */
    Vec3 resolve_head(const Vec3& from, const Vec3& to, float radius) const;

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

    /** What a filtered ray may hit. */
    struct RayFilter {
        /** Entities whose bodies the ray passes through. A probe cast on
         *  behalf of an entity must list that entity: Jolt treats convex
         *  shapes as solid, so a ray that starts inside or on the caller's
         *  own capsule hits it at fraction 0. */
        std::vector<std::uint32_t> ignore{};
        /** Only STATIC-layer bodies (level geometry). */
        bool static_only{false};
        /** Skip every capsule body (NPCs, the player's contact capsule) and
         *  everything a character carries: held items and the player's
         *  parts. A sight line from an NPC's eye must not be blocked by the
         *  rifle in its own hands. */
        bool skip_characters{false};
        /** FLOOR only: static geometry plus bodies marked walkable. Never a
         *  prop, an NPC or anything held -- a rifle carried at hip height is
         *  not something to stand on, and the floor probe that thought it
         *  was lifted an NPC 0.35 m every frame until it reached the sky. */
        bool floor_only{false};
    };

    bool raycast(const Vec3& origin, const Vec3& dir, float max_dist,
                 const RayFilter& filter, Vec3& out_hit, Vec3& out_normal,
                 std::uint32_t& out_entity) const;

    /** Sweep a sphere of ``radius`` from ``origin`` along ``dir`` for
     *  ``max_dist`` and report the first body it touches (a fat ray for
     *  arrows, rockets and grenades). Radius 0 is a raycast. */
    bool sphere_cast(const Vec3& origin, const Vec3& dir, float radius,
                     float max_dist, const RayFilter& filter, Vec3& out_hit,
                     Vec3& out_normal, std::uint32_t& out_entity) const;

private:
    /** Replace a static body with an identical movable one.
     *
     *  Jolt allocates MotionProperties at creation, so a static body cannot be
     *  promoted in place -- ``SetMotionType`` dereferences the null pointer
     *  and crashes. ``motion_type_raw`` is a ``JPH::EMotionType`` as an int to
     *  keep Jolt out of this header. Returns false if the body could not be
     *  rebuilt, in which case the original is already gone.
     */
    bool recreate_body_movable(std::uint32_t entity, int motion_type_raw);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ptv
