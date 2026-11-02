#pragma once

#include <array>
#include <entt/entt.hpp>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ptv/core/MemoryGuard.h"
#include "ptv/core/QualityGovernor.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"
#include "ptv/mechanics/MechanicRegistry.h"
#include "ptv/nav/NavGraph.h"
#include "ptv/physics/PhysicsWorld.h"
#if !defined(__ANDROID__)
// GLFW mirror window is desktop-only. On Quest (NativeActivity) there is no
// desktop surface, and Window's implementation (platform/desktop/Window.cpp)
// is excluded from the Android build -- so even holding a unique_ptr<Window>
// member would odr-use the missing Window::~Window() at link time.
#include "ptv/platform/Window.h"
#endif
#include "ptv/render/Renderer.h"
#include "ptv/render/VulkanContext.h"
#include "ptv/scene/Scene.h"
#include "ptv/xr/XrSession.h"

namespace ptv {

class EffectsSystem;
class ProjectileSystem;
class WeaponBoard;
class WeaponVerifier;
class TextureGallery;

/** Top-level application class.
 *
 * Owns the Vulkan context, the OpenXR session, the ECS world, the physics
 * world, and the mechanic host. The ``run()`` method is the main loop.
 */
class Application {
public:
    struct Config {
        std::string app_name{"ptv_runtime"};
        // Primary scene path (from explicit argv[1] or a launcher). Takes
        // priority over PTV_SCENE_JSON and the fallback below.
        std::string scene_json_path;
        // Sidecar/demo scene (e.g. the starter-room's baked-in scene.json
        // next to the exe). Only used when neither argv[1] nor the
        // PTV_SCENE_JSON env var is provided -- so launching a generated
        // project no longer accidentally loads the sample room.
        std::string scene_json_fallback_path;
        int mirror_width{1280};
        int mirror_height{720};
        bool enable_validation{true};
    };

    Application();
    // Defined out-of-line: the mechanic-service members below are held by
    // unique_ptr to incomplete types.
    ~Application();

    int run(const Config& cfg);

private:
    // Engine-side implementations of the services handed to mechanics
    // (ptv/api/MechanicContext.h). Defined in Application.cpp; nested so they
    // can reach the rig / physics / draw state without exposing any of it.
    class LocomotionService;
    class PhysicsAccessService;
    class ContactsService;
    class SpawnService;
    class AttachService;
    class CombatService;
    class PerceptionService;
    class NavigationService;
    class DebugDrawService;
    class HandsService;
    class TransformFxService;
    class HapticsService;
    class SocketsService;
    class SkeletonService;
    class EquipService;
    class InteractiveService;
    class PersistenceService;
    class TransitionService;
    class UiService;
    class ComfortService;
    class AppService;
    class TextDrawService;
    friend class AppService;
    friend class TextDrawService;
    friend class ComfortService;
    friend class LocomotionService;
    friend class PhysicsAccessService;
    friend class ContactsService;
    friend class SpawnService;
    friend class AttachService;
    friend class CombatService;
    friend class PerceptionService;
    friend class NavigationService;
    friend class DebugDrawService;
    friend class HandsService;
    friend class TransformFxService;
    friend class HapticsService;
    friend class SocketsService;
    friend class SkeletonService;
    friend class EquipService;
    friend class InteractiveService;
    friend class PersistenceService;
    friend class TransitionService;
    friend class UiService;

    bool initialize(const Config& cfg);
    void shutdown();
    void tick(float dt);
    /** Give every entity whose GLB carries a skin a ``CSkeleton`` +
     *  ``CAnimator``, and upload its geometry through the skinned path.
     *
     *  Runs once at scene load, after mesh paths are resolved and the
     *  renderer exists. Rigged meshes are deliberately loaded here rather
     *  than streamed: the streaming worker only knows how to build 40-byte
     *  static vertices. */
    void attach_skeletons();
    /** Create the player's two rigged hand entities from the generated hand
     *  meshes under ``PTV_ENGINE_ASSET_DIR``.
     *
     *  No-op when the assets are missing, in which case ``hand_entities_``
     *  stays null and the render loop falls back to ``push_hand_mesh``. */
    void create_hand_entities();
    /** 1 Hz ``hands:`` line: tracked / loaded / drawn / finite / size. */
    void log_hands_telemetry(const bool tracked[2]);
    // Build the renderer's environment (sky/sun/ambient/fog/point lights) from
    // the loaded ``scene_``. Cached so it isn't rebuilt every frame; defaults
    // are chosen so scenes without an authored sky/lights (e.g. the starter
    // room) still look good.
    void build_render_env();

