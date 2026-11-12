#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

#include "ptv/api/Effects.h"
#include "ptv/api/GameFlow.h"
#include "ptv/api/InteractiveState.h"
#include "ptv/api/Projectiles.h"
#include "ptv/api/Weapons.h"
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

/** Which turning style owns the right stick. Same rule as LocomotionMode:
 *  a turn mechanic early-outs unless ``ILocomotion::turn_mode()`` is its
 *  own, so snap and smooth turning can both be bound and the player can
 *  switch between them in a settings menu. */
enum class TurnMode : int {
    Snap = 0,
    Smooth = 1,
};

/** Who owns the player's height.
 *
 *  ``Grounded`` is the default and the right answer for walking, running,
 *  crouching and jumping: gravity pulls the body down, the feet follow the
 *  terrain up ramps and stairs, and the vertical part of any horizontal move
 *  is ignored -- so a locomotion mechanic cannot accidentally float the
 *  player off the ground. ``Free`` hands vertical motion to the caller, for
 *  flying, swimming or climbing mechanics that genuinely move the player up
 *  and down. */
enum class VerticalMode : int {
    Grounded = 0,
    Free = 1,
};

/** How the player starts running on the slide. ``Hold``: keep the left
 *  stick pressed in. ``Toggle``: press it once; running ends when the stick
 *  returns to centre. ``Off``: walk only. Player-selectable at runtime. */
enum class SprintActivation : int {
    Off = 0,
    Hold = 1,
    Toggle = 2,
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
    /** Walking pace of the slide: full stick without sprint. */
    float slide_max_speed_mps{1.5f};
    float slide_accel_tau_s{0.12f};
    float slide_decel_tau_s{0.18f};
    /** Running pace: full sprint, reached by holding the left stick in. */
    float sprint_speed_mps{3.0f};
    /** False when the scene turned sprint off: the settings menu then
     *  offers no sprint choice. */
    bool sprint_allowed{true};
    float sprint_accel_tau_s{0.35f};
    float sprint_decel_tau_s{0.25f};
    /** Sprint only while the stick points within this angle of straight
     *  forward, and is pushed at least ``sprint_min_stick``. */
    float sprint_forward_cone_deg{70.0f};
    float sprint_min_stick{0.6f};
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

    /** Move the player by ``delta`` (world space).
     *
     *  The move is swept against the world through the player's body, so it
     *  stops at walls from every direction. In ``VerticalMode::Grounded``
     *  (the default) only the horizontal part is used and the body stays on
     *  the ground; the vertical part is applied only in ``Free`` mode.
     *
     *  CLAMPED: non-finite values are rejected, and the per-frame magnitude is
     *  capped so a runaway mechanic cannot launch the player across the map. */
    virtual void translate(const Vec3& delta) = 0;

    /** Same as ``translate`` with the vertical part discarded. Prefer this
     *  for walking: it says what it means. */
    virtual void move_horizontal(const Vec3& delta) = 0;

    /** True while the player's feet are on something walkable. */
    virtual bool is_grounded() const = 0;

    /** Launch the player upward at ``speed_mps`` if they are grounded.
     *  Gravity brings them back down. Ignored mid-air and in Free mode. */
    virtual void jump(float speed_mps) = 0;

    virtual void set_vertical_mode(VerticalMode mode) = 0;
    virtual VerticalMode vertical_mode() const = 0;

    /** Where the player's feet are: under the headset, on the ground.
     *  Differs from ``position()`` (the stage origin) whenever the player
     *  has walked away from the centre of their playspace. */
    virtual Vec3 body_position() const = 0;

    /** The headset's world pose right now, including any rig movement made
     *  earlier this frame. */
    virtual Vec3 head_position() const = 0;
    virtual Quat head_rotation() const = 0;

    /** Which turning style owns the right stick. */
    virtual TurnMode turn_mode() const = 0;
    virtual void set_turn_mode(TurnMode mode) = 0;

    /** Locomotion and turn mechanics announce themselves here when they
     *  attach, so a settings menu offers exactly the options this game has
     *  and the engine never leaves the player in a mode nothing implements. */
    virtual void declare_mode_available(LocomotionMode mode) = 0;
    virtual bool mode_available(LocomotionMode mode) const = 0;
    virtual void declare_turn_available(TurnMode mode) = 0;
    virtual bool turn_available(TurnMode mode) const = 0;

    /** Rotate the rig about world +Y.
     *
     *  CLAMPED: non-finite values are rejected. */
    virtual void rotate_yaw(float radians) = 0;

    /** Move the player so they stand at ``position``: their FEET land
     *  there, directly under their head, wherever they are standing in their
     *  playspace.
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

    /** True while something else owns the sticks (e.g. a settings menu is
     *  open). Locomotion mechanics must not move the player when this is set. */
    virtual bool input_locked() const = 0;

    /** Claim or release the sticks for a menu. Reference counted by owner
     *  id, so two panels closing in either order leave the sticks free. */
    virtual void set_input_lock(const char* owner, bool locked) = 0;

    /** Scene-authored comfort tunables, so a mechanic can default to what the
     *  scene asked for instead of hard-coding its own numbers. */
    virtual const LocomotionTunables& tunables() const = 0;

    // ------------------------------------------------------------------
    // Walk / run. The built-in slide owns sprinting and publishes it here,
    // so the avatar's legs (and any generated mechanic) can tell a run
    // from a fast walk without re-reading the stick.
    // ------------------------------------------------------------------

    /** True while the player is running (sprint mostly engaged). */
    virtual bool sprinting() const { return false; }
    /** The slide's current top speed: the walk pace, rising towards the
     *  run pace while sprinting. */
    virtual float gait_speed_cap() const {
        return tunables().slide_max_speed_mps;
    }
    /** Called by the slide each frame it owns the stick. */
    virtual void publish_sprint(bool sprinting, float speed_cap) {
        (void)sprinting;
        (void)speed_cap;
    }
    /** The player's chosen activation (settings menu), seeded from the
     *  scene's ``player.sprint.activation``. */
    virtual SprintActivation sprint_activation() const {
        return SprintActivation::Off;
    }
    virtual void set_sprint_activation(SprintActivation mode) { (void)mode; }

    // ------------------------------------------------------------------
    // Reference frame: what the player's coordinates are relative to.
    //
    // World space is the special case where there is no parent. Once there
    // is one, the parent's motion carries the player along WITHOUT their
    // local position drifting -- which is the property that matters.
    // Standing still on a moving platform means zero stick input and still
    // arriving somewhere else.
    //
    // One generic feature rather than several specific ones: vehicles,
    // elevators, boats, lifts, moving platforms, and a miniature player
    // riding inside an NPC's head are all the same problem, so none of them
    // needs engine code of its own.
    // ------------------------------------------------------------------

    /** Attach the player to ``entity``'s ``anchor`` (a body slot or socket
     *  name; empty means the entity's own transform).
     *
     *  Naming a joint matters for anything animated: a passenger inside a
     *  head must ride the ANIMATED skull, not the entity's root transform,
     *  which for a walking character sits at its feet and does not turn with
     *  the neck.
     *
     *  Boarding does not move the player -- their local offset is recomputed
     *  so they stay exactly where they were. */
    virtual void set_reference_frame(std::uint32_t entity,
                                     const char* anchor) = 0;

    /** Detach, leaving the player where they are in world space. */
    virtual void clear_reference_frame() = 0;

    /** The entity the player is riding, or ``0xFFFFFFFF``. */
    virtual std::uint32_t reference_frame() const = 0;

    /** Resize the player. 1.0 is life size; 0.02 is a two-centimetre
     *  passenger inside a skull. Clamped to a usable range.
     *
     *  The RIG scales, never the world: the world is what physics,
     *  navigation and every authored dimension agree on, and rescaling it
     *  would invalidate all of them at once. Clip planes, the player's
     *  capsule and their tracked motion all follow this. */
    virtual void set_player_scale(float scale) = 0;
    virtual float player_scale() const = 0;
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
    /** The entity that was hit, or ``0xFFFFFFFF`` when the ray landed on
     *  something with no entity behind it. Shooting and perception need to
     *  know WHAT was hit, not just where. */
    std::uint32_t entity{0xFFFFFFFFu};
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

