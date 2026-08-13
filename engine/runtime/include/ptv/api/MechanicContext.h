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
class ICombat {
public:
    virtual ~ICombat() = default;

    /** Deal damage. Returns true if it landed -- false when the target has
     *  no health, is already dead, or is still inside the invulnerability
     *  window from its last hit.
     *
     *  That window matters more than it looks: contact damage is tested
     *  every frame, so without it a single touch from an enemy empties a
     *  health bar in a few frames. */
    virtual bool apply_damage(std::uint32_t target, float amount,
                              std::uint32_t instigator) = 0;

    /** Restore health, never above the maximum. */
    virtual void heal(std::uint32_t entity, float amount) = 0;

    virtual float health(std::uint32_t entity) const = 0;
    virtual float max_health(std::uint32_t entity) const = 0;

    /** False for a dead entity, one that is playing its death animation, or
     *  one that never had health at all. */
    virtual bool is_alive(std::uint32_t entity) const = 0;

    /** Kill outright, skipping damage and invulnerability. */
    virtual void kill(std::uint32_t entity) = 0;

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

    /** Override one joint's local rotation for this frame. */
    virtual void set_joint_rotation(std::uint32_t entity, int joint,
                                    const Quat& rotation) = 0;
    /** Override one joint's local translation for this frame. Rarely what you
     *  want -- moving a joint off its rig position stretches the mesh. */
    virtual void set_joint_translation(std::uint32_t entity, int joint,
                                       const Vec3& translation) = 0;

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
};

}  // namespace ptv