    /** Log when two bound mechanics fill the same player role (two
     *  teleporters, two snap turns). The engine injects no player mechanics
     *  of its own; a scene that binds none is a stationary game. */
    void check_player_mechanic_roles() const;

    /** Copy locomotion tunables and default modes from the scene's player
     *  block, and reset what the scene's mechanics have declared. */
    void apply_player_settings_from_scene();

    /** Make sure the active locomotion / turn mode is one some bound
     *  mechanic actually implements. */
    void reconcile_available_modes();

    // ------------------------------------------------------------------
    // Player motor
    // ------------------------------------------------------------------

    /** True when the CharacterVirtual body owns the player's movement. */
    bool motor_active() const;

    /** Resolve this frame's requested movement plus the player's physical
     *  walking against the world, and keep the feet on the ground. */
    void step_player_motor(float dt);

    /** Movement for a player the motor does not own (riding a frame, or
     *  scaled): swept, never floated, applied frame-relative. */
    void apply_pending_move_legacy();

    /** Everything that moves WITH the player and must never collide with
     *  the player's own body: the contact capsule, the hand anchors, the
     *  avatar, held items, worn items and anything attached (at any depth)
     *  to one of those. */
    void collect_player_body_family(std::vector<std::uint32_t>& out) const;

    /** The headset's world position against the rig as it is RIGHT NOW. */
    Vec3 current_head_world() const;

    /** Put a hand entity on its controller through the hand mesh's
     *  measured grip alignment. */
    void place_hand_entity(int hand, const Vec3& grip_pos,
                           const Quat& grip_rot);

    /** ``player.hands.grip_trim_deg`` as a rotation in the grip frame. */
    Quat grip_trim_quat() const;

    /** ``player.hands.debug_axes``: draw each grip frame for calibration. */
    void draw_hand_debug_axes(const Vec3 grip_pos[2], const Quat grip_rot[2],
                              const bool valid[2]);

    /** Create the ECS entity that gives the player a body in the world.
     *
     *  The player used to be nothing but ``rig_``, a bare transform, which
     *  meant they walked through walls and, worse, nothing could collide
     *  WITH them -- an enemy that runs at the user simply passed through.
     *  Making the player an ordinary kinematic capsule entity means contacts,
     *  damage and factions all treat them like any other actor. */
    void create_player_body();

    /** Keep the player capsule under the headset and at rig height. */
    void update_player_body(const Vec3& head_world_position, float dt);

    /** Duplicate a scene-authored template entity at a new pose.
     *
     *  Backs ``ISpawn::instance``. Copies the components that describe what
     *  the thing IS (mesh, collider, rig, tags, bindings) and then registers
     *  the clone with physics and the mechanic host. */
    entt::entity clone_entity(std::uint32_t template_entity,
                              const Vec3& position, const Quat& rotation);

    /** Give an entity that appeared after scene load its physics body and
     *  its mechanics. Both used to happen once, at startup only. */
    void attach_entity_runtime(std::uint32_t entity);

    /** Tear down and destroy a runtime entity. */
    void destroy_entity_runtime(std::uint32_t entity);

    /** Where an attachment anchor resolves to on a parent entity. */
    struct AttachAnchor {
        bool valid{false};
        /** True when the anchor is a named socket rather than a raw joint. */
        bool is_socket{false};
        int joint{-1};
    };

    /** Resolve an anchor name to a socket or a rig joint on ``parent``. */
    AttachAnchor resolve_attach_anchor(std::uint32_t parent,
                                       const std::string& name) const;

    /** Drive every attached entity from its parent's anchor.
     *
     *  Runs AFTER the pose system so a child follows the parent's animated
     *  joint, not its bind pose. */
    void tick_attachments(float dt);

    /** Turn the scene's declared nav regions/links into a routable graph. */
    void build_nav_graph();

    // ------------------------------------------------------------------
    // Scene transitions
    // ------------------------------------------------------------------