    /** Cast a ray on behalf of ``self``, passing through its own body.
     *
     *  Use this for any probe that starts on or inside the caster: convex
     *  shapes are solid, so a plain ``raycast`` from an NPC's chest or head
     *  hits that NPC's own capsule at distance zero. ``floor_only`` (a
     *  historical name) skips every character and everything characters
     *  carry -- capsules, held items, the player's hands -- which is what a
     *  sight line or a whisker wants. For the FLOOR use ``raycast_floor``. */
    virtual bool raycast_excluding(std::uint32_t self, const Vec3& origin,
                                   const Vec3& dir, float max_dist,
                                   PhysicsRayHit& out,
                                   bool floor_only = false) const {
        (void)self;
        (void)floor_only;
        return raycast(origin, dir, max_dist, out);
    }

    /** The FLOOR along a ray: static level geometry plus bodies the scene
     *  marked walkable, never a prop, a character or anything carried.
     *  Every ground probe uses this; a probe that could hit the rifle in an
     *  NPC's hands stood the NPC on it, every frame, all the way to the
     *  sky. */
    virtual bool raycast_floor(std::uint32_t self, const Vec3& origin, const Vec3& dir,
                               float max_dist, PhysicsRayHit& out) const {
        return raycast_excluding(self, origin, dir, max_dist, out, true);
    }

    /** True when this entity has a physics body at all. */
    /** Raycast that passes through every entity in ``ignore`` (``count`` of
     *  them): the weapon, the hands holding it and the player, so a shot
     *  fired from a muzzle that sits on or inside the weapon's own collider
     *  does not hit the weapon at distance zero. */
    virtual bool raycast_ignoring(const std::uint32_t* ignore, int count,
                                  const Vec3& origin, const Vec3& dir,
                                  float max_dist, PhysicsRayHit& out) const {
        return raycast_excluding(count > 0 ? ignore[0] : 0xFFFFFFFFu, origin, dir,
                                 max_dist, out);
    }

    /** Sweep a sphere of ``radius`` (a fat ray: arrows, rockets, grenades),
     *  ignoring the listed entities. Radius 0 behaves as
     *  ``raycast_ignoring``. */
    virtual bool sphere_cast_ignoring(const std::uint32_t* ignore, int count,
                                      const Vec3& origin, const Vec3& dir,
                                      float radius, float max_dist,
                                      PhysicsRayHit& out) const {
        (void)radius;
        return raycast_ignoring(ignore, count, origin, dir, max_dist, out);
    }

    virtual bool has_body(std::uint32_t entity) const = 0;

    /** Distance from ``point`` to ``entity``'s collider (an oriented box
     *  around it): 0 when inside, negative when it has no body. Grab reach
     *  is measured to this, so a long rifle can be taken by its stock. */
    virtual float distance_to_body(std::uint32_t entity, const Vec3& point) const {
        (void)entity;
        (void)point;
        return -1.0f;
    }

    /** Take ``entity``'s body out of the simulation (``false``) or put it
     *  back (``true``): a destroyed target that will respawn, a door that
     *  opens a gap. Nothing touches or hits a disabled body. */
    virtual void set_collision_enabled(std::uint32_t entity, bool enabled) {
        (void)entity;
        (void)enabled;
    }

    /** Centre of the entity's collider in world space -- where to aim at it,
     *  whatever its mesh origin is (feet, middle or wherever the GLB put
     *  it). False when it has no body. */
    virtual bool body_center(std::uint32_t entity, Vec3* out) const {
        (void)entity;
        (void)out;
        return false;
    }

    /** Switch a body between dynamic and kinematic. Grab uses this so a held
     *  object stops being affected by gravity while carried. */
    virtual void set_kinematic(std::uint32_t entity, bool kinematic) = 0;

    /** Sweep a kinematic body toward a pose. Call every frame while carrying
     *  so Jolt derives a velocity and contacts resolve properly.
     *
     *  CLAMPED: non-finite poses are rejected. */
    virtual void move_kinematic(std::uint32_t entity, const Vec3& pos,
                                const Quat& rot, float dt) = 0;

    /** Put a body at a pose instantly, at rest: a restocked grenade back
     *  on its table, a reset target. Not for carrying -- that is
     *  move_kinematic, which keeps contacts honest.
     *
     *  CLAMPED: non-finite poses are rejected. */
    virtual void teleport_body(std::uint32_t entity, const Vec3& pos, const Quat& rot) {
        (void)entity;
        (void)pos;
        (void)rot;
    }

    /** Apply a world-space linear impulse at the centre of mass.
     *
     *  CLAMPED: non-finite values are rejected and the magnitude is capped, so
     *  a bad throw cannot rocket an object through the level. */
    virtual void apply_impulse(std::uint32_t entity, const Vec3& impulse) = 0;

    /** Resolve a walking move for ``entity`` against the world.
     *
     *  Sweeps the entity's own capsule through ``delta`` and returns the
     *  movement actually available: the whole thing in the open, or a
     *  shortened, wall-parallel version when something is in the way. This is
     *  what stops a character that is told to walk at a wall from walking
     *  through it, and what lets one brushing past a corner keep going
     *  instead of sticking.
     *
     *  Prefer this over adding to ``CTransform`` directly for anything that
     *  walks. Projectiles and thrown objects should use real physics
     *  instead. */
    virtual Vec3 slide_move(std::uint32_t entity, const Vec3& delta) const = 0;
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

/** How a line of world-space text is placed relative to its anchor. */
enum class TextAlign : int { Left = 0, Center = 1, Right = 2 };

/**
 * World-space text for the current frame.
 *
 * Signed-distance-field glyphs from the engine's bundled font, drawn
 * unlit and alpha-tested so they stay crisp at any distance on both
 * Quest and PC VR. Like IDebugDraw, a call lasts one frame: call it every
 * frame while the text should show.
 *
 * Size text by the ANGLE it subtends, not by metres: readable VR body text
 * is roughly 1.3-1.5 degrees tall. ``height_for_angle`` turns a distance and
 * an angle into the ``height_m`` to pass.
 */
class ITextDraw {
public:
    virtual ~ITextDraw() = default;
    /** Draw ``utf8`` (printable ASCII; anything else shows as '?') with its
     *  first baseline at ``position``, lines split on '\n',
     *  facing along ``rotation``'s +Z (so a panel facing the player passes
     *  the panel's own rotation). ``height_m`` is the cap height. */
    virtual void text(const char* utf8, const Vec3& position,
                      const Quat& rotation, float height_m, const Vec4& color,
                      TextAlign align = TextAlign::Center) = 0;
    /** Width in metres ``utf8`` would occupy at ``height_m``. */
    virtual float measure(const char* utf8, float height_m) const = 0;
    static float height_for_angle(float distance_m, float degrees) {
        return 2.0f * distance_m * std::tan(degrees * 0.00872664626f);
    }
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

    /** The player's own hand entity for ``hand``, or
     *  ``InputSnapshot::kInvalidEntity``.
     *
     *  These are real ECS entities carrying the rigged hand mesh, which is
     *  what lets a mechanic pose the fingers through ``ctx.skeleton``. They
     *  do not exist when the engine falls back to the procedural block hands
     *  (no generated hand asset on disk), so always check the return value.
     *
     *  Do NOT grab, throw or apply physics to these -- they follow the
     *  controller and are not simulated. */
    virtual std::uint32_t hand_entity(int hand) const = 0;

    /** The skeleton whose fingers the player actually SEES on ``hand``, or
     *  ``InputSnapshot::kInvalidEntity`` when that hand has no posable
     *  fingers.
     *
     *  With the engine's default hands this is ``hand_entity``. With a
     *  full-body avatar it is the avatar itself, set by
     *  ``builtin.avatar_body`` once it has found finger bones on that side.
     *  ``builtin.hand_pose`` curls exactly this entity and nothing else, so
     *  a hidden pair of hands can never be the one being driven while the
     *  visible fingers stay frozen. */
    virtual std::uint32_t finger_target(int hand) const = 0;
    virtual void set_finger_target(int hand, std::uint32_t entity) = 0;

    /** World position of a fingertip of ``hand`` as DRAWN: "thumb", "index",
     *  "middle", "ring" or "pinky". From the visible skeleton's last finger
     *  joint, extended by the distal segment; with no posable fingers it is
     *  estimated from the controller pose. Poke buttons read the index tip.
     *  False only when the hand is not tracked. */
    virtual bool fingertip_world(int hand, const char* finger, Vec3* out) const {
        (void)hand;
        (void)finger;
        (void)out;
        return false;
    }

