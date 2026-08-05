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

/** A one-shot visual displacement, expressed in the entity's local space.
 *
 * ``local_offset`` of ``{0, 0, -0.04}`` on a weapon kicks it 4 cm backward
 * along its own barrel axis regardless of how the player is holding it.
 */
struct TransformImpulse {
    Vec3 local_offset{0.0f, 0.0f, 0.0f};
    Vec3 local_euler_deg{0.0f, 0.0f, 0.0f};
    /** Exponential recovery time constant in seconds. ~0.06 reads as a crisp
     *  snap; ~0.25 reads as a heavy wobble. */
    float recover_tau_s{0.06f};
};

/**
 * Short-lived, purely visual transform "juice": recoil kick, hit flinch,
 * impact shove.
 *
 * Why this exists instead of writing ``CTransform`` directly: for any entity
 * with a physics body, ``CTransform`` is an OUTPUT of the physics step. The
 * engine overwrites it from the Jolt body every frame, and ``builtin.grab``
 * drives held objects by commanding physics (``move_kinematic``), not by
 * setting the transform. A mechanic that wrote a recoil offset into
 * ``CTransform`` would either have it erased on the next sync or silently
 * desync the drawn pose from the physical one that raycasts and collision
 * use.
 *
 * Impulses accumulate onto a per-entity offset that springs back to rest, and
 * that offset is applied only at draw time. Physics, grab, and collision
 * never see it, so nothing fights over ownership of the entity's position.
 *
 * This is also how a generated mechanic produces a hit reaction without any
 * damage-event system: whatever code detects the hit just calls ``impulse``.
 */
class ITransformFx {
public:
    virtual ~ITransformFx() = default;

    /** Nudge ``entity`` and let it spring back.
     *
     *  Repeated calls accumulate, so rapid fire compounds into a bigger kick
     *  rather than restarting the same one.
     *
     *  CLAMPED: non-finite values are rejected; the offset magnitude, the
     *  rotation magnitude and the recovery time constant are all capped so a
     *  runaway mechanic cannot fling a mesh across the level or freeze it in
     *  a displaced pose. */
    virtual void impulse(std::uint32_t entity, const TransformImpulse& kick) = 0;

    /** Snap ``entity`` back to rest immediately. */
    virtual void clear(std::uint32_t entity) = 0;
};

/**
 * Controller vibration.
 *
 * Haptics are the cheapest half of "that felt like it fired". Pair a short
 * pulse with a ``ctx.fx`` kick on the same frame and a weapon reads as having
 * real weight, with no animation and no moving parts.
 *
 * Fire-and-forget: submit a pulse and forget it, do NOT call every frame
 * while a button is held -- retriggering each frame produces a flat buzz
 * instead of a hit. Pulse on the EDGE.
 */
class IHaptics {
public:
    virtual ~IHaptics() = default;

    /** Vibrate one controller. ``hand`` is 0 = left, 1 = right.
     *
     *  CLAMPED: non-finite values are rejected, ``duration_s`` is capped at
     *  0.5 s, ``amplitude`` is clamped to 0..1, and each hand is rate-limited
     *  so a mechanic calling this every frame cannot lock the controller into
     *  a permanent buzz. */
    virtual void pulse(int hand, float duration_s, float amplitude) = 0;

    /** Same, with an explicit carrier frequency in Hz. The default used by
     *  ``pulse`` (about 160 Hz) is a solid generic "thump"; lower reads
     *  heavier, higher reads sharper. Pass 0 to let the runtime choose. */
    virtual void pulse_at(int hand, float duration_s, float frequency_hz,
                          float amplitude) = 0;
};

/**
 * Named attachment points on an entity's mesh.
 *
 * A socket is "the muzzle is here, pointing that way" -- a named local
 * transform baked into the asset by the deterministic ``asset_contract``
 * pipeline stage, which had the geometry open. You never see the mesh, so ask
 * for a socket BY NAME and let the data supply the numbers:
 *
 *     const Mat4 m = ctx.sockets->socket_world(self, "muzzle");
 *     const Vec3 origin    = Vec3(m[3]);  // position column
 *     const Vec3 direction = Vec3(m[2]);  // +Z is the socket's forward
 *
 * That same code fires from the correct point on a pistol, a rifle and a
 * rocket launcher. NEVER hard-code a muzzle offset: it is per-asset data you
 * cannot know, and a guessed offset puts the shot inside the player's palm.
 */