    /** Note that a transition is wanted. Does NOT perform it.
     *
     *  Callers reach this from inside a mechanic tick, which is inside a loop
     *  over the entities a transition would destroy. Doing the swap there
     *  invalidates the iterator the caller is holding, so the request is
     *  recorded and honoured at a frame boundary instead. */
    bool request_scene_transition(const std::string& scene_id,
                                  const std::string& spawn_point_id);

    /** Ask to reload the current scene at the next frame boundary (Try Again
     *  / Play Again). ``fresh`` drops every scene's saved deltas. */
    bool request_scene_restart(bool fresh);

    /** Leave Game: ask the XR runtime to end the session; the frame loop
     *  stops when it has. */
    void request_exit();

    /** Perform a requested transition, if there is one. Called once per frame
     *  from outside every entity loop. */
    void apply_pending_transition();

    /** Watch for the player standing in a portal volume, and act on it.
     *
     *  A portal that ``requires_interaction`` waits for a deliberate squeeze
     *  while inside; one that does not fires on entry. Defaulting to requiring
     *  the squeeze is the conservative choice: walking through a doorway you
     *  did not mean to take, and losing the room you were in, is worse than
     *  having to ask. */
    void tick_portals(const InputSnapshot& input);

    /** Swap the world over to ``path``, arriving at ``spawn_point_id``.
     *
     *  Saves the outgoing scene's deltas first and overlays the incoming
     *  scene's on the way in, so a place remembers what the player did to it.
     *  Everything derived from the scene -- physics bodies, the nav graph, the
     *  player body, mechanic instances, the render environment -- is rebuilt,
     *  because all of it describes the scene being replaced. */
    bool load_scene_at(const std::string& path,
                       const std::string& spawn_point_id,
                       bool fresh = false);

    /** Put the rig on a named spawn point, or on the scene's default. */
    void place_player_at_spawn(const std::string& spawn_point_id);

    /** Fold another scene into this one, mounted on ``host``'s ``anchor`` and
     *  shrunk so its longest horizontal extent fits ``fit_extent_m``.
     *
     *  Unlike a transition, the outer world stays loaded and keeps running --
     *  that is the whole point. A skull interior travels with the character
     *  whose head it is in while that character walks around living its own
     *  life, and the player inside it is unaware of any of that because both
     *  they and the interior hang off the same anchor.
     *
     *  Authored at HUMAN scale and shrunk here, never authored small: a level
     *  laid out inside a 22 cm box produces geometry and nav regions too
     *  degenerate to walk on. Returns false if the scene is unknown or already
     *  loaded. */
    bool load_nested_scene(const std::string& scene_id, std::uint32_t host,
                           const std::string& anchor, float fit_extent_m);

    /** Remove a nested scene and everything in it. */
    bool unload_nested_scene(const std::string& scene_id);

    bool nested_scene_loaded(const std::string& scene_id) const;

    /** The mount matrix for a nested scene: anchor world pose times fit
     *  scale. Recomputed rather than stored, so it follows the animated
     *  joint. */
    Mat4 nested_mount_matrix(std::uint32_t host, const std::string& anchor,
                             float scale) const;

    /** Where this scene's save deltas live. Per scene, so a temple's puzzle
     *  state cannot collide with a laboratory's. */
    std::string save_path_for_scene(const std::string& scene_id) const;

    // ------------------------------------------------------------------
    // Player reference frame
    // ------------------------------------------------------------------

    /** Stage-to-world for the player, including any parent frame and scale.
     *
     *  The single place the rig becomes a world transform. Every consumer --
     *  head pose, hand poses, eye views, locomotion sweeps -- goes through
     *  this, because a second composition would drift from the first and the
     *  player's hands would stop matching their eyes. */
    Mat4 rig_world_matrix() const;

    /** World transform of the rig's parent frame, or identity.
     *
     *  Yaw-flattened: a boat that pitches on a wave must not pitch the
     *  player's horizon with it. Tilting the player's frame is the single
     *  most reliable way to make someone sick in VR, so the frame carries
     *  position and heading only -- the parent's roll and pitch are visible
     *  in the world around the player, which is where they belong. */
    Mat4 rig_parent_matrix() const;

    /** True while the rig is parented to something. */
    bool has_reference_frame() const;