    /** Draw ``hand`` at this GRIP pose instead of the controller's, blended
     *  by ``weight`` (0..1). ``builtin.grab`` sets it every frame a hand
     *  holds something by a socket, so the visible hand closes exactly on
     *  the handle, the second hand wraps the first, or the support hand sits
     *  under a handguard. Valid for the frame it is set in; not setting it
     *  hands the visual back to the controller. Affects drawing only
     *  (default hands and a full-body avatar's IK target), never input. */
    virtual void set_hand_anchor(int hand, const Vec3& grip_position,
                                 const Quat& grip_rotation, float weight) {
        (void)hand; (void)grip_position; (void)grip_rotation; (void)weight;
    }
    /** This frame's anchor, if any. */
    virtual bool hand_anchor(int hand, Vec3* grip_position, Quat* grip_rotation,
                             float* weight) const {
        (void)hand; (void)grip_position; (void)grip_rotation; (void)weight;
        return false;
    }

    /** World direction from the palm toward the elbow: from the avatar's
     *  forearm when a full body drives the arm, otherwise from the hand
     *  rig's own wrist. Used to lay a strapped shield along the forearm. */
    virtual Vec3 forearm_direction(int hand) const {
        (void)hand;
        return Vec3(0.0f, 0.0f, 1.0f);
    }
    /** ``builtin.avatar_body`` publishes the measured forearm direction. */
    virtual void publish_forearm_direction(int hand, const Vec3& dir) {
        (void)hand; (void)dir;
    }

    /** The two-handed item ``hand`` is steadying as the SUPPORT hand (the
     *  other hand holds it), or the invalid sentinel. Weapons that require
     *  two hands read this. Published by ``builtin.grab``. */
    virtual std::uint32_t support_entity(int hand) const {
        (void)hand;
        return 0xFFFFFFFFu;
    }
    virtual void set_support_entity(int hand, std::uint32_t entity) {
        (void)hand; (void)entity;
    }

    /** Ask ``builtin.grab`` to put ``entity`` straight into ``hand`` next
     *  frame (an arrow drawn from a quiver, a magazine from a pouch), as if
     *  the player had squeezed on it. ``take_hold_request`` is grab's side:
     *  it returns and clears the pending request. */
    virtual void request_hold(int hand, std::uint32_t entity) {
        (void)hand; (void)entity;
    }
    virtual std::uint32_t take_hold_request(int hand) {
        (void)hand;
        return 0xFFFFFFFFu;
    }

    /** The player's own body: a kinematic capsule that follows the headset.
     *
     *  This is how gameplay refers to the user as an ordinary entity -- to
     *  damage them, to chase them, to test whether something touched them.
     *  It always exists, so unlike ``hand_entity`` it never returns the
     *  invalid sentinel. */
    virtual std::uint32_t player_entity() const = 0;
};

/**
 * Who is touching whom, this frame.
 *
 * Use this for "did the enemy reach the player", "did the projectile land",
 * "is the player standing in the trigger zone" -- anything that is about two
 * colliders intersecting rather than about a ray.
 *
 * These are narrow-phase overlap queries, not physics callbacks, and that is
 * deliberate: Jolt only reports contacts when at least one body is DYNAMIC,
 * but the two things gameplay most wants to know about touching -- the player
 * and an NPC -- are both KINEMATIC, because their motion is authored rather
 * than simulated. An overlap query works for every motion type.
 *
 * ``began`` is edge-triggered and only meaningful if you call it EVERY frame
 * for the same pair: it answers "touching now but not on the previous frame",
 * and the previous-frame answer is only recorded for pairs that were asked
 * about. A mechanic that polls intermittently should use ``touching`` and
 * track its own edges.
 */
class IContacts {
public:
    virtual ~IContacts() = default;

    /** True while the two entities' colliders intersect. */
    virtual bool touching(std::uint32_t a, std::uint32_t b) const = 0;

    /** True only on the frame the overlap starts. Poll every frame. */
    virtual bool began(std::uint32_t a, std::uint32_t b) = 0;

    /** Write up to ``max_out`` entities currently overlapping ``entity``
     *  into ``out``, returning how many were written. */
    virtual int overlapping(std::uint32_t entity, std::uint32_t* out,
                            int max_out) const = 0;
};

/**
 * Create and destroy entities while the game is running.
 *
 * Cloning a TEMPLATE rather than describing an entity from scratch is
 * deliberate. A mechanic cannot load a GLB, build a skeleton, or size a
 * collider -- all of that is decided by the asset pipeline and baked into
 * ``scene.json``. So the scene authors one hidden, fully-formed copy of each
 * spawnable thing (tagged ``"template"``, which keeps it from rendering) and
 * gameplay stamps out instances of it. The clone carries the template's mesh,
 * LOD chain, sockets, collider, skeleton, animation clips, tags and mechanic
 * bindings, and -- unlike an entity created by hand -- it is registered with
 * physics and with the mechanic host, so it collides and its mechanics tick
 * from the very next frame.
 */
class ISpawn {
public:
    virtual ~ISpawn() = default;

    /** Clone ``template_entity`` at a pose. Returns the new entity, or
     *  ``0xFFFFFFFF`` if the template is invalid or the scene is at its
     *  spawn budget. */
    virtual std::uint32_t instance(std::uint32_t template_entity,
                                   const Vec3& position,
                                   const Quat& rotation) = 0;

    /** Destroy a spawned entity: detaches its mechanics, removes its physics
     *  body, and frees it. Safe on an already-dead entity. */
    virtual void despawn(std::uint32_t entity) = 0;

    /** How many live clones of ``template_entity`` currently exist. Wave
     *  spawners use this to respect a concurrency cap instead of counting
     *  by hand. */
    virtual int live_count(std::uint32_t template_entity) const = 0;
};

/** Where an encounter (the scene's NPC waves) is in its life. */
enum class EncounterPhase : int {
    /** Configured but not started: waiting for its start trigger. */
    Idle = 0,
    /** A wave is in progress: NPCs are spawning and/or alive. */
    Active = 1,
    /** Between waves, counting down to the next one. */
    InterWave = 2,
    /** Every wave cleared. */
    Victory = 3,
    /** The player died; spawning has stopped. */
    Failed = 4,
};

/** The state ``IEncounters`` reports, written by the encounter director.
 *  Engine-internal: generated code reads it through ``IEncounters``. */
struct EncounterState {
    bool configured{false};
    EncounterPhase phase{EncounterPhase::Idle};
    int wave_index{0};
    int wave_count{0};
    int alive{0};
    int remaining_in_wave{0};
    int defeated_total{0};
    float countdown_s{0.0f};
    std::uint64_t wave_started_seq{0};
    std::uint64_t wave_cleared_seq{0};
    std::uint64_t victory_seq{0};
    bool paused{false};
    // Commands, consumed by the director.
    bool start_requested{false};
    bool reset_requested{false};
};

/**
 * The scene's NPC population: who spawns, when, and how many are left.
 *
 * Owned by ``builtin.encounter_director``, which reads the scene's encounter
 * plan (waves of NPC types, spawn zones, caps) and clones the hidden
 * templates through ``ISpawn``. Generated code does NOT spawn or pool NPCs
 * itself: it reads this to drive a wave HUD or mission flow, and may start,
 * pause or reset the encounter. ``configured()`` is false in a scene with no
 * encounter plan, and every other call is then inert.
 */
class IEncounters {
public:
    virtual ~IEncounters() = default;

    virtual bool configured() const = 0;
    virtual EncounterPhase phase() const = 0;
    /** 0-based index of the current (or last finished) wave. */
    virtual int wave_index() const = 0;
    virtual int wave_count() const = 0;
    /** Living NPCs this encounter spawned. */
    virtual int alive() const = 0;
    /** NPCs of the current wave not yet defeated (spawned or still to come). */
    virtual int remaining_in_wave() const = 0;
    virtual int defeated_total() const = 0;
    /** Seconds until the next wave while ``InterWave``, or until the
     *  delayed start while ``Idle``; 0 otherwise. */
    virtual float countdown_s() const = 0;
    /** Monotonic counters: compare with your last seen value to detect the
     *  event once. */
    virtual std::uint64_t wave_started_seq() const = 0;
    virtual std::uint64_t wave_cleared_seq() const = 0;
    virtual std::uint64_t victory_seq() const = 0;

