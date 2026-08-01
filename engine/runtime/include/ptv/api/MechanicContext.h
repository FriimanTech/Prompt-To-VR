#pragma once

#include <cstddef>
#include <cstdint>

#include "ptv/math/Math.h"

namespace ptv {

/**
 * Engine services handed to every mechanic.
 *
 * A mechanic normally only reads input (``InputSnapshot``) and reads/writes
 * the ECS (``World``). Some mechanics -- locomotion, grab, throw, anything
 * that shoots a ray or shoves a rigid body -- need more than that. Rather
 * than opening up the engine internals (``ptv/physics/``, the renderer, the
 * OpenXR session) to generated code, the engine exposes these three NARROW,
 * stable interfaces.
 *
 * Everything here is safe to call from generated code: the implementations
 * validate and CLAMP their inputs (see the notes on each method). A mechanic
 * cannot fling the player out of the world, teleport them under the floor, or
 * corrupt the physics world with a NaN, no matter what it passes in.
 */

/** Which style of locomotion currently owns the movement stick.
 *
 * Exactly one mode is active at a time. Locomotion mechanics should early-out
 * unless ``ILocomotion::active_mode()`` matches the mode they implement, so
 * several of them can be bound in the same scene without fighting over the
 * player rig. Snap turn is independent and always runs.
 */
enum class LocomotionMode : int {
    Teleport = 0,
    SlideHeadRelative = 1,
    SlideControllerRelative = 2,
};

/**
 * Comfort/tuning values authored in the scene's ``player`` block.
 *
 * A locomotion mechanic should default to these rather than inventing its
 * own numbers, and let its binding ``params`` override them. That way a scene
 * can dial comfort globally and an individual binding can still special-case
 * itself. Defaults below match the engine's historical values.
 */
struct LocomotionTunables {
    float slide_max_speed_mps{1.5f};
    float slide_accel_tau_s{0.12f};
    float slide_decel_tau_s{0.18f};
    float teleport_launch_speed_mps{6.0f};
    float teleport_max_horizontal_m{12.0f};
    float teleport_floor_y{0.0f};
    float snap_turn_degrees{45.0f};
    float snap_turn_cooldown_s{0.30f};
};

/**
 * Moves the player.
 *
 * The "player rig" is the transform of the stage origin in world space. The
 * physical head/hand poses reported by OpenXR are composed through it, so
 * moving the rig moves the player without touching any entity.
 */
class ILocomotion {
public:
    virtual ~ILocomotion() = default;

    /** Current rig origin in world space. */
    virtual Vec3 position() const = 0;
    /** Current rig yaw in radians. */
    virtual float yaw_radians() const = 0;

    /** Move the rig by ``delta`` (world space).
     *
     *  CLAMPED: non-finite values are rejected, and the per-frame magnitude is
     *  capped so a runaway mechanic cannot launch the player across the map. */
    virtual void translate(const Vec3& delta) = 0;

    /** Rotate the rig about world +Y.
     *
     *  CLAMPED: non-finite values are rejected. */
    virtual void rotate_yaw(float radians) = 0;

    /** Move the rig so the player stands at ``position``.
     *
     *  CLAMPED: non-finite values are rejected, the horizontal distance is
     *  capped to ``teleport_max_horizontal_m()``, and the destination is
     *  lifted to ``teleport_floor_y()`` if it would put the player under the
     *  floor. */
    virtual void teleport_to(const Vec3& position) = 0;

    /** Which locomotion style currently owns the movement stick. */
    virtual LocomotionMode active_mode() const = 0;
    /** Switch styles (the runtime locomotion menu calls this). */
    virtual void set_active_mode(LocomotionMode mode) = 0;

    /** True while something else owns the sticks (e.g. the locomotion menu is
     *  open). Locomotion mechanics must not move the player when this is set. */
    virtual bool input_locked() const = 0;