    /** Attach the rig to ``entity``'s ``anchor``, preserving where the player
     *  currently is: the local offset is recomputed so they do not teleport
     *  at the moment of boarding. */
    void set_reference_frame(std::uint32_t entity, const std::string& anchor);

    /** Detach, leaving the player exactly where they are in world space. */
    void clear_reference_frame();

    /** Resize the player. Clamped to a sane range; see ``kMinPlayerScale``. */
    void set_player_scale(float scale);

    /** The player's stage origin in WORLD space.
     *
     *  ``rig_.pos_*`` is expressed in the reference frame, so it is only the
     *  same thing while the player is unparented. Everything outside the rig
     *  -- physics sweeps, teleport targets, gameplay asking "where is the
     *  player" -- speaks world space, so the conversion happens here rather
     *  than at each of those call sites. */
    Vec3 rig_world_position() const;
    float rig_world_yaw() const;

    /** Move the player to a WORLD position / heading, storing the equivalent
     *  frame-local values. */
    void set_rig_world_position(const Vec3& world);
    void set_rig_world_yaw(float yaw_rad);

    /** Rotate a WORLD direction into the reference frame.
     *
     *  A slide direction derived from where the player is looking is a world
     *  vector; adding it straight to a frame-local position would drift the
     *  moment the frame was rotated -- walking forward on a turning vehicle
     *  would curve. */
    Vec3 world_vector_to_frame(const Vec3& world_dir) const;

    /** Start an entity dying: play its death clip, stop it colliding, and
     *  schedule its removal for when the animation ends. */
    void begin_death(std::uint32_t entity);

    /** Age invulnerability windows and death timers, and remove entities
     *  whose death animation has finished. */
    void tick_combat(float dt);

    // ------------------------------------------------------------------
    // Player rig: the virtual transform of the stage origin in world
    // space. Thumbstick-driven locomotion moves/rotates the rig; the
    // physical head/hand poses from OpenXR are then composed through it.
    // ------------------------------------------------------------------
    struct PlayerRig {
        // Position and yaw are expressed in the REFERENCE FRAME below, which
        // is world space unless a parent has been set.
        float pos_x{0.0f};
        float pos_y{0.0f};
        float pos_z{0.0f};
        float yaw_rad{0.0f};  // rotation around the frame's Y
        // NOTE: the snap-turn cooldown used to live here. It now belongs to
        // the builtin.locomotion.snap_turn mechanic, which owns that state.

        // ----------------------------------------------------------------
        // Reference frame: what the rig's coordinates are relative to.
        //
        // World space is the special case where there is no parent. Once
        // there is one, everything the player does is expressed inside it,
        // and the parent's own motion carries them along for free -- WITHOUT
        // the player's local position drifting, which is the property that
        // matters. Standing still on a moving platform means the stick
        // input is zero and you still arrive somewhere else.
        //
        // Deliberately generic rather than built for one feature. A rig
        // parented to a moving anchor is how vehicles, elevators, boats,
        // lifts and moving platforms all work, and it is also how a
        // miniature player rides inside an NPC's head while that NPC walks
        // around living its own life. Solving it once as a reference frame
        // is why none of those need engine changes of their own.
        // ----------------------------------------------------------------
        entt::entity parent{entt::null};
        /** Joint or socket on the parent to follow, or empty for the
         *  parent's own transform. */
        std::string parent_anchor;

        /** How big the player is relative to the world.
         *
         *  1.0 is life size. 0.02 is a two-centimetre passenger inside a
         *  skull. Scaling the RIG rather than the world is the only version
         *  that works: the world is what physics, navigation and every
         *  authored dimension agree on, and rescaling it would invalidate
         *  all of them at once. */
        float scale{1.0f};
    };

    // Per-hand grab state tracked across frames so we can detect edge
    // transitions (press-to-grab / release-to-throw) and compute a
    // velocity estimate for the throw impulse.
    // Per-hand state owned by the INPUT layer.
    //
    // ``held`` is published here by the builtin.grab mechanic (through
    // ILocomotion's sibling IHands service) so the frame's InputSnapshot can
    // report it; the hold offsets and grip edge detection belong to that
    // mechanic. The velocity is a derived input reading, smoothed here and
    // handed to mechanics on the snapshot.
    struct HandGrab {
        entt::entity held{entt::null};
        Vec3 prev_world_pos{0.0f};   // previous frame hand world position
        Vec3 world_velocity{0.0f};   // smoothed m/s
        bool had_prev_pos{false};
    };