    /** Begin now (an encounter whose start trigger is ``signal`` waits for
     *  this; any other trigger is simply pre-empted). */
    virtual void start() = 0;
    virtual void pause(bool paused) = 0;
    virtual bool paused() const = 0;
    /** Despawn everything this encounter spawned and wait for the start
     *  trigger again (a "play again" button). */
    virtual void reset() = 0;

    /** Engine-internal: the state the director writes. Generated code must
     *  not use it. */
    virtual EncounterState* director_state() { return nullptr; }
};

/** The engine's IEncounters: a view over one EncounterState. */
class EncounterBoard final : public IEncounters {
public:
    bool configured() const override { return s_.configured; }
    EncounterPhase phase() const override { return s_.phase; }
    int wave_index() const override { return s_.wave_index; }
    int wave_count() const override { return s_.wave_count; }
    int alive() const override { return s_.alive; }
    int remaining_in_wave() const override { return s_.remaining_in_wave; }
    int defeated_total() const override { return s_.defeated_total; }
    float countdown_s() const override { return s_.countdown_s; }
    std::uint64_t wave_started_seq() const override { return s_.wave_started_seq; }
    std::uint64_t wave_cleared_seq() const override { return s_.wave_cleared_seq; }
    std::uint64_t victory_seq() const override { return s_.victory_seq; }
    void start() override { s_.start_requested = true; }
    void pause(bool paused) override { s_.paused = paused; }
    bool paused() const override { return s_.paused; }
    void reset() override { s_.reset_requested = true; }
    EncounterState* director_state() override { return &s_; }
    /** Back to unconfigured, for a scene change. */
    void clear() { s_ = EncounterState{}; }

private:
    EncounterState s_{};
};

/**
 * Rigidly parent one entity to a point on another.
 *
 * The case this exists for: the asset pipeline produced a goblin and a rifle
 * as two separate meshes, and the goblin needs to be holding the rifle. It
 * also covers a scope clipped to a weapon, a hat on an NPC, or a pickup
 * stuck to a moving platform.
 *
 * The anchor is resolved by name, in this order:
 *   1. a named socket on the parent (``muzzle``, ``grip``) -- and on a rigged
 *      parent those already follow the animated skeleton;
 *   2. a joint on the parent's rig, by exact name;
 *   3. a common alias (``hand_r``, ``hand_l``, ``head``, ``spine``) matched
 *      loosely against the rig's joint names, because Tripo names bones and
 *      generated code never sees the asset.
 *
 * An attached child is driven entirely by its parent: its physics body is
 * switched to kinematic and its transform is overwritten every frame after
 * the pose system runs, so do not also write its ``CTransform``.
 */
class IAttach {
public:
    virtual ~IAttach() = default;

    /** Parent ``child`` to ``anchor`` on ``parent``. Returns false when the
     *  anchor cannot be resolved, leaving the child untouched. */
    virtual bool attach(std::uint32_t child, std::uint32_t parent,
                        const char* anchor, const Vec3& local_position,
                        const Quat& local_rotation) = 0;

    /** Put a held item in an NPC's hand BY ITS OWN HAND SOCKET: the child's
     *  ``child_socket`` (a hand_grip, in the OpenXR grip frame) is made to
     *  coincide with the grip frame of the parent's hand joint ``anchor``
     *  (``hand_r`` / ``hand_l``, measured from the rig's palm). For an
     *  anchor that is not a recognisable hand, the joint's own frame is
     *  used. Returns false when the socket or anchor cannot be resolved. */
    virtual bool attach_by_socket(std::uint32_t child, const char* child_socket,
                                  std::uint32_t parent, const char* anchor) {
        (void)child; (void)child_socket; (void)parent; (void)anchor;
        return false;
    }

    /** Release a child. It keeps its current world pose and, if it had a
     *  dynamic body before being attached, starts simulating again. */
    virtual void detach(std::uint32_t child) = 0;

    /** What ``child`` is attached to, or ``0xFFFFFFFF``. */
    virtual std::uint32_t parent_of(std::uint32_t child) const = 0;
};

/**
 * Damage, health and factions.
 *
 * These are engine services rather than something each game re-derives
 * because damage only works if everyone agrees on it. An enemy authored by
 * one CodeTask has to be able to hurt a player set up by another, and an
 * allied NPC has to recognise that enemy as an opponent without the two
 * tasks having agreed on a component layout in advance.
 *
 * Death is handled for you: dropping an entity to zero health plays its
 * ``die`` clip if it has one, stops it colliding, and removes it once the
 * animation has finished. A mechanic does not need to schedule any of that.
 */
/** One hit that landed, as recorded by :meth:`ICombat::apply_damage`. */
struct DamageEvent {
    /** Strictly increasing across the whole game; never 0 for a real hit. */
    std::uint64_t seq{0};
    std::uint32_t target{0xFFFFFFFFu};
    std::uint32_t instigator{0xFFFFFFFFu};
    float amount{0.0f};
    float health_after{0.0f};
    /** Where the instigator stood when the hit landed (the target's own
     *  position when there is no instigator) -- for a directional hit cue. */
    Vec3 source_position{0.0f, 0.0f, 0.0f};
    bool killed{false};
};

class ICombat {
public:
    virtual ~ICombat() = default;

    /** ``seq`` of the most recent hit that landed on ``entity``; 0 = never.
     *
     *  This is how a health bar, a damage vignette or a game-over flow
     *  learns that the player was hit, WHOEVER hit them: a built-in enemy,
     *  another task's projectile, a trap. Read it every frame and compare
     *  with the value you saw last; never invent a private hit inbox that
     *  only your own code writes to. */
    virtual std::uint64_t damage_seq(std::uint32_t entity) const {
        (void)entity;
        return 0;
    }

    /** Copy up to ``max_out`` hits on ``entity`` (``0xFFFFFFFF`` = any
     *  target) with ``seq > after_seq``, oldest first. Returns how many were
     *  written. The engine keeps the most recent 64 hits. */
    virtual int damage_events_since(std::uint32_t entity, std::uint64_t after_seq,
                                    DamageEvent* out, int max_out) const {
        (void)entity;
        (void)after_seq;
        (void)out;
        (void)max_out;
        return 0;
    }

    /** Deal damage. Returns true if it landed -- false when the target has
     *  no health, is already dead, or is still inside the invulnerability
     *  window from its last hit.
     *
     *  That window matters more than it looks: contact damage is tested
     *  every frame, so without it a single touch from an enemy empties a
     *  health bar in a few frames. */
    virtual bool apply_damage(std::uint32_t target, float amount,
                              std::uint32_t instigator) = 0;

    /** Damage from a weapon hit (a bullet, a beam tick, a sword swing).
     *  Unlike ``apply_damage`` it is NOT swallowed by an NPC's post-hit
     *  invulnerability window -- that window exists for contact damage
     *  tested every frame, and it would turn a 700 rpm rifle into one hit
     *  every 0.4 s. Hits on the player still respect it. */
    virtual bool apply_weapon_damage(std::uint32_t target, float amount,
                                     std::uint32_t instigator) {
        return apply_damage(target, amount, instigator);
    }

    /** Register a held shield (engine built-ins): melee hits whose path
     *  from the attacker to the holder passes through it are reduced to
     *  ``leak`` of their damage. ``enabled`` false unregisters. */
    virtual void set_blocker(std::uint32_t shield, bool enabled, float leak) {
        (void)shield; (void)enabled; (void)leak;
    }
    /** Hits a shield has blocked so far (telemetry / feedback). */
    virtual std::uint64_t blocked_seq(std::uint32_t shield) const {
        (void)shield;
        return 0;
    }

    /** Restore health, never above the maximum. */
    virtual void heal(std::uint32_t entity, float amount) = 0;

    virtual float health(std::uint32_t entity) const = 0;
    virtual float max_health(std::uint32_t entity) const = 0;

    /** False for a dead entity, one that is playing its death animation, or
     *  one that never had health at all. */
    virtual bool is_alive(std::uint32_t entity) const = 0;

    /** Kill outright, skipping damage and invulnerability. */
    virtual void kill(std::uint32_t entity) = 0;