    /** Scene-authored comfort tunables, so a mechanic can default to what the
     *  scene asked for instead of hard-coding its own numbers. */
    virtual const LocomotionTunables& tunables() const = 0;
};

/** Result of :meth:`IPhysicsAccess::raycast`.
 *
 * Deliberately named ``PhysicsRayHit`` rather than the obvious ``RayHit``:
 * generated mechanics also live in namespace ``ptv`` and frequently declare
 * their own ``RayHit`` helper, which would make every such project ambiguous
 * the moment this header was added. */
struct PhysicsRayHit {
    Vec3 point{0.0f, 0.0f, 0.0f};
    Vec3 normal{0.0f, 1.0f, 0.0f};
    bool hit{false};
};

/**
 * Narrow facade over the physics world.
 *
 * Intentionally much smaller than ``PhysicsWorld``: just what gameplay needs.
 * Generated code must NOT include ``ptv/physics/`` or Jolt directly.
 */
class IPhysicsAccess {
public:
    virtual ~IPhysicsAccess() = default;

    /** Cast a ray against every physics body. ``dir`` need not be normalized. */
    virtual bool raycast(const Vec3& origin, const Vec3& dir, float max_dist,
                         PhysicsRayHit& out) const = 0;

    /** True when this entity has a physics body at all. */
    virtual bool has_body(std::uint32_t entity) const = 0;

    /** Switch a body between dynamic and kinematic. Grab uses this so a held
     *  object stops being affected by gravity while carried. */
    virtual void set_kinematic(std::uint32_t entity, bool kinematic) = 0;

    /** Sweep a kinematic body toward a pose. Call every frame while carrying
     *  so Jolt derives a velocity and contacts resolve properly.
     *
     *  CLAMPED: non-finite poses are rejected. */
    virtual void move_kinematic(std::uint32_t entity, const Vec3& pos,
                                const Quat& rot, float dt) = 0;

    /** Apply a world-space linear impulse at the centre of mass.
     *
     *  CLAMPED: non-finite values are rejected and the magnitude is capped, so
     *  a bad throw cannot rocket an object through the level. */
    virtual void apply_impulse(std::uint32_t entity, const Vec3& impulse) = 0;
};

/**
 * Immediate-mode debug/gameplay drawing for the current frame.
 *
 * Submitted geometry lasts exactly one frame, so just call it every frame
 * while the visual should be on screen. Used by teleport for its aim arc and
 * landing marker; also handy for generated mechanics that need a laser, a
 * highlight, or a trajectory preview.
 */
class IDebugDraw {
public:
    virtual ~IDebugDraw() = default;

    virtual void line(const Vec3& a, const Vec3& b, const Vec4& color,
                      float thickness = 0.02f) = 0;
    /** Connected line strip through ``count`` points. */
    virtual void polyline(const Vec3* points, std::size_t count,
                          const Vec4& color, float thickness = 0.02f) = 0;
    /** Flat disc marker, e.g. a teleport landing spot. */
    virtual void marker(const Vec3& center, float radius,
                        const Vec4& color) = 0;
};

/**
 * Which entity each hand is holding.
 *
 * ``InputSnapshot::HandState::held_entity`` is the read-only view of this,
 * and it is frozen mid-frame from exactly these values. A mechanic that
 * decides what is being held (i.e. grab) must run in ``MechanicPhase::Input``
 * and publish here; every Simulate-phase mechanic then observes the result on
 * the SAME frame instead of one frame late.
 *
 * Most mechanics should just read ``input.hands[h].held_entity`` and never
 * touch this.
 */
class IHands {
public:
    virtual ~IHands() = default;

    /** Entity held by ``hand`` (0 = left, 1 = right), or
     *  ``InputSnapshot::kInvalidEntity`` when empty. */
    virtual std::uint32_t held_entity(int hand) const = 0;

    /** Publish what ``hand`` is holding. Pass
     *  ``InputSnapshot::kInvalidEntity`` to clear. */
    virtual void set_held_entity(int hand, std::uint32_t entity) = 0;
};

/**
 * Bundle of engine services passed to every mechanic call.
 *
 * Pointers are owned by the engine and remain valid for the lifetime of the
 * scene. They are never null while a scene is loaded, but defensive code may
 * still check -- a headless/test host may supply an empty context.
 */
struct MechanicContext {
    ILocomotion* locomotion{nullptr};
    IPhysicsAccess* physics{nullptr};
    IDebugDraw* draw{nullptr};
    IHands* hands{nullptr};
};

}  // namespace ptv