    // ------------------------------------------------------------------
    // Locomotion: the Starter Room prompts the user once on load for
    // their preferred movement style. The same enum / router is reused
    // in every generated game so new prompts don't need to rewire input.
    //
    // Terminology follows Meta's Horizon "Locomotion Types" guide:
    //   - "Teleport" is a discrete locomotion (aim + commit).
    //   - "Sliding" is Meta's name for thumbstick continuous motion.
    //   - "Head-Relative" sliding moves along the user's gaze.
    //   - "Controller-Relative" sliding moves along the left controller's
    //     pointing direction, independent of head yaw.
    // Refs:
    //   https://developers.meta.com/horizon/design/locomotion-types/
    //   https://developers.meta.com/horizon/design/locomotion-types/#artificial-locomotion-types
    //   https://developers.meta.com/horizon/design/locomotion-input-maps/
    // ------------------------------------------------------------------
    enum class LocomotionMode : int {
        Teleport = 0,                // left-stick push: aim arc; release: teleport
        SlideHeadRelative = 1,       // left-stick: slide in the direction of head gaze
        SlideControllerRelative = 2, // left-stick: slide along the left controller's aim
    };
    // NOTE: teleport aim/commit state used to live here. It now belongs to
    // the builtin.locomotion.teleport mechanic, which owns its own arc.

#if !defined(__ANDROID__)
    std::unique_ptr<Window> window_;  // desktop mirror window only
#endif
    VulkanContext vk_{};
    XrRuntime xr_{};
    Renderer renderer_{};
    bool renderer_ready_{false};
    World world_{};
    PhysicsWorld physics_{};
    /** Walkable topology declared by the world builder. Empty when the level
     *  did not declare any, in which case navigation is steering-only. */
    nav::NavGraph nav_graph_{};
    MechanicHost mechanics_{};
    SceneDoc scene_{};
    std::string scene_path_{};
    /** A transition asked for but not yet performed. Held rather than acted on
     *  because the request arrives from inside a loop over the very entities a
     *  transition destroys. */
    struct PendingTransition {
        bool active{false};
        std::string scene_id;
        std::string spawn_point_id;
        /** Reload the CURRENT scene (Try Again / Play Again). */
        bool restart{false};
        /** ...without restoring what the player changed, discarding every
         *  scene's saved deltas. */
        bool fresh{false};
    };
    PendingTransition pending_transition_{};
    /** The game loop's shared state (ctx.game), written by builtin.game_flow.
     *  ``attempts`` survives a restart; everything else starts over. */
    GameFlowBoard game_flow_board_{};
    // Leave Game: the OpenXR session is asked to exit and the frame loop
    // stops when it does (or after a timeout, if the runtime never answers).
    bool exit_requested_{false};
    bool exit_sent_{false};
    float exit_wait_s_{0.0f};
    double uptime_s_{0.0};
    // IApp::request_fade: strongest request this frame, and what is drawn.
    float fade_request_{0.0f};
    float fade_level_{0.0f};
    // Verification of the game flow (``debug.ptv.verify_flow`` = 1): the
    // player is NOT protected, and builtin.game_flow scripts its own
    // end-screen clicks so one unattended run walks death, Game Over, Try
    // Again, victory, Play Again and Leave Game.
    bool verify_flow_{false};
    /** Scenes folded into this one, by scene id.
     *
     *  Tracked so a mechanic can ask whether an interior is already there
     *  (attaching twice would stack two skulls' worth of geometry) and so
     *  unloading can find every entity that belongs to it. */
    struct NestedScene {
        std::string scene_id;
        std::uint32_t host{0xFFFFFFFFu};
        std::string anchor;
        float scale{1.0f};
        std::string tag;
    };
    std::vector<NestedScene> nested_scenes_{};
    /** Portals the player is currently standing in, by id. Tracked so an
     *  interaction-gated portal knows the squeeze happened INSIDE it, and so
     *  an entry-triggered one fires on the frame the player arrives rather
     *  than every frame they remain. */
    std::vector<std::string> portals_occupied_{};
    Renderer::RenderEnv render_env_{};
    bool running_{false};
    PlayerRig rig_{};
    // The player's collision capsule, or entt::null before it is created.
    // Tagged "player" so gameplay code can find the user the same way it
    // finds anything else.
    entt::entity player_entity_{entt::null};
    // Where that capsule is this frame (centre, world space). Cached because
    // locomotion resolves its sweep against it during the Input phase, before
    // the body itself has been moved.
    Vec3 player_capsule_center_{0.0f};
    float player_capsule_height_{1.7f};
    std::array<HandGrab, 2> hands_{};
    // The player's rigged hand entities, or entt::null when the generated
    // hand mesh was not found and the procedural block hands are drawn
    // instead. Exposed to mechanics through IHands::hand_entity so a hand-pose
    // mechanic can drive their joints.
    std::array<entt::entity, 2> hand_entities_{entt::null, entt::null};
    // Previous frame's raw OpenXR input, kept so the per-frame InputSnapshot
    // handed to mechanics can expose edge transitions (trigger/grip/button
    // rising + falling edges).
    XrRuntime::InputState prev_input_{};
    LocomotionMode locomotion_{LocomotionMode::Teleport};
    ::ptv::TurnMode turn_mode_{::ptv::TurnMode::Snap};
    // What the bound mechanics implement, declared as they attach. Indexed
    // by the API enums.
    std::array<bool, 3> mode_available_{false, false, false};
    std::array<bool, 2> turn_available_{false, false};
    // Menus currently holding the sticks.
    std::vector<std::string> input_lock_owners_;