    /** Bring a dead (or dying) entity back with ``health`` (<= 0 = full) and
     *  ``invulnerable_s`` of protection. This is how the player respawns;
     *  NPCs that already finished dying have been removed and cannot be.
     *  Returns false when the entity has no health. Normally only
     *  ``builtin.game_flow`` calls this. */
    virtual bool revive(std::uint32_t entity, float health, float invulnerable_s) {
        (void)entity;
        (void)health;
        (void)invulnerable_s;
        return false;
    }

    /** Faction id, or "" when the entity has no faction. */
    virtual const char* faction(std::uint32_t entity) const = 0;

    /** True when these two are on opposing sides. Symmetric: it is enough
     *  for either faction to name the other as hostile. */
    virtual bool is_hostile(std::uint32_t a, std::uint32_t b) const = 0;
};

/**
 * What an NPC can see.
 *
 * Target selection and line of sight, so every enemy does not hand-roll its
 * own entity scan. All of these ignore dead entities, which is the bug that
 * otherwise shows up as a squad standing around aiming at a corpse.
 */
class IPerception {
public:
    virtual ~IPerception() = default;

    /** Closest living entity hostile to ``self`` within ``max_range``, or
     *  ``0xFFFFFFFF``. Does not test line of sight -- combine with
     *  ``can_see`` when the NPC should not react through a wall. */
    virtual std::uint32_t nearest_hostile(std::uint32_t self,
                                          float max_range) const = 0;

    /** Every living hostile within range, nearest first. Returns how many
     *  were written. */
    virtual int hostiles_in_range(std::uint32_t self, float max_range,
                                  std::uint32_t* out, int max_out) const = 0;

    /** Is ``target`` visible from ``self``: inside range, inside the
     *  horizontal field of view, and with nothing solid in between. */
    virtual bool can_see(std::uint32_t self, std::uint32_t target,
                         float fov_deg, float max_range) const = 0;

    /** Unobstructed line between two entities, ignoring facing. Cheaper than
     *  ``can_see`` when the caller already knows the target's direction --
     *  a turret that always faces its target, say. */
    virtual bool line_of_sight(std::uint32_t from,
                               std::uint32_t to) const = 0;
};

/**
 * Getting an NPC from where it is to where it wants to be.
 *
 * Deliberately a narrow interface over a layered implementation, because the
 * two halves of navigation have very different costs and generated code
 * should never have to know which one answered:
 *
 *   * Local steering -- obstacle whiskers, wall sliding, keeping clear of
 *     other agents, staying on the floor. Always on, cheap, and enough for
 *     open arenas.
 *   * Global routing over the walkable regions and portals the world builder
 *     declared as it built the level. This is what gets an NPC through a
 *     doorway instead of pressing it against the wall beside one.
 *
 * Because everything goes through these five calls, upgrading or replacing
 * the backend never touches a line of generated gameplay code.
 *
 * Cost note for Quest: ``next_waypoint`` is cheap enough to call every frame,
 * but ``is_reachable`` and ``path_distance`` run a graph search. Call those
 * when a decision changes, not every frame for every NPC.
 */
class INavigation {
public:
    virtual ~INavigation() = default;

    /** One step toward ``target``: a world point to steer at this frame,
     *  already avoiding obstacles and other agents.
     *
     *  Feed the result straight into ``CLocomotionIntent``:
     *
     *      const Vec3 wp = ctx.nav->next_waypoint(self, player_pos);
     *      Vec3 dir = wp - my_pos;  dir.y = 0;
     *      intent.desired_velocity = glm::normalize(dir) * speed;
     */
    virtual Vec3 next_waypoint(std::uint32_t agent, const Vec3& target) = 0;

    /** Can something walk from here to there at all? False for a target on a
     *  roof, across a chasm, or inside a sealed room. Without this an NPC
     *  will happily set off toward somewhere it can never arrive. */
    virtual bool is_reachable(const Vec3& from, const Vec3& to) const = 0;

    /** Walking distance, which is not the straight-line distance once walls
     *  are involved. Returns false when there is no route. Use it to choose
     *  between targets, or to decide a flank is too long to be worth it. */
    virtual bool path_distance(const Vec3& from, const Vec3& to,
                               float& out_meters) const = 0;

    /** Drop a point onto whatever surface is under it. */
    virtual Vec3 project_to_ground(const Vec3& position) const = 0;

    /** The closest point to ``position`` INSIDE the walkable space the world
     *  declared, kept ``margin_m`` from its edge; ``position`` itself when it
     *  is already inside or the world declared none. A spot "2 m behind the
     *  player" at the edge of a yard is past the fence. */
    virtual Vec3 nearest_walkable(const Vec3& position, float margin_m = 0.4f) const {
        (void)margin_m;
        return position;
    }

    /** Clear straight line between two points, ignoring who is at each end. */
    virtual bool line_of_sight(const Vec3& from, const Vec3& to) const = 0;
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
     *  Names come from the rig. Tripo character assets use ``spec="tripo"``
     *  (native bone names); the player-hand fixture uses a separate
     *  ``mixamorig:`` naming contract for ``builtin.hand_pose``. Resolve once
     *  in ``on_attach`` and cache the index -- this is a string lookup, not
     *  something to do per joint per frame. */
    virtual int joint_index(std::uint32_t entity, const char* name) const = 0;

    /** Number of joints, or 0. */
    virtual int joint_count(std::uint32_t entity) const = 0;

    /** Index of the joint occupying a canonical body slot, or -1.
     *
     *  This is the resolver every mechanic should use instead of guessing at
     *  joint names, because there is no single naming convention: this
     *  pipeline alone emits Tripo-spec names for characters and a
     *  ``mixamorig:`` contract for the engine's hand fixture, and imported
     *  assets add Mixamo proper, Unity Humanoid and Unreal on top. The slot
     *  vocabulary and its per-convention candidate lists live in
     *  ``ptv/api/BodySlots.h``.
     *
     *  Resolve once in ``on_attach`` and cache the index. */
    virtual int joint_for_slot(std::uint32_t entity,
                               const char* slot) const = 0;

    /** Parent of ``joint``, or -1 for a root joint (and for a bad index).
     *
     *  Needed to decide which end of a span is the parent-most: a gauntlet
     *  covering fingertip-to-elbow has to attach to the elbow, because that
     *  is the joint the rest of the span hangs off. */
    virtual int joint_parent(std::uint32_t entity, int joint) const = 0;

    /** World matrix of ``joint`` this frame, or identity when unavailable.
     *
     *  Reading a joint's pose is what makes a fitted attachment possible at
     *  all: the length from a fingertip to an elbow is not a constant, it is
     *  a property of the rig plus its current pose, so it has to be measured
     *  rather than assumed. Includes the entity's own transform, so the
     *  result is directly comparable with ``ISockets::socket_world``.
     *
     *  Computed from the CURRENT local pose, including every override
     *  written earlier this frame, so a solver that writes a parent and then
     *  reads the child sees the child where the new parent put it. */
    virtual Mat4 joint_world(std::uint32_t entity, int joint) const = 0;

    /** ``joint``'s pose in the entity's OWN space (metres, before the
     *  entity transform), from the current local pose. Rotation columns are
     *  orthonormal: any scale a rig carries on its armature is removed, so
     *  the rotation can be fed straight into quaternion maths. */
    virtual Mat4 joint_model(std::uint32_t entity, int joint) const = 0;

    /** Same as ``joint_model`` but for the rig's bind (rest) pose. Use this,
     *  not the current pose, to measure a rig: bone lengths, finger
     *  directions, which way the body faces. */
    virtual Mat4 joint_bind_model(std::uint32_t entity, int joint) const = 0;

    /** ``joint``'s LOCAL rotation in the bind pose. */
    virtual Quat joint_bind_rotation(std::uint32_t entity, int joint) const = 0;

    /** ``joint``'s LOCAL rotation as it stands right now. */
    virtual Quat joint_local_rotation(std::uint32_t entity,
                                      int joint) const = 0;

    /** Override ``joint``'s local rotation as ``bind * delta`` for this
     *  frame.
     *
     *  Prefer this to ``set_joint_rotation`` for anything that BENDS a rig
     *  (a finger curl, a head turn): real rigs carry a non-identity rest
     *  rotation on nearly every joint, and replacing it outright throws the
     *  rest pose away -- the finger ends up pointing wherever the identity
     *  quaternion happens to point. */
    virtual void set_joint_rotation_from_bind(std::uint32_t entity, int joint,
                                              const Quat& delta) = 0;