class ISockets {
public:
    virtual ~ISockets() = default;

    /** True when ``entity`` has a mesh carrying a socket called ``name``.
     *
     *  Worth checking for optional visuals. For a required socket just call
     *  ``socket_world`` -- the fallback below is safe, and the scene compiler
     *  already refused to build a scene whose mechanic declared a
     *  ``required_sockets`` entry the asset does not have. */
    virtual bool has_socket(std::uint32_t entity,
                            const char* name) const = 0;

    /** World transform of ``name`` on ``entity``: the entity's own transform
     *  composed with the socket's local one.
     *
     *  FALLBACK: an unknown entity, an entity with no mesh, or an unknown
     *  socket returns a pose on the forward face of the entity's bounding
     *  sphere rather than identity -- so a missing socket looks slightly
     *  wrong instead of spawning projectiles at the world origin. */
    virtual Mat4 socket_world(std::uint32_t entity,
                              const char* name) const = 0;
};

/**
 * Pose a rigged entity: play a baked clip, or drive individual joints.
 *
 * Only entities whose GLB carried a skin have a skeleton; everything else
 * returns false / -1 and ignores writes, so calling this on a static prop is
 * harmless.
 *
 * Two ways to move a joint, and they compose:
 *
 *   * ``play_clip`` runs motion baked into the asset -- a walk cycle from
 *     Tripo's animation retarget, say. Time-driven.
 *   * ``set_joint_rotation`` states a joint's pose for THIS frame, computed
 *     from whatever you like. This is how a hand's index finger follows the
 *     trigger:
 *
 *         const int j = ctx.skeleton->joint_index(self, "bone_index_01");
 *         const float curl = input.hands[hand].trigger;   // 0..1
 *         ctx.skeleton->set_joint_rotation(
 *             self, j, glm::angleAxis(curl * 1.4f, Vec3(0, 0, 1)));
 *
 * A joint you write is left alone by the clip that frame; a joint you stop
 * writing goes straight back to the clip (or to bind pose). So a character
 * can play its idle animation while code aims only its head.
 *
 * Writes must happen in ``Phase::Simulate``: the pose system runs after that
 * phase and before the renderer builds its draw list.
 */
class ISkeleton {
public:
    virtual ~ISkeleton() = default;

    /** True when ``entity`` has a skeleton at all. */
    virtual bool has_skeleton(std::uint32_t entity) const = 0;

    /** Index of the joint called ``name``, or -1.
     *
     *  Names come from the rig, so they depend on the ``spec`` the asset was
     *  rigged with (Mixamo names look like ``mixamorig:RightHandIndex1``).
     *  Resolve once in ``on_attach`` and cache the index -- this is a string
     *  lookup, not something to do per joint per frame. */
    virtual int joint_index(std::uint32_t entity, const char* name) const = 0;

    /** Number of joints, or 0. */
    virtual int joint_count(std::uint32_t entity) const = 0;

    /** Override one joint's local rotation for this frame. */
    virtual void set_joint_rotation(std::uint32_t entity, int joint,
                                    const Quat& rotation) = 0;
    /** Override one joint's local translation for this frame. Rarely what you
     *  want -- moving a joint off its rig position stretches the mesh. */
    virtual void set_joint_translation(std::uint32_t entity, int joint,
                                       const Vec3& translation) = 0;

    /** Start a clip by name. Returns false when the asset has no such clip.
     *  Restarts from zero if that clip is already the active one. */
    virtual bool play_clip(std::uint32_t entity, const char* clip_name,
                           bool looping) = 0;
    /** Stop clip playback and hold the current pose. */
    virtual void stop_clip(std::uint32_t entity) = 0;
    /** Playback rate multiplier, clamped by the engine to a sane range. */
    virtual void set_clip_speed(std::uint32_t entity, float speed) = 0;
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
    ITransformFx* fx{nullptr};
    IHaptics* haptics{nullptr};
    ISockets* sockets{nullptr};
    ISkeleton* skeleton{nullptr};
};

}  // namespace ptv