    // --- Player motor ---
    ::ptv::VerticalMode vertical_mode_{::ptv::VerticalMode::Grounded};
    // Movement requested by locomotion mechanics this frame (world metres).
    Vec3 pending_move_{0.0f};
    float pending_jump_mps_{0.0f};
    // Stand the body under the head on the next frame that has a pose.
    bool motor_needs_sync_{true};
    // Where the head was last allowed to be, for the head sweep.
    Vec3 motor_head_resolved_{0.0f};
    float motor_resize_timer_s_{0.0f};
    // No collider under the spawn: treat the flat floor plane as ground.
    bool motor_virtual_floor_{false};
    Vec3 motor_last_grounded_{0.0f};
    bool motor_has_grounded_{false};
    // The player's body family, rebuilt each frame (reused allocation).
    std::vector<std::uint32_t> motor_ignored_;
    std::vector<std::uint32_t> motor_contacts_;
    bool motor_push_warned_{false};
    // Latest headset pose in STAGE space.
    Mat4 head_local_{1.0f};
    bool head_local_valid_{false};

    // Which skeleton builtin.hand_pose curls on each hand (IHands).
    std::array<entt::entity, 2> finger_targets_{entt::null, entt::null};
    // Visual hand anchors (IHands::set_hand_anchor), valid for the frame
    // they were set in.
    struct HandAnchor {
        std::uint64_t frame{0};
        Vec3 position{0.0f};
        Quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        float weight{0.0f};
    };
    std::array<HandAnchor, 2> hand_anchors_{};
    std::uint64_t frame_index_{1};
    std::array<Vec3, 2> forearm_dir_{Vec3(0.0f, 0.0f, 1.0f), Vec3(0.0f, 0.0f, 1.0f)};
    std::array<std::uint64_t, 2> forearm_frame_{0, 0};
    std::array<std::uint32_t, 2> support_entity_{0xFFFFFFFFu, 0xFFFFFFFFu};
    std::array<std::uint32_t, 2> hold_request_{0xFFFFFFFFu, 0xFFFFFFFFu};
    // Wrist position in each default hand's grip frame (HandRig), and the
    // last grip rotation the hand was placed at: their product is the
    // forearm's direction when no body drives the arm.
    std::array<Vec3, 2> hand_wrist_in_grip_{Vec3(0.0f), Vec3(0.0f)};
    std::array<Quat, 2> last_grip_rot_{Quat(1, 0, 0, 0), Quat(1, 0, 0, 0)};
    Vec3 hand_forearm_direction(int hand) const;
    // Grip frame -> hand entity frame, measured per hand mesh.
    std::array<Mat4, 2> hand_grip_to_entity_{Mat4(1.0f), Mat4(1.0f)};
    // Horizontal head-facing direction in **stage / world XZ** (Y=0 plane).
    //
    // CRITICAL: this MUST default to the zero vector so that until we have
    // observed a real HMD pose, *no* slide direction is available and the
    // player simply does not move. A non-zero default like (0, 0, -1)
    // causes "world-based movement": pushing the joystick in head-relative
    // mode would slide the player in fixed world -Z regardless of where
    // they're physically looking. The slide path explicitly skips motion
    // when this length is zero (`glm::length(last_head_forward_xz_) <= 1e-4f`).
    //
    // Updated every frame from ``InputState::head`` *before* locomotion
    // math runs, so head-relative slide always uses the latest HMD yaw.
    Vec3 last_head_forward_xz_{0.0f, 0.0f, 0.0f};
    // Latest cached HMD eye world position (full XYZ). Updated inside the
       // input/locomotion block each frame. Used to anchor the locomotion menu
    // at a comfortable eye-relative height (Meta Horizon "Head -> Comfort"
    // guidance: place primary UI ~1.5m forward, slightly below eye level).
    Vec3 last_head_world_pos_{0.0f, 0.0f, 0.0f};
    bool last_head_pose_valid_{false};