    /** Write every joint in ``root``'s subtree (including ``root``) into
     *  ``out``, returning how many were written.
     *
     *  Exists so a caller can act on a whole limb or a whole head in one go
     *  -- suppressing the avatar's head in first person has to take the jaw,
     *  eyes and hair with it, not just the head joint, or the player sees
     *  their own hair from the inside. */
    virtual int joint_subtree(std::uint32_t entity, int root, int* out,
                              int max_out) const = 0;

    /** Override one joint's local rotation for this frame. */
    virtual void set_joint_rotation(std::uint32_t entity, int joint,
                                    const Quat& rotation) = 0;
    /** Override one joint's local translation for this frame. Rarely what you
     *  want -- moving a joint off its rig position stretches the mesh. */
    virtual void set_joint_translation(std::uint32_t entity, int joint,
                                       const Vec3& translation) = 0;
    /** Override one joint's local scale for this frame.
     *
     *  Scaling a joint to 0 collapses it and everything below it to a point,
     *  which is how a subtree is HIDDEN without a second mesh or a material
     *  swap: the vertices weighted to it land on the joint's own origin. Unlike
     *  ``CSelfViewHide`` this applies in every view, including a mirror, so it
     *  is the right tool when the geometry is being REPLACED rather than
     *  merely kept out of the player's own camera -- an avatar whose rig ends
     *  at the wrist, whose hand is drawn by the engine's finger-rigged
     *  fixture instead. The joint's origin does not move, so an attachment or
     *  socket resolved against it still lands in the right place. */
    virtual void set_joint_scale(std::uint32_t entity, int joint,
                                 const Vec3& scale) = 0;

    /** Start a clip by semantic intent. Returns false when the asset has no
     *  clip for it. Restarts from zero if it is already the active clip.
     *
     *  ``fade_s`` cross-fades out of whatever was playing. The default is a
     *  short blend rather than 0 because a hard cut between, say, idle and
     *  walk is a visible snap on every gait change. Pass 0 when the cut is
     *  the point -- a hit reaction should be instant. */
    virtual bool play_clip(std::uint32_t entity, const char* clip_name,
                           bool looping, float fade_s = 0.15f) = 0;
    /** Stop clip playback and hold the current pose. */
    virtual void stop_clip(std::uint32_t entity) = 0;
    /** Playback rate multiplier, clamped by the engine to a sane range. */
    virtual void set_clip_speed(std::uint32_t entity, float speed) = 0;

    /** True when the asset has a clip for this intent. Cheaper than calling
     *  ``play_clip`` and checking the result, and it does not disturb
     *  playback -- use it to pick between an animated and an unanimated
     *  branch at attach time. */
    virtual bool has_clip(std::uint32_t entity, const char* clip_name)
        const = 0;

    /** The intent currently playing, or "" when nothing is. Compare against
     *  what you want before calling ``play_clip`` so a per-frame "keep
     *  walking" does not restart the walk cycle every frame. */
    virtual const char* current_clip(std::uint32_t entity) const = 0;

    /** Seconds into the active clip. */
    virtual float clip_time(std::uint32_t entity) const = 0;

    /** Length of the active clip in seconds, or 0 when none. */
    virtual float clip_duration(std::uint32_t entity) const = 0;

    /** True once a NON-looping clip has reached its end and stopped.
     *
     *  This is how a one-shot reads as finished: fire ``attack``, then wait
     *  for ``clip_finished`` before returning to the locomotion state. Always
     *  false while a looping clip plays, since it never ends. */
    virtual bool clip_finished(std::uint32_t entity) const = 0;

    /** Stop the active clip by cross-fading back to the rest (bind) pose over
     *  ``fade_s`` instead of freezing on the current frame. What a character
     *  with no ``idle`` clip does when it stands still. */
    virtual void fade_to_rest(std::uint32_t entity, float fade_s) {
        (void)fade_s;
        stop_clip(entity);
    }

    /** True while a NON-looping clip (attack, hurt, die, a wave) is still
     *  playing. Locomotion leaves the clip alone until it finishes, so a
     *  one-shot is not replaced by the walk cycle on the next frame. */
    virtual bool one_shot_playing(std::uint32_t entity) const {
        (void)entity;
        return false;
    }
};

/**
 * Declared world state, and the only correct way to change it.
 *
 * The tempting alternative -- every mechanic writing its own save calls --
 * is wrong. It would mean a dozen built-ins plus every generated mechanic
 * inventing a key format, and any mechanic that forgot would silently lose
 * state with nothing reporting a problem.
 *
 * So state is mutated THROUGH this service, and persistence falls out for
 * free: the registry holds both the pristine value the world document
 * declared and the current one, so what needs saving is simply the
 * difference. A door mechanic calls ``set_state(id, "angle", 45.0f)`` and
 * never touches a save file.
 *
 * ``interaction_allowed`` is the shared gate for per-scene interaction
 * policy, so an object a policy forbids both refuses the action and stops
 * advertising itself -- advertising an interaction that will be refused is
 * worse than showing no cue at all.
 */
class IInteractive {
public:
    virtual ~IInteractive() = default;

    virtual bool exists(const char* id) const = 0;

    /** Entity carrying ``id``, or ``0xFFFFFFFF``. */
    virtual std::uint32_t entity_of(const char* id) const = 0;

    /** Declared id of ``entity``, or "". */
    virtual const char* id_of(std::uint32_t entity) const = 0;

    /** Declared kind ("door", "switchable", ...), or "". */
    virtual const char* kind_of(const char* id) const = 0;

    /** Current value of one state key. A missing key returns a default
     *  (false) value rather than failing, so a mechanic reading a key its
     *  world did not declare degrades instead of crashing. */
    virtual InteractiveValue get_state(const char* id,
                                       const char* key) const = 0;

    /** Change one state key. This is the call that makes the change
     *  persistent; there is no separate save step. */
    virtual void set_state(const char* id, const char* key,
                           const InteractiveValue& value) = 0;

    /** True when ``key`` has diverged from the value the world document
     *  declared. Exposed because gameplay sometimes wants to know that
     *  something has been touched at all. */
    virtual bool is_changed(const char* id, const char* key) const = 0;

    virtual bool is_locked(const char* id) const = 0;
    virtual void set_locked(const char* id, bool locked) = 0;

    /** Whether interaction with this object is permitted right now. */
    virtual bool interaction_allowed(const char* id) const = 0;
    virtual void set_interaction_allowed(const char* id, bool allowed) = 0;

    /** Write the ids of every interactive of ``kind`` into ``out``,
     *  returning how many were written. A generated mechanic uses this to
     *  find "every light in the building" without hard-coding ids. */
    virtual int find_by_kind(const char* kind, const char** out,
                             int max_out) const = 0;
};

/**
 * Facts that are not the state of any object.
 *
 * ``mission_complete``, ``score``, ``chapter_2_complete``, which exhibit the
 * player entered through. These have no entity, so they need no naming
 * contract with world generation and no ordering dependency on it -- which
 * is exactly why they are the ONLY persistence calls generated code should
 * be writing by hand. Anything attached to an object goes through
 * ``IInteractive`` instead.
 *
 * Two scopes because both are needed: a per-scene store keeps a temple's
 * puzzle state from colliding with a laboratory's, while the global store is
 * what carries progress across a transition.
 */
class IPersistence {
public:
    virtual ~IPersistence() = default;

    virtual void set_flag(const char* key, bool value) = 0;
    virtual bool flag(const char* key, bool fallback = false) const = 0;
    virtual void set_number(const char* key, float value) = 0;
    virtual float number(const char* key, float fallback = 0.0f) const = 0;
    virtual void set_text(const char* key, const char* value) = 0;
    virtual const char* text(const char* key,
                             const char* fallback = "") const = 0;

    /** Same four, scoped to one scene id. */
    virtual void set_scene_flag(const char* scene, const char* key,
                                bool value) = 0;
    virtual bool scene_flag(const char* scene, const char* key,
                            bool fallback = false) const = 0;
    virtual void set_scene_number(const char* scene, const char* key,
                                  float value) = 0;
    virtual float scene_number(const char* scene, const char* key,
                               float fallback = 0.0f) const = 0;