    // NOTE: the slide comfort ramp used to live here. It now belongs to the
    // slide mechanics (see mechanics/builtin/slide/SlideComfort.h), one ramp
    // per mechanic instance.

    // Locomotion tunables (loaded from scene.json ``player`` block; see
    // docs/locomotion.md). Defaults match Meta's comfort recommendations.
    float slide_max_speed_mps_{1.5f};
    float slide_accel_tau_s_{0.12f};
    float slide_decel_tau_s_{0.18f};
    float sprint_speed_mps_{3.0f};
    float sprint_accel_tau_s_{0.35f};
    float sprint_decel_tau_s_{0.25f};
    float sprint_forward_cone_deg_{70.0f};
    float sprint_min_stick_{0.6f};
    ::ptv::SprintActivation sprint_activation_{::ptv::SprintActivation::Hold};
    // Published by the slide each frame it owns the stick.
    bool sprinting_{false};
    float gait_speed_cap_{1.5f};
    float teleport_launch_speed_mps_{6.0f};
    float teleport_max_horizontal_m_{12.0f};
    float teleport_floor_y_{0.0f};
    float snap_turn_degrees_{45.0f};
    float snap_turn_cooldown_s_{0.30f};

    // Services handed to mechanics each frame, plus the context that bundles
    // them. Created in initialize(), valid for the lifetime of the scene.
    std::unique_ptr<LocomotionService> locomotion_svc_;
    std::unique_ptr<PhysicsAccessService> physics_svc_;
    std::unique_ptr<ContactsService> contacts_svc_;
    std::unique_ptr<SpawnService> spawn_svc_;
    // Gear entities cloned onto a spawned instance (a goblin's club): they
    // are destroyed with that instance.
    std::unordered_set<std::uint32_t> clone_children_;
    // The scene's NPC waves, written by builtin.encounter_director.
    EncounterBoard encounter_board_;
    std::unique_ptr<AttachService> attach_svc_;
    std::unique_ptr<CombatService> combat_svc_;
    std::unique_ptr<PerceptionService> perception_svc_;
    std::unique_ptr<NavigationService> nav_svc_;
    std::unique_ptr<DebugDrawService> draw_svc_;
    std::unique_ptr<HandsService> hands_svc_;
    std::unique_ptr<TransformFxService> fx_svc_;
    std::unique_ptr<HapticsService> haptics_svc_;
    std::unique_ptr<SocketsService> sockets_svc_;
    std::unique_ptr<SkeletonService> skeleton_svc_;
    std::unique_ptr<EquipService> equip_svc_;
    std::unique_ptr<InteractiveService> interactive_svc_;
    std::unique_ptr<PersistenceService> persistence_svc_;
    std::unique_ptr<TransitionService> transition_svc_;
    std::unique_ptr<UiService> ui_svc_;
    std::unique_ptr<ComfortService> comfort_svc_;
    std::unique_ptr<AppService> app_svc_;
    std::unique_ptr<TextDrawService> text_svc_;
    // Weapons and effects (src/gameplay): ctx.vfx, ctx.projectiles, ctx.weapons.
    std::unique_ptr<EffectsSystem> effects_;
    std::unique_ptr<ProjectileSystem> projectiles_;
    std::unique_ptr<WeaponBoard> weapons_;
    IHands* weapons_hands_{nullptr};
    void configure_weapon_systems();
    MechanicContext mech_ctx_{};