    virtual bool has(const char* key) const = 0;
    virtual void clear(const char* key) = 0;
};

/** One row a caller wants shown. Mirrors ``UiRow`` without dragging the ECS
 *  header into the generated-code surface. */
struct UiRowDesc {
    const char* id{""};
    const char* label{""};
    std::uint32_t thumbnail{0xFFFFFFFFu};
    bool enabled{true};
    bool selected{false};
    bool marked{false};
};

/** A click, as read back by the caller. */
struct UiClick {
    const char* panel_id{""};
    const char* row_id{""};
    int row_index{-1};
    int hand{-1};
};

/**
 * Virtual panels.
 *
 * Exists because four of the equip scenarios want a menu -- see what is worn,
 * pick something, press Unequip -- and generated code had no way to build one.
 * The engine already proved the pattern with the locomotion menu, but that was
 * hard-coded in the application layer: fixed pages, two pads, its own edge
 * detection, its own debounce, its own comfort-anchored placement. Every
 * generated menu would have reimplemented all of it, differently.
 *
 * So the engine owns the mechanics of a panel -- where it sits, which row the
 * pointer is on, what counts as a click -- and the caller owns only what the
 * rows MEAN. A wardrobe becomes: list what ``IEquip`` says is worn, and on a
 * click, call ``unequip``.
 *
 * Rows are addressed by id rather than index, because an inventory that
 * reorders itself between frames would otherwise deliver a click for the wrong
 * item.
 */
class IUi {
public:
    virtual ~IUi() = default;

    /** Does a panel with this id exist in the scene? */
    virtual bool panel_exists(const char* panel_id) const = 0;

    virtual void open(const char* panel_id) = 0;
    virtual void close(const char* panel_id) = 0;
    virtual bool is_open(const char* panel_id) const = 0;

    /** Replace a panel's rows.
     *
     *  Whole-list replacement rather than incremental edits: the caller
     *  already knows the full truth every frame (what is equipped, what is in
     *  the bag), and a diffing API would only add a way for the panel to
     *  disagree with it. Preserves highlight by row id across the swap, so
     *  rebuilding every frame does not fight the pointer. */
    virtual void set_rows(const char* panel_id, const UiRowDesc* rows,
                          int count) = 0;

    /** Read the oldest unread click, or false when there is none.
     *
     *  A queue rather than a callback because a mechanic's tick is the only
     *  place it is safe to act, and clicks are detected during input
     *  processing. Poll in a loop: two rows can be clicked in one frame with
     *  two hands. */
    virtual bool poll_click(const char* panel_id, UiClick* out) = 0;

    /** Which row the pointer is on, or -1. Lets a caller preview a selection
     *  without waiting for the click. */
    virtual int highlighted_row(const char* panel_id) const = 0;

    /** Heading text drawn above the rows ("Game Over") and a smaller line
     *  under it ("Survived 2:41"). Row labels are drawn on the rows. */
    virtual void set_title(const char* panel_id, const char* title,
                           const char* subtitle) {
        (void)panel_id;
        (void)title;
        (void)subtitle;
    }
};

/**
 * Going somewhere else.
 *
 * A transition is not something a mechanic may simply DO. Swapping the world
 * means destroying entities, and a mechanic is being ticked from inside a loop
 * over those entities -- tearing the scene down there invalidates the iterator
 * the caller is holding. So this only ever REQUESTS, and the engine performs
 * the swap at a frame boundary where nothing is mid-iteration.
 *
 * Requesting is enough for every caller that needs it: a portal volume the
 * player walks into, a menu selecting a mission, a generated mechanic ending a
 * chapter. Where the player lands is named rather than positional, because the
 * right place to stand depends on where they came from -- returning to a hub
 * from a mission belongs at that mission's door, not at the hub's front gate.
 */
class ISceneTransition {
public:
    virtual ~ISceneTransition() = default;

    /** Ask to move to ``scene_id``, arriving at ``spawn_point_id``.
     *
     *  Returns false when the target is not in this project's scene index,
     *  which is a build-time mistake worth reporting rather than a runtime
     *  condition to handle: the compiler wrote that index from the same
     *  BuildSpec the portal came from. An empty ``spawn_point_id`` means the
     *  target scene's own default spawn.
     *
     *  Only the FIRST request in a frame is honoured; later ones are ignored
     *  rather than queued, so two portals touched in the same frame cannot
     *  chain the player through two loads. */
    virtual bool go_to(const char* scene_id, const char* spawn_point_id) = 0;

    /** The scene the player is in now. */
    virtual const char* current_scene() const = 0;

    /** Is ``scene_id`` reachable at all? Lets a menu grey out an entry
     *  instead of offering a transition that will be refused. */
    virtual bool scene_exists(const char* scene_id) const = 0;

    /** Is a transition already pending this frame? */
    virtual bool transition_pending() const = 0;

    /** Reload the scene the player is in, at a frame boundary, arriving at
     *  its default spawn.
     *
     *  ``fresh`` = a new attempt: what the player changed (doors, pickups,
     *  loadout) is NOT restored, every scene's saved deltas are discarded,
     *  and the encounter starts over -- this is Try Again / Play Again.
     *  ``fresh = false`` reloads with the scene's remembered state. */
    virtual bool restart_current(bool fresh) {
        (void)fresh;
        return false;
    }

    /** Fold another scene INSIDE this one, mounted on ``host``'s ``anchor``
     *  and shrunk so its longest horizontal extent fits ``fit_extent_m``.
     *
     *  Not a transition: the outer world stays loaded and keeps running,
     *  which is the entire point. A skull interior travels with the character
     *  whose head it is in while that character walks around living its own
     *  life, and the player inside is unaware of any of it because they and
     *  the interior hang off the same anchor.
     *
     *  The interior is authored at HUMAN scale and shrunk here, never
     *  authored small: a level laid out inside a 22 cm box produces geometry
     *  and nav regions too degenerate to walk on.
     *
     *  Safe to call from a mechanic tick -- unlike a transition, nothing
     *  existing is destroyed, so no iterator is invalidated. */
    virtual bool load_nested(const char* scene_id, std::uint32_t host,
                             const char* anchor, float fit_extent_m) = 0;

    virtual bool unload_nested(const char* scene_id) = 0;
    virtual bool nested_loaded(const char* scene_id) const = 0;
};

/**
 * Wearing things.
 *
 * An engine service rather than state private to one mechanic, because equip
 * is driven from too many places for a tag to be enough: a hand releasing an
 * object, a menu button, sitting down in a seat, touching a pickup, or a
 * thirty-second timer expiring. All of those need to ask what is currently
 * worn and change it, and a mechanic that kept the answer to itself would
 * force every other caller to reimplement the bookkeeping.
 *
 * It is also what makes persistence automatic. Loadout and outfit survive an
 * environment transition because this service records them, not because each
 * mechanic remembered to write a save file.
 *
 * Works on any entity with slots, not just the player, so a goblin wearing a
 * rifle is the same code path.
 */
class IEquip {
public:
    virtual ~IEquip() = default;

    /** Put ``item`` on ``wearer`` at ``slot``.
     *
     *  Fails when the slot does not exist, the item's tags are not accepted
     *  there, or the item is no longer re-equippable. Equipping into an
     *  occupied EXCLUSIVE slot unequips the incumbent first; what happens to
     *  that displaced item is the caller's policy, not this service's. */
    virtual bool equip(std::uint32_t item, std::uint32_t wearer,
                       const char* slot) = 0;

    /** Take ``item`` off. Fails when it is not equipped or not currently
     *  removable. Restores physics, so it drops where it was. */
    virtual bool unequip(std::uint32_t item) = 0;

    /** Slot ``item`` occupies, or "" when it is not equipped. */
    virtual const char* slot_of(std::uint32_t item) const = 0;

    /** What is worn in ``slot``, or ``0xFFFFFFFF``. */
    virtual std::uint32_t equipped_at(std::uint32_t wearer,
                                      const char* slot) const = 0;

    /** Write everything ``wearer`` has equipped into ``out``, returning how
     *  many were written. This is what a wardrobe menu iterates. */
    virtual int equipped_items(std::uint32_t wearer, std::uint32_t* out,
                               int max_out) const = 0;

    /** Does ``wearer`` offer this slot at all? */
    virtual bool slot_exists(std::uint32_t wearer,
                             const char* slot) const = 0;

    /** Allow or forbid intentional removal of an already-equipped item.
     *
     *  Deliberately runtime-mutable and deliberately NOT permanent: a helmet
     *  is fixed in flight and free on the ground, goggles are fixed mid-
     *  experiment, and cursed armour becomes removable once the quest that
     *  breaks the curse completes -- all on the same item instance. */
    virtual void set_removable(std::uint32_t item, bool removable) = 0;

    /** Allow or forbid this instance being equipped again in future. A
     *  sterile glove that has been taken off is contaminated. */
    virtual void set_equippable(std::uint32_t item, bool equippable) = 0;

    virtual bool is_removable(std::uint32_t item) const = 0;

    /** Nudge an equipped item's pose without unequipping it, e.g. swinging
     *  night-vision goggles between raised and lowered. Prevents generated
     *  code from writing CTransform on something an attachment owns. */
    virtual void set_offset(std::uint32_t item, const Vec3& position,
                            const Quat& rotation) = 0;
};

/** Which kind of artificial motion a comfort setting applies to. */
enum class ComfortChannel : int {
    /** Sliding locomotion (head- or controller-relative). */
    Movement = 0,
    /** Smooth turning. */
    Turning = 1,
};

/**
 * Comfort vignette ("tunnel vision"), implemented the way Meta's locomotion
 * guidance describes it.
 *
 * A vignette darkens the edges of the view while artificial motion happens,
 * because peripheral optic flow is what drives vection. It is:
 *
 *   * dynamic -- it appears only while moving or turning and fades out
 *     shortly after the motion stops;
 *   * graded -- its strength follows speed, angular speed and acceleration;
 *   * defined in DEGREES of visual angle with a feathered edge, so it
 *     occludes the same amount of view on any headset.
 *
 * It is NOT a blink or a flash. Snap turning and teleporting are already the
 * comfort options and never use it; darkening the screen on every snap is
 * the flicker players complain about.
 *
 * Off by default. A slide or smooth-turn mechanic offers it on attach, with
 * the default state the prompt asked for, and a settings menu may let the
 * player toggle it.
 */
class IComfort {
public:
    virtual ~IComfort() = default;

    /** A motion mechanic says it supports a vignette for ``channel``.
     *  ``enabled`` is its starting state; ``user_toggle`` lets a settings
     *  menu show a switch for it. */
    virtual void offer_vignette(ComfortChannel channel, bool enabled,
                                bool user_toggle) = 0;
    virtual bool vignette_offered(ComfortChannel channel) const = 0;
    virtual bool vignette_toggleable(ComfortChannel channel) const = 0;
    virtual bool vignette_enabled(ComfortChannel channel) const = 0;
    virtual void set_vignette_enabled(ComfortChannel channel, bool on) = 0;

    /** Ask for a vignette THIS frame. ``strength`` 0..1 scales how far the
     *  edge closes in; ``inner_fov_deg`` is the clear field at full strength
     *  and ``feather_deg`` the width of the soft edge. The strongest request
     *  in a frame wins, and the engine smooths it in and out. Ignored when
     *  the channel's vignette is disabled. */
    virtual void request_vignette(ComfortChannel channel, float strength,
                                  float inner_fov_deg, float feather_deg) = 0;
};

/**
 * Where the world says gameplay things go.
 *
 * The world architect declares an ANCHOR for every placed gameplay item and
 * an anchor SET for things the game spawns at runtime -- the hang points
 * under a lane's poles, pickup pads, spawn spots. Spawning code reads them
 * here instead of hard-coding coordinates (which put a timed challenge's
 * targets on the floor outside its lane). Sets are looked up by id or by a
 * tag the set carries.
 */
class IAnchors {
public:
    virtual ~IAnchors() = default;

    /** Points in anchor set ``set_id`` (id or tag), 0 when it does not exist. */
    virtual int count(const char* set_id) const = 0;

    /** Point ``index`` of the set and the direction items there face. */
    virtual bool point(const char* set_id, int index, Vec3* position,
                       Vec3* facing) const = 0;

    /** The set's relation: "on_floor", "on_surface", "hang_from",
     *  "mount_on_wall" or "at_point"; "" when the set does not exist. */
    virtual const char* relation(const char* set_id) const = 0;

    /** A single named anchor. */
    virtual bool anchor(const char* id, Vec3* position, Vec3* facing) const = 0;

    /** The pose that puts ``entity`` at point ``index`` of the set by the
     *  set's relation, from the entity's measured collider: its BASE on the
     *  point for on_floor / on_surface, its TOP at it for hang_from, its
     *  origin there for at_point -- facing the set's facing. */
    virtual bool pose_at(std::uint32_t entity, const char* set_id, int index,
                         Vec3* position, Quat* rotation) const = 0;
};

/**
 * Named events, so built-ins and generated code can be wired together
 * without glue code: a button raises "lane2.start", a challenge waits for it.
 */
class ISignals {
public:
    virtual ~ISignals() = default;

    /** Raise ``name``. */
    virtual void emit(const char* name) = 0;

    /** How many times ``name`` has been raised since the scene loaded. */
    virtual std::uint32_t count(const char* name) const = 0;

    /** True when ``name`` was raised since ``seen`` was last updated (and
     *  updates it). Start ``seen`` at ``count(name)`` in on_attach. */
    bool fired_since(const char* name, std::uint32_t& seen) const {
        const std::uint32_t c = count(name);
        const bool fired = c != seen;
        seen = c;
        return fired;
    }
};

/**
 * Live, named values for the scene: a challenge's time left and score, a
 * wave counter. Not saved (persistent facts go through IPersistence). A
 * world display shows them with templates like "{lane2.time_left:0.0}s".
 */
class IVars {
public:
    virtual ~IVars() = default;
    virtual void set_number(const char* name, float value) = 0;
    virtual void set_text(const char* name, const char* value) = 0;
    /** The value, or ``fallback`` when unset or not a number. */
    virtual float number(const char* name, float fallback = 0.0f) const = 0;
    /** The value as text ("" when unset; numbers formatted with up to 2
     *  decimals). Valid until the next call. */
    virtual const char* text(const char* name) const = 0;
    virtual bool has(const char* name) const = 0;
    /** Bumps every time the value changes. */
    virtual std::uint32_t seq(const char* name) const = 0;
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
    IContacts* contacts{nullptr};
    ISpawn* spawn{nullptr};
    IAttach* attach{nullptr};
    ICombat* combat{nullptr};
    IPerception* perception{nullptr};
    INavigation* nav{nullptr};
    IDebugDraw* draw{nullptr};
    IHands* hands{nullptr};
    ITransformFx* fx{nullptr};
    IHaptics* haptics{nullptr};
    ISockets* sockets{nullptr};
    ISkeleton* skeleton{nullptr};
    IEquip* equip{nullptr};
    IInteractive* interactive{nullptr};
    IPersistence* persistence{nullptr};
    ISceneTransition* transition{nullptr};
    IUi* ui{nullptr};
    IComfort* comfort{nullptr};
    /** The scene's NPC waves (see IEncounters). Always set by the engine;
     *  ``configured()`` is false when the scene has no encounter plan. */
    IEncounters* encounters{nullptr};
    /** The game loop (see ptv/api/GameFlow.h). ``configured()`` is false in a
     *  scene with no ``builtin.game_flow``; ``gameplay_active()`` is then
     *  always true. */
    IGameFlow* game{nullptr};
    /** Quit the app (Leave Game), platform, uptime. */
    IApp* app{nullptr};
    /** World-space text (see ITextDraw). */
    ITextDraw* text{nullptr};
    /** Gameplay effects: flashes, tracers, beams, impacts, decals
     *  (ptv/api/Effects.h). */
    IEffects* vfx{nullptr};
    /** Engine-owned projectiles with travel time (ptv/api/Projectiles.h). */
    IProjectiles* projectiles{nullptr};
    /** Weapon state, hits and NPC triggers (ptv/api/Weapons.h). */
    IWeapons* weapons{nullptr};
    /** World-declared anchors and spawn-point sets (see IAnchors). */
    IAnchors* anchors{nullptr};
    /** Named events (see ISignals). */
    ISignals* signals{nullptr};
    /** Live named values (see IVars). */
    IVars* vars{nullptr};
};

}  // namespace ptv