    // Comfort vignette state, per ComfortChannel.
    struct VignetteChannel {
        bool offered{false};
        bool toggleable{false};
        bool enabled{false};
    };
    std::array<VignetteChannel, 2> vignette_channels_{};
    // Strongest request this frame, and the smoothed value actually drawn.
    float vignette_request_{0.0f};
    float vignette_request_inner_deg_{55.0f};
    float vignette_request_feather_deg_{25.0f};
    float vignette_level_{0.0f};
    float vignette_inner_deg_{55.0f};
    float vignette_feather_deg_{25.0f};
    float vignette_since_request_s_{10.0f};
    /** Smooth this frame's vignette requests into what the renderer draws. */
    void tick_comfort_vignette(float dt);
    // Monotonic seconds since the loop started; the haptics rate limiter
    // needs a clock that does not depend on wall-clock formatting.
    double haptic_clock_s_{0.0};
    // One-frame draw list produced by mechanics through IDebugDraw; appended
    // to the render item list and cleared every frame.
    std::vector<Renderer::DrawItem> mech_draw_items_;

    // Adaptive quality: trades LOD / cull distance / foveation for frame rate
    // when the measured frame time misses the headset's target.
    QualityGovernor governor_{};
    // Memory protection (Quest 3 PSS), separate from the frame-rate governor
    // and always on.
    MemoryGuard memory_guard_{};
    int logged_evictions_{0};
    float applied_pressure_scale_{1.0f};
    // Telemetry accumulators; flushed to the log about once a second so
    // ``adb logcat -s ptv`` shows real on-device performance.
    float telemetry_accum_s_{0.0f};
    // Sum of per-frame WORK time (excludes the xrWaitFrame block, which the
    // runtime controls) -- this is what the governor reacts to.
    float telemetry_work_accum_s_{0.0f};
    int telemetry_frames_{0};
    float telemetry_worst_frame_ms_{0.0f};
    float telemetry_cpu_accum_ms_{0.0f};
    float telemetry_gpu_accum_ms_{0.0f};
    float telemetry_gpu_worst_ms_{0.0f};

    // --- NPC telemetry + on-device verification --------------------------
    // One ``npc_manifest`` line per NPC at scene load and one ``npc`` status
    // line per NPC per second, so ``adb logcat -s ptv`` shows whether each
    // character exists, stands on the floor, moves and animates. The
    // quest_runtime_verify stage parses exactly these lines.
    float npc_telemetry_accum_s_{0.0f};
    double npc_telemetry_clock_s_{0.0};
    bool npc_manifest_logged_{false};
    // Position at the previous status line, per NPC, for its measured speed.
    std::unordered_map<std::uint32_t, Vec3> npc_last_pos_{};
    void log_npc_manifest();
    void tick_npc_telemetry(float dt);
    // Verification mode (Android system property ``debug.ptv.verify`` = 1,
    // or env ``PTV_VERIFY`` = 1 on desktop): the player walks a scripted loop
    // so followers and chasers have something to react to, and cannot die,
    // so one run watches a whole encounter. Hits are still recorded.
    bool verify_mode_{false};
    // Weapons pass (``debug.ptv.verify_weapons`` = 1): the player stands
    // still and a scripted hand holds and fires every weapon in turn.
    bool verify_weapons_{false};
    std::unique_ptr<WeaponVerifier> weapon_verifier_;
    // debug.ptv.verify_textures: the on-headset texture gallery.
    bool verify_textures_{false};
    std::unique_ptr<TextureGallery> texture_gallery_;
    // Verification only: an enemy that landed a hit on the (protected)
    // player is defeated shortly after, so the waves keep advancing and every
    // NPC type in the plan actually appears during one unattended run.
    std::vector<std::pair<std::uint32_t, float>> verify_defeats_{};
    float verify_time_s_{0.0f};
    Vec3 verify_origin_{0.0f};
    bool verify_origin_set_{false};
    void read_verify_mode();
    /** This frame's scripted walk, added to the locomotion request. */
    void tick_verify_autopilot(float dt);
};

}  // namespace ptv
